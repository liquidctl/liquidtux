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

#define DRVNAME "liquidctl"	/* FIXME hid x usb, hwmon x other, nzxt x other */

struct liquidctl_device_data {
	struct hid_device *hid_dev;
	struct device *hwmon_dev;

	int temp_count;
	int fan_count;
	const char *const *temp_label;
	const char *const *fan_label;
	long *temp_in;
	long *fan_in;
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
		if (attr != hwmon_temp_input || channel >= ldata->temp_count)
			return -EINVAL;
		*val = ldata->temp_in[channel];
		break;
	case hwmon_fan:
		if (attr != hwmon_fan_input || channel >= ldata->fan_count)
			return -EINVAL;
		*val = ldata->fan_in[channel];
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
	struct liquidctl_device_data *ldata = dev_get_drvdata(dev);

	switch (type) {
	case hwmon_temp:
		if (attr != hwmon_temp_label || channel >= ldata->temp_count ||
		    !ldata->temp_label[channel])
			return -EINVAL;
		*str = ldata->temp_label[channel];
		break;
	case hwmon_fan:
		if (attr != hwmon_fan_label || channel >= ldata->fan_count ||
		    !ldata->fan_label[channel])
			return -EINVAL;
		*str = ldata->fan_label[channel];
		break;
	default:
		return -EINVAL;
	}
	return 0;
}

static const struct hwmon_ops liquidctl_hwmon_ops = {
	.is_visible = liquidctl_is_visible,
	.read = liquidctl_read,
	.read_string = liquidctl_read_string,
};

#define DEVNAME_KRAKEN_GEN3 "kraken"	/* FIXME not precise for user-space */
#define KRAKEN_TEMP_COUNT		1
#define KRAKEN_FAN_COUNT		2

static const char *const kraken_temp_label[] = {
	"Coolant",
};

static const u32 kraken_temp_config[] = {
	HWMON_T_INPUT | HWMON_T_LABEL,
	0
};

static const u32 kraken_fan_config[] = {
	HWMON_F_INPUT,
	HWMON_F_INPUT | HWMON_F_LABEL,
	0
};

static const char *const kraken_fan_label[] = {
	NULL,
	"Pump",
};

static const struct hwmon_channel_info kraken_temp = {
	.type = hwmon_temp,
	.config = kraken_temp_config,
};

static const struct hwmon_channel_info kraken_fan = {
	.type = hwmon_fan,
	.config = kraken_fan_config,
};

static const struct hwmon_channel_info *kraken_info[] = {
	&kraken_temp,
	&kraken_fan,
	NULL			/* TODO pwm */
};

static const struct hwmon_chip_info kraken_chip_info = {
	.ops = &liquidctl_hwmon_ops,
	.info = kraken_info,
};

#define DEVNAME_SMART_DEVICE "smart_device"
#define SMART_DEVICE_TEMP_COUNT		0
#define SMART_DEVICE_FAN_COUNT		3

static const u32 smart_device_fan_config[] = {
	HWMON_F_INPUT,
	HWMON_F_INPUT,
	HWMON_F_INPUT,
	0
};

static const struct hwmon_channel_info smart_device_fan = {
	.type = hwmon_fan,
	.config = smart_device_fan_config,
};

static const struct hwmon_channel_info *smart_device_info[] = {
	&smart_device_fan,
	NULL			/* TODO cur, in, pwm */
};

static const struct hwmon_chip_info smart_device_chip_info = {
	.ops = &liquidctl_hwmon_ops,
	.info = smart_device_info,
};

#define USB_VENDOR_ID_NZXT		0x1e71
#define USB_DEVICE_ID_KRAKEN_GEN3	0x170e
#define USB_DEVICE_ID_SMART_DEVICE	0x1714

#define STATUS_REPORT_ID		4
#define STATUS_MIN_BYTES		16

#define show_ctx() \
	printk(KERN_DEBUG "%s:%d: irq: %lu, serving_softirq: %lu, nmi: %lu, task: %u\n", \
	       __FUNCTION__, __LINE__, in_irq(), in_serving_softirq(), in_nmi(), in_task());

static int liquidctl_raw_event(struct hid_device *hdev,
			       struct hid_report *report, u8 *data, int size)
{
	struct liquidctl_device_data *ldata;
	u8 channel;

	/* TODO show_ctx(): in hard irq, how much should we do here? */

	/* TODO we only want one report, specify it in hid_driver */
	if (report->id != STATUS_REPORT_ID || size < STATUS_MIN_BYTES)
		return 0;

	ldata = hid_get_drvdata(hdev);

	/* TODO reads don't need the latest data, but each store must be atomic */
	switch (hdev->product) {
	case USB_DEVICE_ID_KRAKEN_GEN3:
		ldata->temp_in[0] = data[1] * 1000 + data[2] * 100;
		ldata->fan_in[0] = be16_to_cpup((__be16 *) (data + 3));
		ldata->fan_in[1] = be16_to_cpup((__be16 *) (data + 5));
		break;
	case USB_DEVICE_ID_SMART_DEVICE:
		channel = data[15] >> 4;
		if (channel >= ldata->fan_count)
			return 0;
		ldata->fan_in[channel] = be16_to_cpup((__be16 *) (data + 3));
		break;
	default:
		return 0;
	}
	return 0;
}

static const struct hid_device_id liquidctl_table[] = {
	{ HID_USB_DEVICE(USB_VENDOR_ID_NZXT, USB_DEVICE_ID_KRAKEN_GEN3) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_NZXT, USB_DEVICE_ID_SMART_DEVICE) },
	{ }
};

MODULE_DEVICE_TABLE(hid, liquidctl_table);

static int liquidctl_probe(struct hid_device *hdev,
			   const struct hid_device_id *id)
{
	struct liquidctl_device_data *ldata;
	struct device *hwmon_dev;
	const struct hwmon_chip_info *chip_info;
	char *chip_name;
	int ret;

	ldata = devm_kzalloc(&hdev->dev, sizeof(*ldata), GFP_KERNEL);
	if (!ldata)
		return -ENOMEM;

	switch (hdev->product) {
	case USB_DEVICE_ID_KRAKEN_GEN3:
		chip_name = DEVNAME_KRAKEN_GEN3;
		ldata->temp_count = KRAKEN_TEMP_COUNT;
		ldata->fan_count = KRAKEN_FAN_COUNT;
		ldata->temp_label = kraken_temp_label;
		ldata->fan_label = kraken_fan_label;
		chip_info = &kraken_chip_info;
		break;
	case USB_DEVICE_ID_SMART_DEVICE:
		chip_name = DEVNAME_SMART_DEVICE;
		ldata->temp_count = SMART_DEVICE_TEMP_COUNT;
		ldata->fan_count = SMART_DEVICE_FAN_COUNT;
		ldata->temp_label = NULL;
		ldata->fan_label = NULL;
		chip_info = &smart_device_chip_info;
		break;
	default:
		return -EINVAL;
	}
	hid_info(hdev, "device: %s\n", chip_name);

	ldata->temp_in = devm_kcalloc(&hdev->dev, ldata->temp_count,
				      sizeof(*ldata->temp_in), GFP_KERNEL);
	if (!ldata->temp_in)
		return -ENOMEM;

	ldata->fan_in = devm_kcalloc(&hdev->dev, ldata->fan_count,
				     sizeof(*ldata->fan_in), GFP_KERNEL);
	if (!ldata->fan_in)
		return -ENOMEM;

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

	hwmon_dev = devm_hwmon_device_register_with_info(&hdev->dev, chip_name,
							 ldata, chip_info,
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
