// SPDX-License-Identifier: GPL-2.0+
/*
 * hwmon driver for NZXT Grid+ V3 and Smart Device (V1)
 *
 * The device asynchronously sends HID reports five times a second to
 * communicate fan speed, current, voltage and control mode.  It does not
 * respond to Get_Report or honor Set_Idle requests for this status report.
 *
 * Fan speeds can be controlled through output HID reports, but duty cycles
 * cannot be read back from the device.
 *
 * A special initialization routine causes the device to detect the fan
 * channels in use and their appropriate control mode (DC or PWM); once
 * requested, the initialization routine is executed asynchronously by the
 * device.
 *
 * Before initialization:
 * - all fans default to 40% PWM;
 * - PWM value changes are sometimes honored, other times ignored;
 * - the device does not send status reports.
 *
 * After initialization:
 * - the device sends status reports five times a second;
 * - for channels in use, the control mode has been detected and PWM changes
 *   are honored;
 * - for channels that were not detected as in use, fan speeds, current and
 *   voltage are still measured, and PWM changes are still accepted even though
 *   they have no immediate effect.
 *
 * Control mode and PWM settings only persist as long as the USB device is
 * connected and powered on.
 *
 * Copyright 2019-2021  Jonas Malaco <jonas@protocubo.io>
 */

#include <asm/unaligned.h>
#include <linux/bitops.h>
#include <linux/errno.h>
#include <linux/hid.h>
#include <linux/hwmon.h>
#include <linux/jiffies.h>
#include <linux/minmax.h>
#include <linux/module.h>
#include <linux/mutex.h>

#define VID_NZXT		0x1e71
#define PID_GRIDPLUS3		0x1711
#define PID_SMARTDEVICE		0x1714

#define REPORT_REQ_INIT		0x01
#define REQ_INIT_DETECT		0x5c
#define REQ_INIT_OPEN		0x5d

#define REPORT_STATUS		0x04
#define STATUS_VALIDITY		3 /* seconds */

#define REPORT_CONFIG		0x02
#define CONFIG_FAN_PWM		0x4d

#define DC_FAN			BIT(0)
#define PWM_FAN			BIT(1)

/**
 * struct grid3_channel_status - Last known data for a given channel.
 * @rpms:	Fan speed in rpm.
 * @centiamps:	Fan current draw in centiamperes.
 * @centivolts:	Fan supply voltage in centivolts.
 * @pwm:	Fan PWM value (last set value, device does not report it).
 * @fan_type:	Fan type (no fan, DC, PWM).
 * @updated:	Last update in jiffies.
 *
 * Centiamperes and centivolts are used to save some space.
 */
struct grid3_channel_status {
	u16 rpms;
	u16 centiamps;
	u16 centivolts;
	u8 pwm;
	u8 fan_type;
	unsigned long updated;
};

/**
 * struct grid3_data - Driver private data.
 * @hid_dev:	HID device.
 * @hwmon_dev:	HWMON device.
 * @lock:	Protects the output buffer @out and writes to @status[].pwm.
 * @out:	DMA-safe output buffer for HID output reports.
 * @channels:	Number of channels.
 * @status:	Last known status for each channel.
 */
struct grid3_data {
	struct hid_device *hid_dev;
	struct device *hwmon_dev;

	struct mutex lock; /* see comment above */
	u8 out[8];

	int channels;
	struct grid3_channel_status status[];
};

static umode_t grid3_is_visible(const void *data, enum hwmon_sensor_types type,
				u32 attr, int channel)
{
	const struct grid3_data *priv = data;

	if (channel >= priv->channels)
		return 0;

	switch (type) {
	case hwmon_fan:
	case hwmon_curr:
	case hwmon_in:
		return 0444;
	case hwmon_pwm:
		switch (attr) {
		case hwmon_pwm_input:
			return 0644;
		case hwmon_pwm_mode:
			return 0444;
		default:
			return 0;
		}
	default:
		return 0;
	}
}

static int grid3_read_pwm(struct grid3_data *priv, u32 attr, int channel, long *val)
{
	switch (attr) {
	case hwmon_pwm_input:
		*val = priv->status[channel].pwm;
		break;
	case hwmon_pwm_mode:
		/*
		 * For fan control, the device treats undetected == PWM.
		 */
		*val = priv->status[channel].fan_type != DC_FAN;
		break;
	default:
		return -EOPNOTSUPP;
	}

	return 0;
}

static int grid3_read(struct device *dev, enum hwmon_sensor_types type, u32 attr,
		      int channel, long *val)
{
	struct grid3_data *priv = dev_get_drvdata(dev);
	unsigned long expires;

	expires = priv->status[channel].updated + STATUS_VALIDITY * HZ;
	if (time_after(jiffies, expires))
		return -ENODATA;

	switch (type) {
	case hwmon_fan:
		*val = priv->status[channel].rpms;
		break;
	case hwmon_curr:
		*val = priv->status[channel].centiamps * 10;
		break;
	case hwmon_in:
		*val = priv->status[channel].centivolts * 10;
		break;
	case hwmon_pwm:
		return grid3_read_pwm(priv, attr, channel, val);
	default:
		return -EOPNOTSUPP;
	}

	return 0;
}

