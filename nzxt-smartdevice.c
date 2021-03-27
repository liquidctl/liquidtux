// SPDX-License-Identifier: GPL-2.0+
/*
 * nzxt-smartdevice.c - hwmon driver for NZXT Smart Device (V1) and Grid+ V3
 *
 * The device asynchronously sends HID reports five times a second to
 * communicate fan speed, current, voltage and control mode.  It does not
 * respond to Get_Report or honor Set_Idle requests for this status report.
 *
 * Fan speeds can be controlled through output HID reports, but duty cycles
 * cannot be read back from the device.  The Smart Device has three fan
 * channels, while the Grid+ V3 has six.
 *
 * A special initialization routine causes the device to detect the fan
 * channels in use and their appropriate control mode (DC or PWM); once
 * requested, the initialization routine is executed asynchronously by the
 * device.
 *
 * Before initialization:
 * - the device does not send status reports;
 *  - all fans default to 40% PWM.
 *
 * After initialization:
 * - device sends status reports;
 * - for channels in use, the control mode has been detected and PWM changes
 *   are honored;
 * - for channels that were not detected as in use, fan speeds, current and
 *   voltage are still measured, but control is disabled and PWM changes are
 *   ignored.
 *
 * Control mode and PWM settings only persist as long as the USB device is
 * connected and powered on.
 *
 * TODO check pwmconfig + fancontrol + other daemons
 * TODO report IDs are indeed used?
 *
 * Copyright 2019-2021  Jonas Malaco <jonas@protocubo.io>
 */

#include <asm/unaligned.h>
#include <linux/hid.h>
#include <linux/hwmon.h>
#include <linux/jiffies.h>
#include <linux/minmax.h>
#include <linux/module.h>
#include <linux/mutex.h>

#define VID_NZXT		0x1e71
#define PID_GRIDPLUS3		0x1711
#define PID_SMARTDEVICE		0x1714

#define MAX_CHANNELS		6

#define REPORT_REQ_INIT 	0x01
#define REQ_INIT_DETECT		0x5c
#define REQ_INIT_OPEN		0x5d

#define REPORT_STATUS		0x04
#define STATUS_VALIDITY		3 /* seconds */

#define REPORT_CONFIG		0x02
#define CONFIG_FAN_PWM		0x4d

/* FIXME remove, only used for testing */
#include <linux/moduleparam.h>
static bool do_not_init = false;
module_param(do_not_init, bool, 0);
MODULE_PARM_DESC(do_not_init, "do not request device initialization");

/**
 * smartdevice_fan_mode - Device fan control mode.
 * @smartdevice_no_control:	Control is disabled, typically means no fan has
 * 				been detected on the channel.
 * @smartdevice_dc_control:	Control is enabled, mode is DC.
 * @smartdevice_pwm_control:	Control is enabled, mode is PWM.
 */
enum __packed smartdevice_fan_mode {
	smartdevice_no_control,
	smartdevice_dc_control,
	smartdevice_pwm_control,
};

/**
 * smartdevice_channel_data - Last known status for a given channel.
 * @rpms:	Fan speed in rpm.
 * @centiamps:	Fan current draw in centiamperes.
 * @centivolts:	Fan supply voltage in centivolts.
 * @pwm:	Fan PWM value (last set value, device does not report it).
 * @mode:	Fan control mode (no, DC, PWM).
 * @updated:	Last update in jiffies.
 *
 * Current and voltage are stored in centiamperes and centivolts to save some
 * space.
 *
 * All fields are marked as volatile to inhibit optimizations to loads/stores,
 * since those happen concurrently in process and interrupt contexts.
 */
struct smartdevice_channel_data {
	volatile u16 rpms;
	volatile u16 centiamps;
	volatile u16 centivolts;
	volatile u8 pwm;
	volatile enum smartdevice_fan_mode mode;
	volatile unsigned long updated;
};

