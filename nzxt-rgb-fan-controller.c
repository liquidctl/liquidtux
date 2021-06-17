// SPDX-License-Identifier: GPL-2.0-or-later
/*
 *  Copyright (c) 2021 Aleksandr Mezin
 */

#include <linux/completion.h>
#include <linux/hid.h>
#include <linux/hwmon.h>
#include <linux/module.h>
#include <asm/byteorder.h>
#include <asm/unaligned.h>

/*
 * The device has only 3 fan channels/connectors. But all HID reports have
 * space reserved for up to 8 channels.
 */
#define FAN_CHANNELS 3
#define FAN_CHANNELS_MAX 8

#define UPDATE_INTERVAL_PRECISION_MS 250
#define UPDATE_INTERVAL_DEFAULT_MS 1000
#define INITIAL_REPORT_TIMEOUT_MS 1000

enum {
	INPUT_REPORT_ID_FAN_STATUS = 0x67,
};

enum {
	FAN_STATUS_REPORT_SPEED = 0x02,
	FAN_STATUS_REPORT_VOLTAGE = 0x04,
};

enum {
	FAN_TYPE_NONE = 0,
	FAN_TYPE_DC = 1,
	FAN_TYPE_PWM = 2,
};

struct fan_status_report {
	/* report_id should be INPUT_REPORT_ID_STATUS = 0x67 */
	uint8_t report_id;
	/* FAN_STATUS_REPORT_SPEED = 0x02 or FAN_STATUS_REPORT_VOLTAGE = 0x04 */
	uint8_t type;
	/* Some configuration data? Stays the same after fan speed changes,
	 * changes in fan configuration, reboots and driver reloads.
	 * Byte 12 seems to be the number of fan channels, but I am not sure.
	 */
	uint8_t unknown1[14];
	/* Fan type as detected by the device. See FAN_TYPE_* enum. */
	uint8_t fan_type[FAN_CHANNELS_MAX];

	union {
		/* When type == FAN_STATUS_REPORT_SPEED */
		struct {
			/* Fan speed, in RPM. Zero for channels without fans connected. */
			__le16 fan_rpm[FAN_CHANNELS_MAX];
			/* Fan duty cycle, in percent. Non-zero even for channels without fans connected. */
			uint8_t duty_percent[FAN_CHANNELS_MAX];
			/* Exactly the same values as duty_percent[], non-zero for disconnected fans too. */
			uint8_t duty_percent_dup[FAN_CHANNELS_MAX];
			/* "Case Noise" in db */
			uint8_t noise_db;
		} __packed fan_speed;
		/* When type == FAN_STATUS_REPORT_VOLTAGE */
		struct {
			/* Voltage, in millivolts. Non-zero even when fan is not connected */
			__le16 fan_in[FAN_CHANNELS_MAX];
			/* Current, in milliamperes. Near-zero when disconnected */
			__le16 fan_current[FAN_CHANNELS_MAX];
		} __packed fan_voltage;
	} __packed;
} __packed;

#define OUTPUT_REPORT_SIZE 64

enum {
	OUTPUT_REPORT_ID_INIT_COMMAND = 0x60,
	OUTPUT_REPORT_ID_SET_FAN_SPEED = 0x62,
};

enum {
	INIT_COMMAND_SET_UPDATE_INTERVAL = 0x02,
	INIT_COMMAND_DETECT_FANS = 0x03,
};

struct set_fan_speed_report {
	/* report_id should be OUTPUT_REPORT_ID_SET_FAN_SPEED = 0x62 */
	uint8_t report_id;
	/* Should be 0x01 */
	uint8_t magic;
	/* To change fan speed on i-th channel, set i-th bit here */
	uint8_t channel_bit_mask;
	/* Fan duty cycle/target speed in percent */
	uint8_t duty_percent[FAN_CHANNELS_MAX];
} __packed;

struct fan_channel_status {
	uint8_t type;
	uint8_t duty_percent;
	uint16_t rpm;
	uint16_t in;
	uint16_t curr;
};

struct drvdata {
	struct hid_device *hid;
	struct device *hwmon;
	struct completion status_received;
	struct fan_channel_status fan[FAN_CHANNELS];
	long update_interval;
};