/*
 * Caller must hold priv->lock or otherwise ensure exclusive access to
 * priv->out and priv->status[*].pwm.
 */
static int grid3_write_pwm_assume_locked(struct grid3_data *priv, int channel, long val)
{
	int ret;

	val = clamp_val(val, 0, 255);

	priv->out[0] = REPORT_CONFIG;
	priv->out[1] = CONFIG_FAN_PWM;
	priv->out[2] = channel;
	priv->out[3] = 0x00;
	priv->out[4] = val * 100 / 255;

	ret = hid_hw_output_report(priv->hid_dev, priv->out, 5);
	if (ret < 0)
		return ret;
	if (ret != 5)
		return -EIO; /* FIXME */

	/*
	 * Store the value that was just set; the device does not support
	 * reading it later, but user-space needs it.
	 */
	priv->status[channel].pwm = val;

	return 0;
}

static int grid3_write_pwm_input(struct device *dev, enum hwmon_sensor_types type,
				 u32 attr, int channel, long val)
{
	struct grid3_data *priv = dev_get_drvdata(dev);
	int ret;

	if (mutex_lock_interruptible(&priv->lock))
		return -ERESTARTSYS;

	ret = grid3_write_pwm_assume_locked(priv, channel, val);

	mutex_unlock(&priv->lock);
	return ret;
}

static const struct hwmon_ops grid3_hwmon_ops = {
	.is_visible = grid3_is_visible,
	.read = grid3_read,
	.write = grid3_write_pwm_input,
};

static const struct hwmon_channel_info *grid3_info[] = {
	HWMON_CHANNEL_INFO(fan,
			   HWMON_F_INPUT,
			   HWMON_F_INPUT,
			   HWMON_F_INPUT,
			   HWMON_F_INPUT,
			   HWMON_F_INPUT,
			   HWMON_F_INPUT),
	HWMON_CHANNEL_INFO(curr,
			   HWMON_C_INPUT,
			   HWMON_C_INPUT,
			   HWMON_C_INPUT,
			   HWMON_C_INPUT,
			   HWMON_C_INPUT,
			   HWMON_C_INPUT),
	HWMON_CHANNEL_INFO(in,
			   HWMON_I_INPUT,
			   HWMON_I_INPUT,
			   HWMON_I_INPUT,
			   HWMON_I_INPUT,
			   HWMON_I_INPUT,
			   HWMON_I_INPUT),
	HWMON_CHANNEL_INFO(pwm,
			   HWMON_PWM_INPUT | HWMON_PWM_MODE,
			   HWMON_PWM_INPUT | HWMON_PWM_MODE,
			   HWMON_PWM_INPUT | HWMON_PWM_MODE,
			   HWMON_PWM_INPUT | HWMON_PWM_MODE,
			   HWMON_PWM_INPUT | HWMON_PWM_MODE,
			   HWMON_PWM_INPUT | HWMON_PWM_MODE),
	NULL
};

static const struct hwmon_chip_info grid3_chip_info = {
	.ops = &grid3_hwmon_ops,
	.info = grid3_info,
};

static int grid3_raw_event(struct hid_device *hdev, struct hid_report *report,
			   u8 *data, int size)
{
	struct grid3_data *priv;
	int channel;

	if (size < 16 || report->id != REPORT_STATUS)
		return 0;

	priv = hid_get_drvdata(hdev);

	channel = data[15] >> 4;

	if (channel > priv->channels)
		return 0;

	priv->status[channel].rpms = get_unaligned_be16(data + 3);
	priv->status[channel].centiamps = data[9] * 100 + data[10];
	priv->status[channel].centivolts = data[7] * 100 + data[8];
	priv->status[channel].fan_type = data[15] & 0x3;

	priv->status[channel].updated = jiffies;

	return 0;
}

/*
 * Caller must hold priv->lock or otherwise ensure exclusive access to
 * priv->out.
 */
static int grid3_req_init_assume_locked(struct hid_device *hdev, u8 *buf)
{
	u8 cmds[2] = {REQ_INIT_DETECT, REQ_INIT_OPEN};
	int i, ret;

	buf[0] = REPORT_REQ_INIT;

	for (i = 0; i < ARRAY_SIZE(cmds); i++) {
		buf[1] = cmds[i];
		ret = hid_hw_output_report(hdev, buf, 2);
		if (ret < 0)
			return ret;
		if (ret != 2)
			return -EIO; /* FIXME */
	}

	return 0;
}

/*
 * Caller must hold priv->lock or otherwise ensure exclusive access to
 * priv->out and priv->status[*].pwm.
 */
