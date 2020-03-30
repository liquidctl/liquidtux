// SPDX-License-Identifier: GPL-2.0+
/*
 * ultmt.c - hwmon driver for the Aqua Computer aquastream ULTIMATE
 *
 * Copyright 2020 Matthias Groß <grmat@sub.red>
 * Based on grdp3.c by Jonas Malaco <jonas@protocubo.io>
 */

#include <linux/hid.h>
#include <linux/hwmon.h>
#include <linux/kernel.h>
#include <linux/module.h>

#define DRVNAME					"ultmt"

#define AQUA_COMPUTER_VENDOR_ID			0x0c70
#define AQUASTREAM_ULTIMATE_PRODUCT_ID		0xf00b

#define AQUASTREAM_ULTIMATE_STATUS_REPORT_ID	1

struct aquastream_device_data {
	struct hid_device *hid_dev;
	struct device *hwmon_dev;

	struct aquastream_ultimate_report_1 *report_1;
};

struct aquastream_ultimate_report_1 {
	u_int8_t report_id;
	u_int16_t raw0[22];

	u_int16_t temp_0;
	u_int16_t temp_1;
	u_int16_t temp_2;
	u_int16_t temp_3;
	u_int16_t temp_4;

	u_int16_t flow_external;
	u_int16_t flow_virtual;
	u_int16_t flow;

	u_int16_t pump_voltage;

	u_int16_t fan_mode;
	u_int16_t fan_current;
	u_int16_t fan_voltage;
	u_int16_t fan_power;
	u_int16_t fan_rpm;
	u_int16_t fan_torque;
	u_int16_t fan_target_power_percent;

	u_int16_t pump_mode;
	u_int16_t pump_state;
	u_int16_t pump_rpm;
	u_int16_t pump_current;
	u_int16_t pump_power;
	u_int16_t pump_pressure;
	u_int16_t pump_target_rpm;

	u_int16_t raw1[6];
} __packed;

static umode_t aquastream_is_visible(const void *data,
		enum hwmon_sensor_types type,
		u32 attr, int channel)
{
	return S_IRUGO;
}

static int aquastream_read(struct device *dev, enum hwmon_sensor_types type,
		u32 attr, int channel, long *val)
{
	struct aquastream_device_data *ldata = dev_get_drvdata(dev);

	switch (type) {
		case hwmon_temp:
			if (attr != hwmon_temp_input) {
				return -EINVAL;
			}
			switch (channel) {
				case 0:
					*val = be16_to_cpu(ldata->report_1->temp_0);
					break;
				case 1:
					*val = be16_to_cpu(ldata->report_1->temp_1);
					break;
				case 2:
					*val = be16_to_cpu(ldata->report_1->temp_2);
					break;
				case 3:
					*val = be16_to_cpu(ldata->report_1->temp_3);
					break;
				case 4:
					*val = be16_to_cpu(ldata->report_1->temp_4);
					break;
				default:
					return -EINVAL;
			}

			if (*val == SHRT_MAX) {
				return -EINVAL;
			} else {
				*val *= 10;
			}
			break;

		case hwmon_fan:
			switch (attr) {
				case hwmon_fan_input:
					switch (channel) {
						case 0:
							*val = be16_to_cpu(ldata->report_1->pump_rpm);
							break;
						case 1:
							*val = be16_to_cpu(ldata->report_1->fan_rpm);
							break;
						default :
							return -EINVAL;
					}
					break;
				case hwmon_fan_target:
					switch (channel) {
						case 0:
							*val = be16_to_cpu(ldata->report_1->pump_target_rpm);
							break;
						case 1:
							/* TODO this is PWM percent *100 */
							*val = be16_to_cpu(ldata->report_1->fan_target_power_percent);
							break;
						default :
							return -EINVAL;
					}
					break;
				default :
					return -EINVAL;
			}
			break;

		case hwmon_in:
			if (attr != hwmon_in_input)
				return -EINVAL;
			switch (channel) {
				case 0:
					*val = be16_to_cpu(ldata->report_1->pump_voltage);
					break;
				case 1:
					*val = be16_to_cpu(ldata->report_1->fan_voltage);
					break;
				default:
					return -EINVAL;
			}

			if (*val == SHRT_MAX) {
				return -EINVAL;
			} else {
				*val *= 10;
			}
			break;

		case hwmon_curr:
			if (attr != hwmon_curr_input)
				return -EINVAL;
			switch (channel) {
				case 0:
					*val = be16_to_cpu(ldata->report_1->pump_current);
					break;
				case 1:
					*val = be16_to_cpu(ldata->report_1->fan_current);
					break;
				default:
					return -EINVAL;
			}

			if (*val == SHRT_MAX) {
				return -EINVAL;
			}
			break;

		case hwmon_power:
			if (attr != hwmon_power_input)
				return -EINVAL;
			switch (channel) {
				case 0:
					*val = be16_to_cpu(ldata->report_1->pump_power);
					break;
				case 1:
					*val = be16_to_cpu(ldata->report_1->fan_power);
					break;
				default:
					return -EINVAL;
			}

			if (*val == SHRT_MAX) {
				return -EINVAL;
			} else {
				*val *= 10000;
			}
			break;

		default:
			return -EINVAL;
	}
	return 0;
}

