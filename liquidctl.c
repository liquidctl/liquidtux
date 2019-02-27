// SPDX-License-Identifier: GPL-2.0+
/*
 * liquidctl.c - hwmon for closed-loop liquid coolers or AIOs
 *
 * Copyright 2019  Jonas Malaco <jonas@protocubo.io>
 */

#include <linux/hid.h>
#include <linux/hwmon.h>
#include <linux/kernel.h>
#include <linux/module.h>

#define __passed printk(KERN_DEBUG DRVNAME ": passed %s:%d\n", __FUNCTION__, __LINE__);

#define DRVNAME "liquidctl"  /* FIXME for upstream (hid x usb, hwmon x other) */
#define DEVNAME_KRAKEN_GEN3 "kraken"  /* FIXME not descriptive for user-space */

/* FIXME generalize temp and fan data */
struct liquidctl_device_data {
	struct hid_device *hid_dev;
	struct device *hwmon_dev;
	long temp1;
	long fan1;
	long fan2;
};

static umode_t liquidctl_is_visible(const void *data,
				    enum hwmon_sensor_types type,
				    u32 attr, int channel)
{
	return S_IRUGO;
}

static int liquidctl_read(struct device *dev, enum hwmon_sensor_types type,
			  u32 attr, int channel, long *val)
{
	struct liquidctl_device_data *ldata = dev_get_drvdata(dev);

	switch (type) {
	case hwmon_temp:
		switch (attr) {
		case hwmon_temp_input:
			*val = ldata->temp1;
			break;
		default:
			return -EINVAL;
		}
		break;
	case hwmon_fan:
		switch (attr) {
		case hwmon_fan_input:
			switch (channel) {
			case 0:
				*val = ldata->fan1;
				break;
			case 1:
				*val = ldata->fan2;
				break;
			default:
				return -EINVAL;
			}
			break;
		default:
			return -EINVAL;
		}
		break;
	default:
		return -EINVAL;
	}
	return 0;
}

static int liquidctl_read_string(struct device *dev,
				 enum hwmon_sensor_types type, u32 attr,
				 int channel, const char **str)
{
	switch (type) {
	case hwmon_temp:
		switch (attr) {
		case hwmon_temp_label:
			*str = "Coolant";
			break;
		default:
			return -EINVAL;
		}
		break;
	case hwmon_fan:
		switch (attr) {
		case hwmon_fan_label:
			*str = "Pump";
			break;
		default:
			return -EINVAL;
		}
		break;
	default:
		return -EINVAL;
	}
	return 0;
}

static const u32 liquidctl_temp_config[] = {
	HWMON_T_INPUT | HWMON_T_LABEL,
	0
};

static const u32 liquidctl_fan_config[] = {
	HWMON_F_INPUT,
	HWMON_F_INPUT | HWMON_F_LABEL,
	0
};

static const struct hwmon_channel_info liquidctl_temp = {
	.type = hwmon_temp,
	.config = liquidctl_temp_config,
};

static const struct hwmon_channel_info liquidctl_fan = {
	.type = hwmon_fan,
	.config = liquidctl_fan_config,
};

static const struct hwmon_ops liquidctl_hwmon_ops = {
	.is_visible = liquidctl_is_visible,
	.read = liquidctl_read,
	.read_string = liquidctl_read_string,
};

static const struct hwmon_channel_info *kraken_info[] = {
	&liquidctl_temp,
	&liquidctl_fan,
	NULL
};

static const struct hwmon_chip_info kraken_chip_info = {
	.ops = &liquidctl_hwmon_ops,
	.info = kraken_info,
};

static int liquidctl_raw_event(struct hid_device *hdev,
			       struct hid_report *report, u8 *data, int size)
{
	struct liquidctl_device_data *ldata = hid_get_drvdata(hdev);

	/* printk(KERN_DEBUG DRVNAME " raw_event report: id=%u, type=%u, application=%u, maxfield=%u, size=%u", */
	/* 		report->id, report->type, report->application, report->maxfield, report->size); */
	/* print_hex_dump(KERN_DEBUG, DRVNAME, DUMP_PREFIX_OFFSET, 16, 4, data, */
	/* 		size, false); */

	if (size < 32) {
		hid_err(hdev, "message too short: %d\n", size);
		return -EINVAL;
	}

	/* FIXME add lock */
	ldata->temp1 = data[1] * 1000 + data[2] * 100;
	ldata->fan1 = be16_to_cpup((__be16 *)(data + 3));
	ldata->fan2 = be16_to_cpup((__be16 *)(data + 5));
	return 0;
}

#define USB_VENDOR_ID_NZXT		0x1e71
#define USB_DEVICE_ID_KRAKEN_GEN3	0x170e

static const struct hid_device_id liquidctl_table[] = {
	{ HID_USB_DEVICE(USB_VENDOR_ID_NZXT, USB_DEVICE_ID_KRAKEN_GEN3) },
	{ }
};
MODULE_DEVICE_TABLE(hid, liquidctl_table);

static int liquidctl_probe(struct hid_device *hdev,
			   const struct hid_device_id *id)
{
	struct liquidctl_device_data *ldata;
	struct device *hwmon_dev;
	int ret;

	ldata = devm_kzalloc(&hdev->dev, sizeof(*ldata), GFP_KERNEL);
	if (!ldata)
		return -ENOMEM;

	hid_info(hdev, "device: " DEVNAME_KRAKEN_GEN3 "\n");
	ldata->hid_dev = hdev;
	hid_set_drvdata(hdev, ldata);

	ret = hid_parse(hdev);
	if (ret) {
		hid_err(hdev, "hid_parse failed with %d\n", ret);
		return ret;
	}

	/* keep hidraw so user-space can (easily) take care of the other
	 * features of the device (e.g. LEDs) */
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

	hwmon_dev = devm_hwmon_device_register_with_info(&hdev->dev,
							 DEVNAME_KRAKEN_GEN3,
							 ldata,
							 &kraken_chip_info,
							 NULL);
	if (IS_ERR(hwmon_dev)) {
		hid_err(hdev, "failed to register hwmon device\n");
		ret = PTR_ERR(hwmon_dev);
		goto rec_close_hid;
	}
	ldata->hwmon_dev = hwmon_dev;

	hid_info(hdev, "probing successful\n");
	return 0;

rec_close_hid:
	hid_hw_close(hdev);
rec_stop_hid:
	hid_hw_stop(hdev);
	return ret;
}

static void liquidctl_remove(struct hid_device *hdev)
{
	hid_hw_close(hdev);
	hid_hw_stop(hdev);
}

static struct hid_driver liquidctl_driver = {
	.name = DRVNAME,
	.id_table = liquidctl_table,
	.probe = liquidctl_probe,
	.remove = liquidctl_remove,
	.raw_event = liquidctl_raw_event,
};

module_hid_driver(liquidctl_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Jonas Malaco <jonas@protocubo.io>");
MODULE_DESCRIPTION("Closed loop liquid coolers monitoring");