/**
 * smartdevice_priv_data - Driver private data.
 * @hid_dev:	HID device.
 * @hwmon_dev:	HWMON device.
 * @lock:	protects the output buffer @out and writes to @status
 * 	  	from process context.
 * @out:	DMA-safe output buffer.
 * @cha_cnt:	Number of channels.
 * @status:	Last known status for each channel.
 */
struct smartdevice_priv_data {
	struct hid_device *hid_dev;
	struct device *hwmon_dev;

	struct mutex lock;
	u8 out[8];

	int cha_cnt;
	struct smartdevice_channel_data status[];
};

static umode_t smartdevice_is_visible(const void *data,
				      enum hwmon_sensor_types type,
				      u32 attr, int channel)
{
	const struct smartdevice_priv_data *priv = data;

	if (channel >= priv->cha_cnt)
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
		case hwmon_pwm_enable: /* FIXME remove */
			return 0;
		case hwmon_pwm_mode:
			return 0444;
		default:
			return 0; /* unreachable */
		}
	default:
		return 0; /* unreachable */
	}
}

static int smartdevice_read_pwm(struct smartdevice_channel_data *channel,
				 u32 attr, long *val)
{
	switch (attr) {
		case hwmon_pwm_input:
			*val = channel->pwm;
			break;
		case hwmon_pwm_enable: /* FIXME remove */
			switch (channel->mode) {
				case smartdevice_no_control:
					*val = 0;
					break;
				default:
					*val = 1;
					break;
			}
			break;
		case hwmon_pwm_mode:
			switch (channel->mode) {
				case smartdevice_no_control:
					return -ENODATA;
				case smartdevice_dc_control:
					*val = 0;
					break;
				case smartdevice_pwm_control:
					*val = 1;
					break;
			}
			break;
		default:
			return -EOPNOTSUPP; /* unreachable */
	}

	return 0;
}

static int smartdevice_read(struct device *dev, enum hwmon_sensor_types type,
			    u32 attr, int channel, long *val)
{
	struct smartdevice_priv_data *priv = dev_get_drvdata(dev);

	/* FIXME formatting */
	if (time_after(jiffies, priv->status[channel].updated + STATUS_VALIDITY * HZ))
		return -ENODATA;

	smp_rmb(); /* order reading .updated before attributes */

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
		return smartdevice_read_pwm(&priv->status[channel], attr, val);
	default:
		return -EOPNOTSUPP; /* unreachable */
	}

	return 0;
}

static int smartdevice_write_pwm_unlocked(struct smartdevice_priv_data *priv,
					  int channel, long val)
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
		return -EPIPE; /* FIXME */

	/*
	 * Store the value that was just set; the device does not support
	 * reading it later, but user-space needs it.
	 */
	priv->status[channel].pwm = val;

	return 0;
}

static int smartdevice_write(struct device *dev, enum hwmon_sensor_types type,
			     u32 attr, int channel, long val)
{
	struct smartdevice_priv_data *priv = dev_get_drvdata(dev);
	int ret;

	/* FIXME remove, probably */
	if (attr == hwmon_pwm_enable)
		return 0;

	/*
	 * The device does not honor changes to the duty cycle of a fan channel
	 * it thinks is unconnected.
	 */
	if (priv->status[channel].mode == smartdevice_no_control)
		return -EOPNOTSUPP; /* FIXME */

	if (mutex_lock_interruptible(&priv->lock))
		return -EINTR;

	ret = smartdevice_write_pwm_unlocked(priv, channel, val);

	mutex_unlock(&priv->lock);

	return ret;
}

static const struct hwmon_ops smartdevice_hwmon_ops = {
	.is_visible = smartdevice_is_visible,
	.read = smartdevice_read,
	.write = smartdevice_write,
};

