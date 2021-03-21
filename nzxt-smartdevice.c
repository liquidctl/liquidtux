// SPDX-License-Identifier: GPL-2.0+
/*
 * nzxt-smartdevice.c - hwmon driver for NZXT Smart Device (V1) and Grid+ V3
 *
 * The device asynchronously sends HID reports (with id 0x???) five times a
 * second to communicate current fan speed, current, voltage and control mode.
 * The device does not respond to Get_Report requests for this status report.
 * TODO check
 *
 * During probe, the device is requested to run its initialization routine,
 * where the connected fans and their appropriate control modes are detected.
 * A channel's control mode does not change after the initialization routine
 * finishes and the fan type has been recognized.
 * TODO can we detect uninitialized devices?
 *
 * Fan speeds can be controlled by sending HID reports with id 0x02, but duty
 * cycles cannot be read back from the device.
 *
 * TODO try to set the polling rate
 * TODO try Get_Report
 * TODO if both of them are available, try JIT reading
 * TODO try to dynamically change the polling rate depending on whether hidraw
 *      is in use
 * TODO try to only open the device when necessary
 *
 * Copyright 2019-2021  Jonas Malaco <jonas@protocubo.io>
 */

#include <asm/unaligned.h>
#include <linux/hid.h>
#include <linux/hwmon.h>
#include <linux/jiffies.h>
#include <linux/module.h>
#include <linux/mutex.h>

#define VID_NZXT		0x1e71
#define PID_GRIDPLUS3		0x1711
#define PID_SMARTDEVICE		0x1714

#define STATUS_REPORT_ID	0x04
#define STATUS_VALIDITY		3 /* seconds */

#define MAX_CHANNELS		6

struct smartdevice_priv_data {
	struct hid_device *hid_dev;
	struct device *hwmon_dev;

	u16 fan_input[MAX_CHANNELS];
	u16 curr_input[MAX_CHANNELS]; /* centiamperes */
	u16 in_input[MAX_CHANNELS]; /* centivolts */
	u8 pwm_mode[MAX_CHANNELS];
	u8 channel_count;

	struct mutex lock; /* protects the output buffer */
	u8 out[8]; /* output buffer */

	unsigned long updated; /* jiffies */
};

static umode_t smartdevice_is_visible(const void *data,
				      enum hwmon_sensor_types type,
				      u32 attr, int channel)
{
	const struct smartdevice_priv_data *priv = data;

	if (channel >= priv->channel_count)
		return 0;

	/* TODO should we hide unconnected fans? */

	switch (type) {
	case hwmon_fan:
	case hwmon_curr:
	case hwmon_in:
		return 0444;
	case hwmon_pwm:
		switch (attr) {
		case hwmon_pwm_input:
			return 0200;
		case hwmon_pwm_mode:
			return 0444;
		default:
			return 0; /* unreachable */
		}
	default:
		return 0; /* unreachable */
	}
}

static int smartdevice_read(struct device *dev, enum hwmon_sensor_types type,
			    u32 attr, int channel, long *val)
{
	struct smartdevice_priv_data *priv = dev_get_drvdata(dev);

	/* TODO should we validate each channel individually? */

	if (time_after(jiffies, priv->updated + STATUS_VALIDITY * HZ))
		return -ENODATA;

	switch (type) {
	case hwmon_fan:
		*val = priv->fan_input[channel];
		break;
	case hwmon_curr:
		*val = priv->curr_input[channel] * 10;
		break;
	case hwmon_in:
		*val = priv->in_input[channel] * 10;
		break;
	case hwmon_pwm:
		*val = priv->pwm_mode[channel];
		break;
	default:
		return -EOPNOTSUPP; /* unreachable */
	}

	return 0;
}

static int smartdevice_write(struct device *dev, enum hwmon_sensor_types type,
			     u32 attr, int channel, long val)
{
	struct smartdevice_priv_data *priv = dev_get_drvdata(dev);
	int ret;

	if (mutex_lock_interruptible(&priv->lock))
		return -EINTR;

	priv->out[0] = 0x02;
	priv->out[1] = 0x4d;
	priv->out[2] = channel;
	priv->out[3] = 0x00;
	priv->out[4] = val * 100 / 255;

	ret = hid_hw_output_report(priv->hid_dev, priv->out, 5);
	mutex_unlock(&priv->lock);

	return ret == 5 ? 0 : ret;
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
			   HWMON_PWM_INPUT | HWMON_PWM_MODE,
			   HWMON_PWM_INPUT | HWMON_PWM_MODE,
			   HWMON_PWM_INPUT | HWMON_PWM_MODE,
			   HWMON_PWM_INPUT | HWMON_PWM_MODE,
			   HWMON_PWM_INPUT | HWMON_PWM_MODE,
			   HWMON_PWM_INPUT | HWMON_PWM_MODE),
	NULL
};