static long scale_value(long val, long orig_max, long new_max)
{
	if (val <= 0)
		return 0;

	if (val >= orig_max)
		return new_max;

	val *= new_max;

	if ((val % orig_max) * 2 >= orig_max)
		return val / orig_max + 1;
	else
		return val / orig_max;
}

static void handle_fan_status_report(struct drvdata *drvdata, void *data,
				     int size)
{
	struct fan_status_report *report = data;
	int i;

	if (size < sizeof(struct fan_status_report))
		return;

	switch (report->type) {
	case FAN_STATUS_REPORT_SPEED:
		for (i = 0; i < FAN_CHANNELS; i++) {
			struct fan_channel_status *fan = &drvdata->fan[i];

			fan->type = report->fan_type[i];
			fan->rpm = get_unaligned_le16(
				&report->fan_speed.fan_rpm[i]);
			fan->duty_percent = report->fan_speed.duty_percent[i];
		}

		if (!completion_done(&drvdata->status_received))
			complete_all(&drvdata->status_received);

		return;
	case FAN_STATUS_REPORT_VOLTAGE:
		for (i = 0; i < FAN_CHANNELS; i++) {
			struct fan_channel_status *fan = &drvdata->fan[i];

			fan->type = report->fan_type[i];
			fan->in = get_unaligned_le16(
				&report->fan_voltage.fan_in[i]);
			fan->curr = get_unaligned_le16(
				&report->fan_voltage.fan_current[i]);
		}
		return;
	default:
		return;
	}
}

static umode_t hwmon_is_visible(const void *data, enum hwmon_sensor_types type,
				u32 attr, int channel)
{
	switch (type) {
	case hwmon_pwm:
		switch (attr) {
		case hwmon_pwm_input:
		case hwmon_pwm_enable:
			return 0644;

		default:
			return 0444;
		}

	case hwmon_chip:
		switch (attr) {
		case hwmon_chip_update_interval:
			return 0644;

		default:
			return 0444;
		}

	default:
		return 0444;
	}
}

static int hwmon_read(struct device *dev, enum hwmon_sensor_types type,
		      u32 attr, int channel, long *val)
{
	struct drvdata *drvdata = dev_get_drvdata(dev);
	struct fan_channel_status *fan;

	if (type == hwmon_chip) {
		switch (attr) {
		case hwmon_chip_update_interval:
			*val = drvdata->update_interval;
			return 0;

		default:
			return -EINVAL;
		}
	}

	if (channel < 0 || channel >= FAN_CHANNELS)
		return -EINVAL;

	fan = &drvdata->fan[channel];

	switch (type) {
	case hwmon_fan:
		switch (attr) {
		case hwmon_fan_input:
			*val = fan->rpm;
			return 0;

		default:
			return -EINVAL;
		}

	case hwmon_pwm:
		switch (attr) {
		case hwmon_pwm_enable:
			*val = fan->type != FAN_TYPE_NONE;
			return 0;

		case hwmon_pwm_mode:
			*val = fan->type == FAN_TYPE_PWM;
			return 0;

		case hwmon_pwm_input:
			*val = scale_value(fan->duty_percent, 100, 255);
			return 0;

		default:
			return -EINVAL;
		}

	case hwmon_in:
		switch (attr) {
		case hwmon_in_input:
			*val = fan->in;
			return 0;

		default:
			return -EINVAL;
		}

	case hwmon_curr:
		switch (attr) {
		case hwmon_curr_input:
			*val = fan->curr;
			return 0;

		default:
			return -EINVAL;
		}

	default:
		return -EINVAL;
	}
}

static int send_output_report(struct hid_device *hdev, const void *data,
			      size_t data_size)
{
	void *buffer;
	int ret;

	if (data_size > OUTPUT_REPORT_SIZE)
		return -EINVAL;

	buffer = kzalloc(OUTPUT_REPORT_SIZE, GFP_KERNEL);

	if (!buffer)
		return -ENOMEM;

	memcpy(buffer, data, data_size);
	ret = hid_hw_output_report(hdev, buffer, OUTPUT_REPORT_SIZE);
	kfree(buffer);
	return ret < 0 ? ret : 0;
}