static const char *aquastream_ultimate_temp_label[] = {
	"internal",
	0,
	0,
	0,
	0,
};

static const u32 aquastream_ultimate_temp_config[] = {
	HWMON_T_INPUT | HWMON_T_LABEL,
	HWMON_T_INPUT,
	HWMON_T_INPUT,
	HWMON_T_INPUT,
	HWMON_T_INPUT,
	0
};

static const struct hwmon_channel_info aquastream_ultimate_temp = {
	.type = hwmon_temp,
	.config = aquastream_ultimate_temp_config,
};

static const char *aquastream_ultimate_fan_label[] = {
	"Pump RPM",
	"Fan RPM",
	0,
};

static const u32 aquastream_ultimate_fan_config[] = {
	HWMON_F_INPUT | HWMON_F_LABEL | HWMON_F_TARGET,
	HWMON_F_INPUT | HWMON_F_LABEL | HWMON_F_TARGET,
	0
};

static const struct hwmon_channel_info aquastream_ultimate_fan = {
	.type = hwmon_fan,
	.config = aquastream_ultimate_fan_config,
};

static const char *aquastream_ultimate_in_label[] = {
	"Pump voltage",
	"Fan voltage",
	0,
};

static const u32 aquastream_ultimate_in_config[] = {
	HWMON_I_INPUT | HWMON_I_LABEL,
	HWMON_I_INPUT | HWMON_I_LABEL,
	0
};

static const struct hwmon_channel_info aquastream_ultimate_in = {
	.type = hwmon_in,
	.config = aquastream_ultimate_in_config,
};

static const char *aquastream_ultimate_curr_label[] = {
	"Pump current",
	"Fan current",
	0,
};

static const u32 aquastream_ultimate_curr_config[] = {
	HWMON_C_INPUT | HWMON_C_LABEL,
	HWMON_C_INPUT | HWMON_C_LABEL,
	0
};

static const struct hwmon_channel_info aquastream_ultimate_curr = {
	.type = hwmon_curr,
	.config = aquastream_ultimate_curr_config,
};

static const char *aquastream_ultimate_power_label[] = {
	"Pump power",
	"Fan power",
	0,
};

static const u32 aquastream_ultimate_power_config[] = {
	HWMON_P_INPUT | HWMON_P_LABEL,
	HWMON_P_INPUT | HWMON_P_LABEL,
	0
};

static const struct hwmon_channel_info aquastream_ultimate_power = {
	.type = hwmon_power,
	.config = aquastream_ultimate_power_config,
};

static const struct hwmon_channel_info *aquastream_ultimate_info[] = {
	&aquastream_ultimate_temp,
	&aquastream_ultimate_fan,
	&aquastream_ultimate_in,
	&aquastream_ultimate_curr,
	&aquastream_ultimate_power,
	0
};