static const struct hwmon_chip_info smartdevice_chip_info = {
	.ops = &smartdevice_hwmon_ops,
	.info = smartdevice_info,
};

static int smartdevice_req_init(struct hid_device *hdev, u8 *buf)
{
	int cmd, ret;

	buf[0] = 0x01;

	for (cmd = 0x5c; cmd < 0x5e; cmd++) {
		buf[1] = cmd;
		ret = hid_hw_output_report(hdev, buf, 2);
		if (ret < 0)
			return ret;
		if (ret != 2)
			return -EPIPE;
	}

	return 0;
}

static int smartdevice_raw_event(struct hid_device *hdev,
				 struct hid_report *report, u8 *data, int size)
{
	struct smartdevice_priv_data *priv;
	int channel;

	if (size < 16 || report->id != STATUS_REPORT_ID)
		return 0;

	priv = hid_get_drvdata(hdev);

	channel = data[15] >> 4;
	if (channel > priv->channel_count)
		return 0;

	/* TODO are data for initialized undetected fans still reported? */

	/*
	 * Keep current/voltage in centiamperes/volts to save some space.
	 */

	priv->fan_input[channel] = get_unaligned_be16(data + 3);
	priv->curr_input[channel] = data[9] * 100 + data[10];
	priv->in_input[channel] = data[7] * 100 + data[8];
	priv->pwm_mode[channel] = !(data[15] & 0x1);

	priv->updated = jiffies;

	return 0;
}

static int smartdevice_reset_resume(struct hid_device *hdev)
{
	struct smartdevice_priv_data *priv = hid_get_drvdata(hdev);
	int ret;

	mutex_lock(&priv->lock);
	ret = smartdevice_req_init(hdev, priv->out);
	mutex_unlock(&priv->lock);

	hid_info(hdev, "(reset_resume) req_init returned %d\n", ret);

	return ret;
}

static int smartdevice_resume(struct hid_device *hdev)
{
	hid_info(hdev, "(resume) forwarding call to reset_resume for testing purposes");
	return smartdevice_reset_resume(hdev);
}

static int smartdevice_probe(struct hid_device *hdev,
			     const struct hid_device_id *id)
{
	struct smartdevice_priv_data *priv;
	char *hwmon_name;
	int ret;

	priv = devm_kzalloc(&hdev->dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->hid_dev = hdev;
	mutex_init(&priv->lock);
	hid_set_drvdata(hdev, priv);

	switch (id->product) {
	case PID_GRIDPLUS3:
		priv->channel_count = 6;
		hwmon_name = "gridplus3";
		break;
	case PID_SMARTDEVICE:
		priv->channel_count = 3;
		hwmon_name = "smartdevice";
		break;
	default:
		return -EINVAL; /* unreachable */
	}

	/*
	 * Initialize ->updated to STATUS_VALIDITY seconds in the past, making
	 * the initial empty data invalid for kraken2_read without the need for
	 * a special case there.
	 */
	priv->updated = jiffies - STATUS_VALIDITY * HZ;

	ret = hid_parse(hdev);
	if (ret) {
		hid_err(hdev, "hid parse failed with %d\n", ret);
		return ret;
	}

	/*
	 * Enable hidraw so existing user-space tools can continue to work.
	 */
	ret = hid_hw_start(hdev, HID_CONNECT_HIDRAW);
	if (ret) {
		hid_err(hdev, "hid hw start failed with %d\n", ret);
		goto fail_and_stop;
	}

	ret = hid_hw_open(hdev);
	if (ret) {
		hid_err(hdev, "hid hw open failed with %d\n", ret);
		goto fail_and_close;
	}

	/*
	 * Use priv->out without taking priv->lock because no one else has
	 * access to it yet.
	 */
	ret = smartdevice_req_init(hdev, priv->out);
	if (ret) {
		hid_err(hdev, "request device init failed with %d\n", ret);
		goto fail_and_close;
	}

	priv->hwmon_dev = hwmon_device_register_with_info(&hdev->dev, hwmon_name,
							  priv, &smartdevice_chip_info,
							  NULL);
	if (IS_ERR(priv->hwmon_dev)) {
		ret = PTR_ERR(priv->hwmon_dev);
		hid_err(hdev, "hwmon registration failed with %d\n", ret);
		goto fail_and_close;
	}

	return 0;

fail_and_close:
	hid_hw_close(hdev);
fail_and_stop:
	hid_hw_stop(hdev);
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
	.resume = smartdevice_resume,
	.reset_resume = smartdevice_reset_resume,
};

static int __init smartdevice_init(void)
{
	pr_debug("smartdevice_priv_data size: %ld\n",
		 sizeof(struct smartdevice_priv_data));
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