static const struct hwmon_channel_info *smartdevice_info[] = {
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
			   HWMON_PWM_INPUT | HWMON_PWM_ENABLE | HWMON_PWM_MODE,
			   HWMON_PWM_INPUT | HWMON_PWM_ENABLE | HWMON_PWM_MODE,
			   HWMON_PWM_INPUT | HWMON_PWM_ENABLE | HWMON_PWM_MODE,
			   HWMON_PWM_INPUT | HWMON_PWM_ENABLE | HWMON_PWM_MODE,
			   HWMON_PWM_INPUT | HWMON_PWM_ENABLE | HWMON_PWM_MODE,
			   HWMON_PWM_INPUT | HWMON_PWM_ENABLE | HWMON_PWM_MODE),
	NULL
};

static const struct hwmon_chip_info smartdevice_chip_info = {
	.ops = &smartdevice_hwmon_ops,
	.info = smartdevice_info,
};

static int smartdevice_raw_event(struct hid_device *hdev,
				 struct hid_report *report, u8 *data, int size)
{
	struct smartdevice_priv_data *priv;
	int channel;

	if (size < 16 || report->id != REPORT_STATUS)
		return 0;

	priv = hid_get_drvdata(hdev);

	channel = data[15] >> 4;
	if (channel > priv->cha_cnt)
		return 0;

	priv->status[channel].rpms = get_unaligned_be16(data + 3);
	priv->status[channel].centiamps = data[9] * 100 + data[10];
	priv->status[channel].centivolts = data[7] * 100 + data[8];
	priv->status[channel].mode = data[15] & 0x3;

	smp_wmb(); /* order writing .updated after attributes */

	priv->status[channel].updated = jiffies;

	return 0;
}

static int smartdevice_req_init(struct hid_device *hdev, u8 *buf)
{
	u8 cmds[2] = {REQ_INIT_DETECT, REQ_INIT_OPEN};
	int i, ret;

	if (do_not_init)
		return 0;

	buf[0] = REPORT_REQ_INIT;

	for (i = 0; i < ARRAY_SIZE(cmds); i++) {
		buf[1] = cmds[i];
		ret = hid_hw_output_report(hdev, buf, 2);
		if (ret < 0)
			return ret;
		if (ret != 2)
			return -EPIPE; /* FIXME */
	}

	return 0;
}

#ifdef CONFIG_PM
static int smartdevice_reset_resume(struct hid_device *hdev)
{
	struct smartdevice_priv_data *priv = hid_get_drvdata(hdev);
	int ret;

	/* FIXME remove */
	hid_info(hdev, "(reset_resume) requesting new initialization");

	mutex_lock(&priv->lock);
	ret = smartdevice_req_init(hdev, priv->out);
	mutex_unlock(&priv->lock);

	if (ret) {
		hid_err(hdev, "req init (reset_resume) failed with %d\n", ret);
	}

	return ret;
}

/* FIXME remove */
static int smartdevice_resume(struct hid_device *hdev)
{
	hid_info(hdev, "(resume) forwarding call to reset_resume for testing purposes");
	return smartdevice_reset_resume(hdev);
}
#endif