static int grid3_driver_init_assume_locked(struct grid3_data *priv)
{
	int i, ret;

	ret = grid3_req_init_assume_locked(priv->hid_dev, priv->out);
	if (ret) {
		hid_err(priv->hid_dev, "request init failed with %d\n", ret);
		return ret;
	}

	for (i = 0; i < priv->channels; i++) {
		/*
		 * Initialize ->updated to STATUS_VALIDITY seconds in the past,
		 * making the initial empty data invalid for grid3_read
		 * without the need for a special case there.
		 */
		priv->status[i].updated = jiffies - STATUS_VALIDITY * HZ;

		/*
		 * Mimic the behavior of the device after being powered on,
		 * ensuring predictable behavior if the driver has been
		 * previously removed.
		 */
		ret = grid3_write_pwm_assume_locked(priv, i, 40 * 255 / 100);
		if (ret) {
			hid_err(priv->hid_dev, "write pwm failed with %d\n", ret);
			return ret;
		}
	}
	return 0;
}

static int __maybe_unused grid3_reset_resume(struct hid_device *hdev)
{
	struct grid3_data *priv = hid_get_drvdata(hdev);
	int ret;

	mutex_lock(&priv->lock);
	ret = grid3_driver_init_assume_locked(priv);
	mutex_unlock(&priv->lock);

	if (ret)
		hid_err(hdev, "req init (reset_resume) failed with %d\n", ret);

	return ret;
}

static int grid3_probe(struct hid_device *hdev, const struct hid_device_id *id)
{
	struct grid3_data *priv;
	char *hwmon_name;
	int channels, ret;

	switch (id->product) {
	case PID_GRIDPLUS3:
		channels = 6;
		hwmon_name = "gridplus3";
		break;
	case PID_SMARTDEVICE:
		channels = 3;
		hwmon_name = "smartdevice";
		break;
	default:
		return -EINVAL; /* unreachable */
	}

	priv = devm_kzalloc(&hdev->dev, struct_size(priv, status, channels), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->hid_dev = hdev;
	priv->channels = channels;
	mutex_init(&priv->lock);

	hid_set_drvdata(hdev, priv);

	ret = hid_parse(hdev);
	if (ret) {
		hid_err(hdev, "hid parse failed with %d\n", ret);
		goto fail_mutex_destroy;
	}

	/*
	 * Enable hidraw so existing user-space tools can continue to work.
	 */
	ret = hid_hw_start(hdev, HID_CONNECT_HIDRAW);
	if (ret) {
		hid_err(hdev, "hid hw start failed with %d\n", ret);
		goto fail_mutex_destroy;
	}

	ret = hid_hw_open(hdev);
	if (ret) {
		hid_err(hdev, "hid hw open failed with %d\n", ret);
		goto fail_hid_stop;
	}

	/*
	 * Concurrent access to priv->out or priv->status[*].pwm is not
	 * possible yet.
	 */
	ret = grid3_driver_init_assume_locked(priv);
	if (ret) {
		hid_err(hdev, "driver init failed with %d\n", ret);
		goto fail_hid_close;
	}

	priv->hwmon_dev = hwmon_device_register_with_info(&hdev->dev, hwmon_name,
							  priv, &grid3_chip_info,
							  NULL);
	if (IS_ERR(priv->hwmon_dev)) {
		ret = PTR_ERR(priv->hwmon_dev);
		hid_err(hdev, "hwmon registration failed with %d\n", ret);
		goto fail_hid_close;
	}

	return 0;

fail_hid_close:
	hid_hw_close(hdev);
fail_hid_stop:
	hid_hw_stop(hdev);
fail_mutex_destroy:
	mutex_destroy(&priv->lock);
	return ret;
}

static void grid3_remove(struct hid_device *hdev)
{
	struct grid3_data *priv = hid_get_drvdata(hdev);

	hwmon_device_unregister(priv->hwmon_dev);

	hid_hw_close(hdev);
	hid_hw_stop(hdev);

	mutex_destroy(&priv->lock);
}

static const struct hid_device_id grid3_table[] = {
	{ HID_USB_DEVICE(VID_NZXT, PID_GRIDPLUS3) },
	{ HID_USB_DEVICE(VID_NZXT, PID_SMARTDEVICE) },
	{ }
};

MODULE_DEVICE_TABLE(hid, grid3_table);

static struct hid_driver grid3_driver = {
	.name = "nzxt-grid3",
	.id_table = grid3_table,
	.probe = grid3_probe,
	.remove = grid3_remove,
	.raw_event = grid3_raw_event,
#ifdef CONFIG_PM
	.reset_resume = grid3_reset_resume,
#endif
};

static int __init grid3_init(void)
{
	return hid_register_driver(&grid3_driver);
}

static void __exit grid3_exit(void)
{
	hid_unregister_driver(&grid3_driver);
}

/*
 * When compiled into the kernel, initialize after the hid bus.
 */
late_initcall(grid3_init);
module_exit(grid3_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Jonas Malaco <jonas@protocubo.io>");
MODULE_DESCRIPTION("Hwmon driver for NZXT Smart Device (V1) and Grid+ V3");
