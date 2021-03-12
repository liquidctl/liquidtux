// SPDX-License-Identifier: GPL-2.0+
/*
 * nzxt-kraken2.c - hwmon driver for NZXT Kraken X42/X52/X62/X72 coolers
 *
 * Copyright 2019  Jonas Malaco <jonas@protocubo.io>
 */

#include <asm/unaligned.h>
#include <linux/hid.h>
#include <linux/hwmon.h>
#include <linux/module.h>
#include <linux/spinlock.h>

#define DRIVER_NAME	"nzxt-kraken2"
#define HWMON_NAME	"kraken2"

#define STATUS_REPORT		0x04
#define STATUS_USEFUL_SIZE	8

static const char *const kraken2_temp_label[] = {
	"Coolant",
};

static const char *const kraken2_fan_label[] = {
	"Fans",
	"Pump",
};

struct kraken2_priv_data {
	struct hid_device *hid_dev;
	struct device *hwmon_dev;

	spinlock_t lock;
	u8 status[STATUS_USEFUL_SIZE];
};

static umode_t kraken2_is_visible(const void *data,
				  enum hwmon_sensor_types type,
				  u32 attr, int channel)
{
	return 0444;
}

static int kraken2_read(struct device *dev, enum hwmon_sensor_types type,
			u32 attr, int channel, long *val)
{
	struct kraken2_priv_data *priv = dev_get_drvdata(dev);
	unsigned long flags;
	int offset;

	switch (type) {
	case hwmon_temp:
		/*
		 * status[2] has been observed to be in the interval [1,9], but
		 * some steps are skipped for certain status[1] values; a 1/9
		 * °C precision would also be very odd.  Just do the simple
		 * thing and assume status[2] is the number of 0.1 °C
		 * increments, and consider that the possible inaccuracy is too
		 * small to matter in practice.
		 */
		spin_lock_irqsave(&priv->lock, flags);
		*val = priv->status[1] * 1000 + priv->status[2] * 100;
		spin_unlock_irqrestore(&priv->lock, flags);
		break;
	case hwmon_fan:
		offset = 3 + channel * 2;
		spin_lock_irqsave(&priv->lock, flags);
		*val = get_unaligned_be16(priv->status + 3 + channel * 2);
		spin_unlock_irqrestore(&priv->lock, flags);
		break;
	default:
		return -EINVAL;
	}

	return 0;
}

static int kraken2_read_string(struct device *dev,
			       enum hwmon_sensor_types type, u32 attr,
			       int channel, const char **str)
{
	switch (type) {
	case hwmon_temp:
		*str = kraken2_temp_label[channel];
		break;
	case hwmon_fan:
		*str = kraken2_fan_label[channel];
		break;
	default:
		return -EINVAL;
	}
	return 0;
}

static const struct hwmon_ops kraken2_hwmon_ops = {
	.is_visible = kraken2_is_visible,
	.read = kraken2_read,
	.read_string = kraken2_read_string,
};

static const struct hwmon_channel_info *kraken2_info[] = {
	HWMON_CHANNEL_INFO(temp,
			   HWMON_T_INPUT | HWMON_T_LABEL),
	HWMON_CHANNEL_INFO(fan,
			   HWMON_F_INPUT | HWMON_F_LABEL,
			   HWMON_F_INPUT | HWMON_F_LABEL),
	NULL
};

static const struct hwmon_chip_info kraken2_chip_info = {
	.ops = &kraken2_hwmon_ops,
	.info = kraken2_info,
};

static int kraken2_raw_event(struct hid_device *hdev,
			     struct hid_report *report, u8 *data, int size)
{
	struct kraken2_priv_data *priv;
	unsigned long flags;

	if (report->id != STATUS_REPORT)
		return 0;

	if (size < STATUS_USEFUL_SIZE)
		return -ENODATA;  /* FIXME */

	priv = hid_get_drvdata(hdev);

	spin_lock_irqsave(&priv->lock, flags);
	memcpy(priv->status, data, STATUS_USEFUL_SIZE);
	spin_unlock_irqrestore(&priv->lock, flags);

	return 0;
}

static int kraken2_probe(struct hid_device *hdev,
			 const struct hid_device_id *id)
{
	struct kraken2_priv_data *priv;
	int ret;

	priv = devm_kzalloc(&hdev->dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->hid_dev = hdev;
	spin_lock_init(&priv->lock);
	hid_set_drvdata(hdev, priv);

	ret = hid_parse(hdev);
	if (ret) {
		hid_err(hdev, "hid_parse failed with %d\n", ret);
		return ret;
	}

	/*
	 * Enable hidraw so existing user-space tools can continue to work and
	 * provide additional functionality, at least for now.
	 */
	ret = hid_hw_start(hdev, HID_CONNECT_HIDRAW);
	if (ret) {
		hid_err(hdev, "hid_hw_start failed with %d\n", ret);
		goto rec_stop_hid;
	}

	ret = hid_hw_open(hdev);
	if (ret) {
		hid_err(hdev, "hid_hw_open failed with %d\n", ret);
		goto rec_close_hid;
	}

	priv->hwmon_dev = hwmon_device_register_with_info(&hdev->dev, HWMON_NAME,
							  priv, &kraken2_chip_info,
							  NULL);
	if (IS_ERR(priv->hwmon_dev)) {
		hid_err(hdev, "failed to register hwmon device\n");
		ret = PTR_ERR(priv->hwmon_dev);
		goto rec_close_hid;
	}

	return 0;

rec_close_hid:
	hid_hw_close(hdev);
rec_stop_hid:
	hid_hw_stop(hdev);
	return ret;
}

static void kraken2_remove(struct hid_device *hdev)
{
	struct kraken2_priv_data *priv = hid_get_drvdata(hdev);
	
	hwmon_device_unregister(priv->hwmon_dev);

	hid_hw_close(hdev);
	hid_hw_stop(hdev);
}

static const struct hid_device_id kraken2_table[] = {
	{ HID_USB_DEVICE(0x1e71, 0x170e) }, /* NZXT Kraken X42, X52, X62 or X72 */
	{ }
};

MODULE_DEVICE_TABLE(hid, kraken2_table);

static struct hid_driver kraken2_driver = {
	.name = DRIVER_NAME,
	.id_table = kraken2_table,
	.probe = kraken2_probe,
	.remove = kraken2_remove,
	.raw_event = kraken2_raw_event,
};

module_hid_driver(kraken2_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Jonas Malaco <jonas@protocubo.io>");
MODULE_DESCRIPTION("Hwmon driver for NZXT Kraken X42/X52/X62/X72 coolers");