static int smartdevice_probe(struct hid_device *hdev,
			     const struct hid_device_id *id)
{
	struct smartdevice_priv_data *priv;
	char *hwmon_name;
	int i, cha_cnt, ret;

	switch (id->product) {
	case PID_GRIDPLUS3:
		cha_cnt = 6;
		hwmon_name = "gridplus3";
		break;
	case PID_SMARTDEVICE:
		cha_cnt = 3;
		hwmon_name = "smartdevice";
		break;
	default:
		return -EINVAL; /* unreachable */
	}

	/* FIXME remove */
	hid_info(hdev, "smartdevice_priv_data size: %ld bytes\n",
		 sizeof(*priv) + sizeof(*priv->status) * cha_cnt);
	hid_info(hdev, "with mutex size: %ld bytes\n", sizeof(struct mutex));

	priv = devm_kzalloc(&hdev->dev,
			    sizeof(*priv) + sizeof(*priv->status) * cha_cnt,
			    GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->hid_dev = hdev;
	priv->cha_cnt = cha_cnt;
	mutex_init(&priv->lock);

	hid_set_drvdata(hdev, priv);

	ret = hid_parse(hdev);
	if (ret) {
		hid_err(hdev, "hid parse failed with %d\n", ret);
		goto fail_but_destroy;
	}

	/*
	 * Enable hidraw so existing user-space tools can continue to work.
	 */
	ret = hid_hw_start(hdev, HID_CONNECT_HIDRAW);
	if (ret) {
		hid_err(hdev, "hid hw start failed with %d\n", ret);
		goto fail_but_stop;
	}

	ret = hid_hw_open(hdev);
	if (ret) {
		hid_err(hdev, "hid hw open failed with %d\n", ret);
		goto fail_but_close;
	}

	ret = smartdevice_req_init(hdev, priv->out);
	if (ret) {
		hid_err(hdev, "req init failed with %d\n", ret);
		goto fail_but_close;
	}

	for (i = 0; i < priv->cha_cnt; i++) {
		/*
		 * Initialize ->updated to STATUS_VALIDITY seconds in the past,
		 * making the initial empty data invalid for smartdevice_read
		 * without the need for a special case there.
		 */
		priv->status[i].updated = jiffies - STATUS_VALIDITY * HZ;

		/*
		 * Force 40% PWM: mimic the device upon true initialization and
		 * ensure predictable behavior if the driver has been
		 * previously unbound.
		 */
		ret = smartdevice_write_pwm_unlocked(priv, i, 40 * 255 / 100);
		if (ret) {
			hid_err(hdev, "write pwm failed with %d\n", ret);
			goto fail_but_close;
		}
	}

	priv->hwmon_dev = hwmon_device_register_with_info(&hdev->dev, hwmon_name,
							  priv, &smartdevice_chip_info,
							  NULL);
	if (IS_ERR(priv->hwmon_dev)) {
		ret = PTR_ERR(priv->hwmon_dev);
		hid_err(hdev, "hwmon registration failed with %d\n", ret);
		goto fail_but_close;
	}

	return 0;

fail_but_close:
	hid_hw_close(hdev);
fail_but_stop:
	hid_hw_stop(hdev);
fail_but_destroy:
	mutex_destroy(&priv->lock);
	return ret;
}

static void smartdevice_remove(struct hid_device *hdev)
{
	struct smartdevice_priv_data *priv = hid_get_drvdata(hdev);

	hwmon_device_unregister(priv->hwmon_dev);

	hid_hw_close(hdev);
	hid_hw_stop(hdev);

	mutex_destroy(&priv->lock);
}

static const struct hid_device_id smartdevice_table[] = {
	{ HID_USB_DEVICE(VID_NZXT, PID_GRIDPLUS3) },
	{ HID_USB_DEVICE(VID_NZXT, PID_SMARTDEVICE) },
	{ }
};

MODULE_DEVICE_TABLE(hid, smartdevice_table);

static struct hid_driver smartdevice_driver = {
	.name = "nzxt-smartdevice",
	.id_table = smartdevice_table,
	.probe = smartdevice_probe,
	.remove = smartdevice_remove,
	.raw_event = smartdevice_raw_event,
#ifdef CONFIG_PM
	.reset_resume = smartdevice_reset_resume,
	.resume = smartdevice_resume, /* FIXME remove */
#endif
};

static int __init smartdevice_init(void)
{
	return hid_register_driver(&smartdevice_driver);
}

static void __exit smartdevice_exit(void)
{
	hid_unregister_driver(&smartdevice_driver);
}

/*
 * When compiled into the kernel, initialize after the hid bus.
 */
late_initcall(smartdevice_init);
module_exit(smartdevice_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Jonas Malaco <jonas@protocubo.io>");
MODULE_DESCRIPTION("Hwmon driver for NZXT Smart Device (V1) and Grid+ V3");