static int set_pwm(struct drvdata *drvdata, int channel, long val)
{
	int ret;
	uint8_t duty_percent = scale_value(val, 255, 100);

	struct set_fan_speed_report report = {
		.report_id = OUTPUT_REPORT_ID_SET_FAN_SPEED,
		.magic = 1,
		.channel_bit_mask = 1 << channel
	};

	report.duty_percent[channel] = duty_percent;
	ret = send_output_report(drvdata->hid, &report, sizeof(report));

	if (ret == 0) {
		/* pwmconfig and fancontrol scripts expect pwm writes to take
		 * effect immediately (i. e. read from pwm* sysfs should return
		 * the value written into it). The device seems to always
		 * accept pwm values - even when there is no fan connected - so
		 * update pwm status without waiting for a report, to make
		 * pwmconfig and fancontrol happy.
		 *
		 * This avoids "fan stuck" messages from pwmconfig, and
		 * fancontrol setting fan speed to 100% during shutdown.
		 */
		drvdata->fan[channel].duty_percent = duty_percent;
	}

	return ret;
}

static int set_pwm_enable(struct drvdata *drvdata, int channel, long val)
{
	/* Workaround for fancontrol/pwmconfig trying to write to pwm*_enable
	 * even if it already is 1.
	 */

	struct fan_channel_status *fan = &drvdata->fan[channel];
	long expected_val = fan->type != FAN_TYPE_NONE;

	return (val == expected_val) ? 0 : -ENOTSUPP;
}

static int set_update_interval(struct drvdata *drvdata, long val)
{
	uint8_t val_transformed =
		max(val / UPDATE_INTERVAL_PRECISION_MS, 1L) - 1;
	uint8_t report[] = {
		OUTPUT_REPORT_ID_INIT_COMMAND,
		INIT_COMMAND_SET_UPDATE_INTERVAL,
		0x01,
		0xe8,
		val_transformed,
		0x01,
		0xe8,
		val_transformed,
	};

	int ret;

	ret = send_output_report(drvdata->hid, report, sizeof(report));
	if (ret)
		return ret;

	drvdata->update_interval =
		(val_transformed + 1) * UPDATE_INTERVAL_PRECISION_MS;
	return 0;
}

static int detect_fans(struct hid_device *hdev)
{
	uint8_t report[] = {
		OUTPUT_REPORT_ID_INIT_COMMAND,
		INIT_COMMAND_DETECT_FANS,
	};

	return send_output_report(hdev, report, sizeof(report));
}

static int init_device(struct drvdata *drvdata, long update_interval)
{
	int ret;

	ret = detect_fans(drvdata->hid);
	if (ret)
		return ret;

	reinit_completion(&drvdata->status_received);

	ret = set_update_interval(drvdata, 0);
	if (ret)
		return ret;

	if (!wait_for_completion_timeout(&drvdata->status_received,
					 INITIAL_REPORT_TIMEOUT_MS))
		return -ETIMEDOUT;

	return set_update_interval(drvdata, update_interval);
}

static int hwmon_write(struct device *dev, enum hwmon_sensor_types type,
		       u32 attr, int channel, long val)
{
	struct drvdata *drvdata = dev_get_drvdata(dev);

	switch (type) {
	case hwmon_pwm:
		if (channel < 0 || channel >= FAN_CHANNELS)
			return -EINVAL;

		switch (attr) {
		case hwmon_pwm_enable:
			return set_pwm_enable(drvdata, channel, val);

		case hwmon_pwm_input:
			return set_pwm(drvdata, channel, val);

		default:
			return -EINVAL;
		}

	case hwmon_chip:
		switch (attr) {
		case hwmon_chip_update_interval:
			return set_update_interval(drvdata, val);

		default:
			return -EINVAL;
		}

	default:
		return -EINVAL;
	}
}

static const struct hwmon_ops hwmon_ops = {
	.is_visible = hwmon_is_visible,
	.read = hwmon_read,
	.write = hwmon_write,
};