static int aquastream_read_labels(struct device *dev, enum hwmon_sensor_types type,
		u32 attr, int channel, const char **val)
{
	switch (type) {
		case hwmon_temp:
			if (attr != hwmon_temp_label || channel > 4) {
				return -EINVAL;
			}
			*val = aquastream_ultimate_temp_label[channel];
			break;
		case hwmon_fan:
			if (attr != hwmon_fan_label || channel > 1) {
				return -EINVAL;
			}
			*val = aquastream_ultimate_fan_label[channel];
			break;
		case hwmon_in:
			if (attr != hwmon_in_label || channel > 1) {
				return -EINVAL;
			}
			*val = aquastream_ultimate_in_label[channel];
			break;
		case hwmon_curr:
			if (attr != hwmon_curr_label || channel > 1) {
				return -EINVAL;
			}
			*val = aquastream_ultimate_curr_label[channel];
			break;
		case hwmon_power:
			if (attr != hwmon_power_label || channel > 1) {
				return -EINVAL;
			}
			*val = aquastream_ultimate_power_label[channel];
			break;
		default:
			return -EINVAL;
	}
	return 0;
}

static const struct hwmon_ops aquastream_hwmon_ops = {
	.is_visible = aquastream_is_visible,
	.read = aquastream_read,
	.read_string = aquastream_read_labels,
};

static const struct hwmon_chip_info aquastream_ultimate_chip_info = {
	.ops = &aquastream_hwmon_ops,
	.info = aquastream_ultimate_info,
};

static int aquastream_raw_event(struct hid_device *hdev,
		struct hid_report *report, u8 *data, int size)
{
	struct aquastream_device_data *ldata;

	/* TODO we only want one report, specify it in hid_driver */
	if (report->id != AQUASTREAM_ULTIMATE_STATUS_REPORT_ID || size != sizeof(struct aquastream_ultimate_report_1)) {
		return 0;
	}

	ldata = hid_get_drvdata(hdev);

	/* TODO reads don't need the latest data, but each store must be atomic */
	switch (hdev->product) {
		case AQUASTREAM_ULTIMATE_PRODUCT_ID:
			memcpy(ldata->report_1, data, sizeof(struct aquastream_ultimate_report_1));
			break;
		default:
			return 0;
	}
	return 0;
}

static int ultmt_initialize(struct hid_device *hdev)
{
	/* TODO ?? */
	return 0;
}

static const struct hid_device_id aquastream_table[] = {
	{ HID_USB_DEVICE(AQUA_COMPUTER_VENDOR_ID, AQUASTREAM_ULTIMATE_PRODUCT_ID) },
	{ }
};

MODULE_DEVICE_TABLE(hid, aquastream_table);

static int aquastream_probe(struct hid_device *hdev,
		const struct hid_device_id *id)
{
	struct aquastream_device_data *ldata;
	struct device *hwmon_dev;
	const struct hwmon_chip_info *chip_info;
	char *chip_name;
	int ret;

	ldata = devm_kzalloc(&hdev->dev, sizeof(*ldata), GFP_KERNEL);
	if (!ldata)
		return -ENOMEM;

	switch (hdev->product) {
		case AQUASTREAM_ULTIMATE_PRODUCT_ID:
			chip_name = "aquastream_ultimate";
			chip_info = &aquastream_ultimate_chip_info;
			break;
		default:
			return -EINVAL;
	}
	hid_info(hdev, "device: %s\n", chip_name);

	ldata->report_1 = devm_kcalloc(&hdev->dev, 1,
			sizeof(struct aquastream_ultimate_report_1), GFP_KERNEL);
	if (!ldata->report_1)
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

	ret = ultmt_initialize(hdev);
	if (ret) {
		hid_err(hdev, "failed to initialize device");
		goto rec_close_hid;
	}

	hid_info(hdev, "probing successful\n");
	return 0;

rec_close_hid:
	hid_hw_close(hdev);
rec_stop_hid:
	hid_hw_stop(hdev);
	return ret;
}

static void aquastream_remove(struct hid_device *hdev)
{
	hid_hw_close(hdev);
	hid_hw_stop(hdev);
}

static struct hid_driver aquastream_driver = {
	.name = DRVNAME,
	.id_table = aquastream_table,
	.probe = aquastream_probe,
	.remove = aquastream_remove,
	.raw_event = aquastream_raw_event,
};

module_hid_driver(aquastream_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Matthias Groß <grmat@sub.red>");
MODULE_DESCRIPTION("hwmon driver for Aqua Computer aquastream ULTIMATE");
