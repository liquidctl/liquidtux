// SPDX-License-Identifier: GPL-2.0+
/*
 * nzxt-smartdevice.c - hwmon driver for NZXT Smart Device (V1) and Grid+ V3
 *
 * The device asynchronously sends HID reports five times a second to
 * communicate current fan speed, current, voltage and control mode.  It does
 * not respond to Get_Report or honor Set_Idle requests for this status report.
 *
 * Fan speeds can be controlled though output HID reports, but duty cycles
 * cannot be read back from the device.
 *
 * A special initialization routine causes the device to detect the fan
 * status in use and their appropriate control mode (DC or PWM).  The
 * initialization routine, once requested, is executed asynchronously by the
 * device.
 *
 * Before initialization:
 * - device does not send status reports;
*  - all status set to a default of 40% PWM.
*
 * After initialization:
 * - device sends status reports;
 * - for status in use, the control mode detected and PWM changes are
 *   honored;
 * - for status detected as not in use, fan speeds, current and voltage are
 *   still measured, but control is disabled and PWM changes are ignored.
 *
 * Control mode and PWM settings only persist as long as the USB device is
 * connected and powered on.  The Smart Device has three fan status, while
 * the Grid+ V3 has six.
 *
 * TODO should we validate each channel individually?
 * TODO store pwm for later reads (it's better than fictitious values)
 * TODO optimize space
 * TODO initialize pwm to 40%
 * TODO use bitops
 * TODO are barriers necessary?
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

/* FIXME remove (probably) */
#include <linux/moduleparam.h>
static bool do_not_init = false;
module_param(do_not_init, bool, 0);
MODULE_PARM_DESC(do_not_init, "Do not request device initialization");

/*
 * Store current/voltage in centiamperes/centivolts to save some space.
 */
struct smartdevice_status_data {
	u16 rpms;
	u16 centiamps;
	u16 centivolts;
	u8 pwm; /* fake, simply the last set value */
	u8 device_mode;
	unsigned long updated; /* jiffies */
};

struct smartdevice_priv_data {
	struct hid_device *hid_dev;
	struct device *hwmon_dev;

	struct smartdevice_status_data status[MAX_CHANNELS];
	u8 channel_count;

	struct mutex lock; /* protects out and writes to status[*].pwm */
	u8 out[8]; /* output buffer */
};

static umode_t smartdevice_is_visible(const void *data,
				      enum hwmon_sensor_types type,
				      u32 attr, int channel)
{
	const struct smartdevice_priv_data *priv = data;

	if (channel >= priv->channel_count)
		return 0;

	switch (type) {
	case hwmon_fan:
	case hwmon_curr:
	case hwmon_in:
		return 0444;
	case hwmon_pwm:
		switch (attr) {
		case hwmon_pwm_input:
		case hwmon_pwm_enable:
			return 0644;
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

	if (time_after(jiffies, priv->status[channel].updated + STATUS_VALIDITY * HZ)) /* FIXME */
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
		switch (attr) {
		case hwmon_pwm_input:
			/* The device does not support reading the control
			 * mode, but user-space really depends on this being
			 * RW.  Fake the read and return the last value set.
			 */
			*val = priv->status[channel].pwm;
			break;
		case hwmon_pwm_enable:
			*val = priv->status[channel].device_mode != 0;
			break;
		case hwmon_pwm_mode:
			if (!priv->status[channel].device_mode)
				return -EOPNOTSUPP;
			*val = priv->status[channel].device_mode >> 1;
			break;
		default:
			return -EOPNOTSUPP; /* unreachable */
		}
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

	/*
	 * The device does not support changing the control method, but
	 * user-space really depends on the pwm[*]_enable attribute being RW.
	 * Fake the write and behave as if the device had immediately reverted
	 * the change.
	 */
	if (attr == hwmon_pwm_enable)
		return 0;

	/*
	 * The device does not honor changes to the duty cycle of a fan channel
	 * it thinks is unconnected.
	 */
	if (!(priv->status[channel].device_mode & 0x3))
		return -EOPNOTSUPP;

	if (mutex_lock_interruptible(&priv->lock))
		return -EINTR;

	priv->out[0] = REPORT_CONFIG;
	priv->out[1] = CONFIG_FAN_PWM;
	priv->out[2] = channel;
	priv->out[3] = 0x00;
	priv->out[4] = clamp_val(val, 0, 255) * 100 / 255;

	ret = hid_hw_output_report(priv->hid_dev, priv->out, 5);

	if (ret == 5)
		priv->status[channel].pwm = priv->out[4];

	mutex_unlock(&priv->lock);

	if (ret < 0)
		return ret;
	if (ret != 5)
		return -EPIPE; /* FIXME */

	return 0;
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
	if (channel > priv->channel_count)
		return 0;

	/* TODO are all of these stores atomic? */
	/* TODO are all corresponding reads (in other functions) atomic? */
	priv->status[channel].rpms = get_unaligned_be16(data + 3);
	priv->status[channel].centiamps = data[9] * 100 + data[10];
	priv->status[channel].centivolts = data[7] * 100 + data[8];
	priv->status[channel].device_mode = data[15] & 0x3;
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
	int i, ret;

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
	for (i = 0; i < priv->channel_count; i++) {
		/* TODO set pwm to 40% */
		priv->status[i].updated = jiffies - STATUS_VALIDITY * HZ;
	}

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
		hid_err(hdev, "req init failed with %d\n", ret);
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
#ifdef CONFIG_PM
	.reset_resume = smartdevice_reset_resume,
	.resume = smartdevice_resume, /* FIXME remove */
#endif
};

static int __init smartdevice_init(void)
{
	/* FIXME remove */
	printk(KERN_DEBUG "smartdevice_priv_data takes %ld bytes\n",
	       sizeof(struct smartdevice_priv_data) - sizeof(struct mutex) + 32);
	printk(KERN_DEBUG "smartdevice_status_data takes %ld bytes\n",
	       sizeof(struct smartdevice_status_data));
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