static const struct hwmon_channel_info *channel_info[] = {
	HWMON_CHANNEL_INFO(fan, HWMON_F_INPUT, HWMON_F_INPUT, HWMON_F_INPUT),
	HWMON_CHANNEL_INFO(pwm,
			   HWMON_PWM_INPUT | HWMON_PWM_MODE | HWMON_PWM_ENABLE,
			   HWMON_PWM_INPUT | HWMON_PWM_MODE | HWMON_PWM_ENABLE,
			   HWMON_PWM_INPUT | HWMON_PWM_MODE | HWMON_PWM_ENABLE),
	HWMON_CHANNEL_INFO(in, HWMON_I_INPUT, HWMON_I_INPUT, HWMON_I_INPUT),
	HWMON_CHANNEL_INFO(curr, HWMON_C_INPUT, HWMON_C_INPUT, HWMON_C_INPUT),
	HWMON_CHANNEL_INFO(chip, HWMON_C_UPDATE_INTERVAL),
	NULL
};

static const struct hwmon_chip_info chip_info = {
	.ops = &hwmon_ops,
	.info = channel_info,
};

static int hid_raw_event(struct hid_device *hdev, struct hid_report *report,
			 u8 *data, int size)
{
	struct drvdata *drvdata = hid_get_drvdata(hdev);
	uint8_t report_id = *data;

	if (report_id == INPUT_REPORT_ID_FAN_STATUS)
		handle_fan_status_report(drvdata, data, size);

	return 0;
}

static int hid_reset_resume(struct hid_device *hdev)
{
	struct drvdata *drvdata = hid_get_drvdata(hdev);

	return init_device(drvdata, drvdata->update_interval);
}

static int hid_probe(struct hid_device *hdev, const struct hid_device_id *id)
{
	struct drvdata *drvdata;
	int ret;

	drvdata = devm_kzalloc(&hdev->dev, sizeof(struct drvdata), GFP_KERNEL);
	if (!drvdata)
		return -ENOMEM;

	drvdata->hid = hdev;
	hid_set_drvdata(hdev, drvdata);
	init_completion(&drvdata->status_received);

	ret = hid_parse(hdev);
	if (ret)
		return ret;

	ret = hid_hw_start(hdev, HID_CONNECT_HIDRAW);
	if (ret)
		return ret;

	ret = hid_hw_open(hdev);
	if (ret)
		goto out_hw_stop;

	hid_device_io_start(hdev);

	init_device(drvdata, UPDATE_INTERVAL_DEFAULT_MS);

	drvdata->hwmon =
		hwmon_device_register_with_info(&hdev->dev,
						"nzxt_rgb_fan_controller",
						drvdata, &chip_info, NULL);
	if (IS_ERR(drvdata->hwmon)) {
		ret = PTR_ERR(drvdata->hwmon);
		goto out_hw_close;
	}

	return 0;

out_hw_close:
	hid_hw_close(hdev);

out_hw_stop:
	hid_hw_stop(hdev);
	return ret;
}

static void hid_remove(struct hid_device *hdev)
{
	struct drvdata *drvdata = hid_get_drvdata(hdev);

	hwmon_device_unregister(drvdata->hwmon);

	hid_hw_close(hdev);
	hid_hw_stop(hdev);
}

static const struct hid_device_id hid_id_table[] = {
	{ HID_USB_DEVICE(0x1e71, 0x2009) },
	{},
};

static struct hid_driver hid_driver = {
	.name = "nzxt_rgb_fan_controller",
	.id_table = hid_id_table,
	.probe = hid_probe,
	.remove = hid_remove,
	.raw_event = hid_raw_event,
#ifdef CONFIG_PM
	.reset_resume = hid_reset_resume,
#endif
};

static int __init nzxt_rgb_fan_controller_init(void)
{
	return hid_register_driver(&hid_driver);
}

static void __exit nzxt_rgb_fan_controller_exit(void)
{
	hid_unregister_driver(&hid_driver);
}

MODULE_DEVICE_TABLE(hid, hid_id_table);
MODULE_AUTHOR("Aleksandr Mezin <mezin.alexander@gmail.com>");
MODULE_DESCRIPTION("Driver for NZXT RGB & Fan controller");
MODULE_LICENSE("GPL");

late_initcall(nzxt_rgb_fan_controller_init);
module_exit(nzxt_rgb_fan_controller_exit);
