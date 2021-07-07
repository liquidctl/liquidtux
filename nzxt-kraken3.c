// SPDX-License-Identifier: GPL-2.0+
/*
 * nzxt-kraken3.c - hwmon driver for NZXT Kraken X53/X63/X73 coolers
 *
 * Copyright 2021  Jonas Malaco <jonas@protocubo.io>
 */

#include <asm/unaligned.h>
#include <linux/hid.h>
#include <linux/hwmon.h>
#include <linux/jiffies.h>
#include <linux/module.h>

#define STATUS_REPORT_ID	0x75
#define STATUS_INTERVAL		1 /* seconds */
#define STATUS_VALIDITY		(4 * STATUS_INTERVAL) /* seconds */

static const char *const kraken3_temp_label[] = {
	"Coolant",
};

static const char *const kraken3_fan_label[] = {
	"Pump",
};

struct kraken3_priv_data {
	struct hid_device *hid_dev;
	struct device *hwmon_dev;
	s32 temp_input[1];
	u16 fan_input[1];
	unsigned long updated; /* jiffies */
	u8 out[8]; /* DMA output buffer */
};

static umode_t kraken3_is_visible(const void *data,
				  enum hwmon_sensor_types type,
				  u32 attr, int channel)
{
	return 0444;
}

static int kraken3_read(struct device *dev, enum hwmon_sensor_types type,
			u32 attr, int channel, long *val)
{
	struct kraken3_priv_data *priv = dev_get_drvdata(dev);

	if (time_after(jiffies, priv->updated + STATUS_VALIDITY * HZ))
		return -ENODATA;

	switch (type) {
	case hwmon_temp:
		*val = priv->temp_input[channel];
		break;
	case hwmon_fan:
		*val = priv->fan_input[channel];
		break;
	default:
		return -EOPNOTSUPP;
	}

	return 0;
}

static int kraken3_read_string(struct device *dev, enum hwmon_sensor_types type,
			       u32 attr, int channel, const char **str)
{
	switch (type) {
	case hwmon_temp:
		*str = kraken3_temp_label[channel];
		break;
	case hwmon_fan:
		*str = kraken3_fan_label[channel];
		break;
	default:
		return -EOPNOTSUPP;
	}
	return 0;
}

static const struct hwmon_ops kraken3_hwmon_ops = {
	.is_visible = kraken3_is_visible,
	.read = kraken3_read,
	.read_string = kraken3_read_string,
};

static const struct hwmon_channel_info *kraken3_info[] = {
	HWMON_CHANNEL_INFO(temp,
			   HWMON_T_INPUT | HWMON_T_LABEL),
	HWMON_CHANNEL_INFO(fan,
			   HWMON_F_INPUT | HWMON_F_LABEL),
	NULL
};

static const struct hwmon_chip_info kraken3_chip_info = {
	.ops = &kraken3_hwmon_ops,
	.info = kraken3_info,
};

static int kraken3_raw_event(struct hid_device *hdev,
			     struct hid_report *report, u8 *data, int size)
{
	struct kraken3_priv_data *priv;

	if (size < 20 || report->id != STATUS_REPORT_ID)
		return 0;

	if (data[15] == 0xff && data[16] == 0xff)
		return 0;

	priv = hid_get_drvdata(hdev);
	priv->temp_input[0] = data[15] * 1000 + data[16] * 100;
	priv->fan_input[0] = get_unaligned_le16(data + 17);
	priv->updated = jiffies;

	return 0;
}

/*
 * Caller must ensure exclusive access to buf.
 */
static int kraken3_init_device(struct hid_device *hdev, u8 *buf)
{
	u8 set_interval_cmd[5] = {0x70, 0x02, 0x01, 0xb8, STATUS_INTERVAL};
	u8 finish_init_cmd[2] = {0x70, 0x01};

	int ret;
	memcpy(buf, set_interval_cmd, 5);
	ret = hid_hw_output_report(hdev, buf, 5);
	if (ret < 0)
		return ret;

	memcpy(buf, finish_init_cmd, 2);
	ret = hid_hw_output_report(hdev, buf, 2);
	if (ret < 0)
		return ret;

	return 0;
}

#ifdef CONFIG_PM
/*
 * Caller must ensure exclusive access to priv->out.
 */
static int kraken3_reset_resume(struct hid_device *hdev)
{
	struct kraken3_priv_data *priv = hid_get_drvdata(hdev);
	int ret;

	ret = kraken3_init_device(hdev, priv->out);
	if (ret)
		hid_err(hdev, "req init (reset_resume) failed with %d\n", ret);

	return ret;
}
#endif

static int kraken3_probe(struct hid_device *hdev,
			 const struct hid_device_id *id)
{
	struct kraken3_priv_data *priv;
	int ret;

	priv = devm_kzalloc(&hdev->dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->hid_dev = hdev;
	hid_set_drvdata(hdev, priv);

	/*
	 * Initialize ->updated to STATUS_VALIDITY seconds in the past, making
	 * the initial empty data invalid for kraken3_read without the need for
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
	 * Concurrent access to priv->out is not possible.
	 */
	ret = kraken3_init_device(hdev, priv->out);
	if (ret) {
		hid_err(hdev, "device init failed with %d\n", ret);
		goto fail_and_close;
	}

	priv->hwmon_dev = hwmon_device_register_with_info(&hdev->dev, "kraken3",
							  priv, &kraken3_chip_info,
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

static void kraken3_remove(struct hid_device *hdev)
{
	struct kraken3_priv_data *priv = hid_get_drvdata(hdev);

	hwmon_device_unregister(priv->hwmon_dev);

	hid_hw_close(hdev);
	hid_hw_stop(hdev);
}

static const struct hid_device_id kraken3_table[] = {
	{ HID_USB_DEVICE(0x1e71, 0x2007) }, /* NZXT Kraken X53/X63/X73 */
	/*
	 * TODO Add support for NZXT Kraken Z53/Z63/Z73
	 *
	 *     { HID_USB_DEVICE(0x1e71, 0x3008) },
	 *
	 * WARNING: Kraken Z coolers appear to require a write in order to
	 * fetch the status, besides also reporting fan speeds.
	 */
	{ }
};

MODULE_DEVICE_TABLE(hid, kraken3_table);

static struct hid_driver kraken3_driver = {
	.name = "nzxt-kraken3",
	.id_table = kraken3_table,
	.probe = kraken3_probe,
	.remove = kraken3_remove,
	.raw_event = kraken3_raw_event,
#ifdef CONFIG_PM
	.reset_resume = kraken3_reset_resume,
#endif
};

static int __init kraken3_init(void)
{
	return hid_register_driver(&kraken3_driver);
}

static void __exit kraken3_exit(void)
{
	hid_unregister_driver(&kraken3_driver);
}

/*
 * When compiled into the kernel, initialize after the hid bus.
 */
late_initcall(kraken3_init);
module_exit(kraken3_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Jonas Malaco <jonas@protocubo.io>");
MODULE_DESCRIPTION("Hwmon driver for NZXT Kraken X53/X63/X73 coolers");
