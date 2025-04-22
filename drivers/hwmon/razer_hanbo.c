// SPDX-License-Identifier: GPL-2.0+
/*
 * hwmon driver for Razer Hanbo Chroma AIO CPU coolers.
 *
 * Copyright 2025 Joseph East <eastyjr@gmail.com>
 */

#include <linux/debugfs.h>
#include <linux/hid.h>
#include <linux/hwmon.h>
#include <linux/hwmon-sysfs.h>
#include <linux/jiffies.h>
#include <linux/module.h>
#include <linux/spinlock.h>
#include <linux/unaligned.h>

#define DRIVER_NAME			"razer_hanbo"

/* Device parameters */
#define USB_VENDOR_ID_RAZER		0x1532
#define USB_PRODUCT_ID_HANBO		0x0f35

#define STATUS_VALIDITY_MS		(2 * 1000)
#define MAX_REPORT_LENGTH		64
#define DUTY_CYCLE_MIN			20
#define DUTY_CYCLE_MAX			100
#define TEMPERATURE_MAX			100
#define CUSTOM_CURVE_POINTS		9

/* Firmware command response signatures */
#define FIRMWARE_STATUS_REPORT_ID	0x02
#define PUMP_STATUS_REPORT_ID		0x13
#define PUMP_PROFILE_ACK_REPORT_ID	0x15
#define PUMP_CURVE_ACK_REPORT_ID	0x19
#define FAN_STATUS_REPORT_ID		0x21
#define FAN_PROFILE_ACK_REPORT_ID	0x23
#define BRIGHTNESS_ACK_REPORT_ID	0x71
#define BRIGHTNESS_STATUS_REPORT_ID	0x73
#define RGB_MODE_SET_ACK_REPORT_ID	0x81
#define RGB_MODE_STATUS_REPORT_ID	0x83
#define CPU_TEMP_ACK_REPORT_ID		0xC1
#define FAN_CURVE_ACK_REPORT_ID		0xC9

/* Firmware commands and templates */
static const u8 get_firmware_ver_cmd[] = { 0x01, 0x01 };
static const u8 get_pump_status_cmd[] = { 0x12, 0x01 };
static const u8 set_pump_fan_cmd_template[] = { 0x14, 0x01, 0x00, 0x00 };
static const u8 get_fan_status_cmd[] = { 0x20, 0x01 };
static const u8 set_vcpu_temp_cmd_template[] = { 0xc0, 0x01, 0x00, 0x00, 0x1e, 0x00 };
static const u8 set_pump_fan_curve_cmd_template[] = {
	0x18, 0x01, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };
static const u8 default_fan_curve[] = { 0x18, 0x1e, 0x28, 0x30, 0x3c, 0x51, 0x64, 0x64, 0x64 };
static const u8 default_pump_curve[] = { 0x14, 0x28, 0x3c, 0x50, 0x64, 0x64, 0x64, 0x64, 0x64 };
static const u8 profile_base_duties[] = { 0x00, 0x14, 0x32, 0x50 };
static const u8 ack_header_type_a[] = { 0x00, 0x02, 0x01, 0x00 };
static const u8 ack_header_type_b[] = { 0x00, 0x02, 0x02, 0x01 };

/* Firmware command lengths and offsets */
#define GET_STATUS_CMD_LENGTH		2
#define GET_FIRMWARE_VER_CMD_LENGTH	2
#define SET_PROFILE_CMD_LENGTH		4
#define SET_CURVE_CMD_LENGTH		13
#define SET_CPU_TEMP_CMD_LENGTH		6
#define SERIAL_NUMBER_LENGTH		15
#define SET_PROFILE_ID_OFFSET		2
#define SET_PROFILE_PWM_OFFSET		3
#define SET_CPU_TEMP_PAYLOAD_OFFSET	2
#define FIRMWARE_VERSION_OFFSET		29
#define SERIAL_NUMBER_OFFSET		3
#define CURVE_PAYLOAD_OFFSET		4
#define SHORT_ACK_LENGTH		2
#define REG_ACK_LENGTH			3
#define LONG_ACK_LENGTH			4

/* Convenience labels specific to this driver */
#define PUMP_CHANNEL			0
#define FAN_CHANNEL			1
#define QUIET_PROFILE_ID		1
#define CURVE_PROFILE_ID		4

static const char *const hanbo_temp_label[] = {
	"Coolant temp",
	"Reference temp"
};

static const char *const hanbo_speed_label[] = {
	"Pump speed",
	"Fan speed"
};

/* Convenience structure for storing PWM info */
struct hanbo_pwm_channel {
	u16 tacho;
	u8 commanded_pwm;
	u8 attained_pwm;
	u8 active_profile;
	u8 pwm_points[CUSTOM_CURVE_POINTS];
	u8 profile_sticky;
};

/* Global data structure for HID and hwmon functions */
struct hanbo_data {
	struct hid_device *hdev;
	struct device *hwmon_dev;
	struct dentry *debugfs;
	/* For locking access to buffer */
	struct mutex buffer_lock;
	/* For queueing multiple readers */
	struct mutex status_report_request_mutex;
	/* For reinitializing the completion below */
	spinlock_t status_report_request_lock;
	struct completion status_report_received;
	struct completion fw_version_processed;
	/* Sensor data */
	u32 temp_input[2];
	struct hanbo_pwm_channel channel_info[2];
	/* Staging buffer for sending HID packets */
	u8 *buffer;
	u8 firmware_version[6];
	char serial_number[15];
	unsigned long updated;	/* jiffies */
};

/* Validates the internal layout of a report, not the contents */
static int hanbo_hid_validate_header(int header_size, const u8 *data,
				     int eop_offset)
{
	int i;
	u8 header[LONG_ACK_LENGTH];

	switch (header_size) {
	case SHORT_ACK_LENGTH:
	case REG_ACK_LENGTH:
		memcpy(header, ack_header_type_a, REG_ACK_LENGTH);
		break;
	case LONG_ACK_LENGTH:
		memcpy(header, ack_header_type_b, LONG_ACK_LENGTH);
		break;
	default:
		return -EPROTO;
	}
	for (i = 1; i < header_size; i++) {
		if (header[i] != data[i])
			return -EPROTO;
	}
	for (i = eop_offset; i < MAX_REPORT_LENGTH; i++) {
		if (data[i] != 0)
			return -EPROTO;
	}
	return 0;
}

/* Write a command to the device with zero padding the report size */
static int hanbo_hid_write_expanded(struct hanbo_data *priv, const u8 *cmd,
				    int cmd_length)
{
	int ret;

	mutex_lock(&priv->buffer_lock);
	memcpy_and_pad(priv->buffer, MAX_REPORT_LENGTH, cmd, cmd_length, 0x00);
	ret = hid_hw_output_report(priv->hdev, priv->buffer, MAX_REPORT_LENGTH);
	mutex_unlock(&priv->buffer_lock);
	return ret;
}

/* Convenience function to declutter hanbo_hwmon_read() */
static int hanbo_hid_get_status(struct hanbo_data *priv)
{
	int ret = mutex_lock_interruptible(&priv->status_report_request_mutex);

	if (ret < 0)
		return ret;
	/* Data is up to date */
	if (!time_after(jiffies, priv->updated + msecs_to_jiffies(STATUS_VALIDITY_MS)))
		goto unlock_and_return;
	/*
	 * Disable raw event parsing for a moment to safely reinitialize the
	 * completion. Reinit is done because hidraw could have triggered
	 * the raw event parsing and marked the priv->status_report_received
	 * completion as done. This is done per transaction.
	 */
	spin_lock_bh(&priv->status_report_request_lock);
	reinit_completion(&priv->status_report_received);
	spin_unlock_bh(&priv->status_report_request_lock);

	/* Send status requests - Fans */
	ret = hanbo_hid_write_expanded(priv, get_fan_status_cmd, GET_STATUS_CMD_LENGTH);
	if (ret < 0)
		goto unlock_and_return;
	ret = wait_for_completion_interruptible_timeout(&priv->status_report_received,
							msecs_to_jiffies(STATUS_VALIDITY_MS));
	if (ret == 0)
		ret = -ETIMEDOUT;
	/* Then pump */
	spin_lock_bh(&priv->status_report_request_lock);
	reinit_completion(&priv->status_report_received);
	spin_unlock_bh(&priv->status_report_request_lock);

	ret = hanbo_hid_write_expanded(priv, get_pump_status_cmd, GET_STATUS_CMD_LENGTH);
	if (ret < 0)
		goto unlock_and_return;
	ret = wait_for_completion_interruptible_timeout(&priv->status_report_received,
							msecs_to_jiffies(STATUS_VALIDITY_MS));
	if (ret == 0)
		ret = -ETIMEDOUT;
unlock_and_return:
	/* If we've failed to send for whatever reason, cancel the completion */
	if (ret < 0) {
		spin_lock(&priv->status_report_request_lock);
		if (!completion_done(&priv->status_report_received))
			complete_all(&priv->status_report_received);
		spin_unlock(&priv->status_report_request_lock);
	}
	mutex_unlock(&priv->status_report_request_mutex);
	return ret;
}

/* Convenience function to declutter hanbo_hwmon_write() */
static int hanbo_hid_profile_send(struct hanbo_data *priv, int channel,
				  u8 profile)
{
	int ret = 0;
	u8 set_profile_cmd[SET_CURVE_CMD_LENGTH];

	if (channel < PUMP_CHANNEL || channel > FAN_CHANNEL)
		return -EINVAL; /* sysfs unreachable */
	if (profile < QUIET_PROFILE_ID || profile > CURVE_PROFILE_ID)
		return -EINVAL;
	if (profile == CURVE_PROFILE_ID) {
		memcpy(set_profile_cmd, set_pump_fan_curve_cmd_template, SET_CURVE_CMD_LENGTH);
		/* Templates come with pump commands, replace with fan commands */
		if (channel == FAN_CHANNEL) {
			set_profile_cmd[0] = 0xc8;
			set_profile_cmd[2] = 0x00;
		}
		int i;
		/*
		 * Sanity check curve profile, PWM duty cycles cannot decrease
		 * the higher up the curve they are.
		 */
		for (i = SET_CURVE_CMD_LENGTH - 1; i > CURVE_PAYLOAD_OFFSET - 1; i--) {
			set_profile_cmd[i] =
				priv->channel_info[channel].pwm_points[i - CURVE_PAYLOAD_OFFSET];
			if (i != SET_CURVE_CMD_LENGTH - 1 &&
			    set_profile_cmd[i + 1] < set_profile_cmd[i])
				ret = -EINVAL;
		}
		if (ret < 0)
			return ret;
		ret = hanbo_hid_write_expanded(priv, set_profile_cmd, SET_CURVE_CMD_LENGTH);
		priv->channel_info[channel].profile_sticky = true;
	} else { /* sending a profile */
		memcpy(set_profile_cmd, set_pump_fan_cmd_template, SET_PROFILE_CMD_LENGTH);
		/* Templates come with pump commands, replace with fan commands */
		if (channel == FAN_CHANNEL)
			set_profile_cmd[0] = 0x22;
		set_profile_cmd[SET_PROFILE_ID_OFFSET] = profile;
		/* Technically this value does nothing, kept as OEM software sends it */
		set_profile_cmd[SET_PROFILE_PWM_OFFSET] = profile_base_duties[profile];
		ret = hanbo_hid_write_expanded(priv, set_profile_cmd, SET_PROFILE_CMD_LENGTH);
		priv->channel_info[channel].profile_sticky = false;
	}
	if (ret >= 0)
		priv->channel_info[channel].active_profile = profile;
	return ret;
}

/* Set hwmon sysfs nodes, see documentation for rationale */
static umode_t hanbo_hwmon_is_visible(const void *data,
				      enum hwmon_sensor_types type,
				      u32 attr, int channel)
{
	switch (type) {
	case hwmon_temp:
		switch (attr) {
		case hwmon_temp_label:
			return 0444;
		case hwmon_temp_input:
			if (channel == FAN_CHANNEL)
				return 0644;
			return 0444;
		default:
			break;
		}
		break;
	case hwmon_fan:
		switch (attr) {
		case hwmon_fan_label:
		case hwmon_fan_input:
			return 0444;
		default:
			break;
		}
		break;
	case hwmon_pwm:
		switch (attr) {
		case hwmon_pwm_input:
			return 0444;
		case hwmon_pwm_enable:
			return 0644;
		default:
			break;
		}
		break;
	default:
		break;
	}
	return 0;
}

static int hanbo_hwmon_read(struct device *dev, enum hwmon_sensor_types type,
			    u32 attr, int channel, long *val)
{
	struct hanbo_data *priv = dev_get_drvdata(dev);
	int ret = hanbo_hid_get_status(priv);

	if (ret < 0)
		return ret;
	switch (type) {
	case hwmon_temp:
		*val = priv->temp_input[channel];
		break;
	case hwmon_fan:
		*val = priv->channel_info[channel].tacho;
		break;
	case hwmon_pwm:
		switch (attr) {
		case hwmon_pwm_input:
			*val = ((int)(priv->channel_info[channel].attained_pwm) & 0xFF);
			break;
		case hwmon_pwm_enable:
			*val = priv->channel_info[channel].active_profile;
			break;
		default:
			return -EOPNOTSUPP; /* sysfs unreachable */
		}
		break;
	default:
		return -EOPNOTSUPP; /* sysfs unreachable */
	}
	if (ret > 0)
		return 0;
	return ret;
}

static int hanbo_hwmon_read_string(struct device *dev,
				   enum hwmon_sensor_types type, u32 attr,
				   int channel, const char **str)
{
	switch (type) {
	case hwmon_temp:
		*str = hanbo_temp_label[channel];
		break;
	case hwmon_fan:
		*str = hanbo_speed_label[channel];
		break;
	default:
		return -EOPNOTSUPP; /* sysfs unreachable */
	}
	return 0;
}

static int hanbo_hwmon_write(struct device *dev, enum hwmon_sensor_types type,
			     u32 attr, int channel, long val)
{
	struct hanbo_data *priv = dev_get_drvdata(dev);
	long degrees_c;
	int ret = mutex_lock_interruptible(&priv->status_report_request_mutex);

	if (ret < 0)
		return ret;
	/*
	 * As writes generate acknowledgment reports the spinlock pattern
	 * is used here to satisfy that we see them through.
	 */
	spin_lock_bh(&priv->status_report_request_lock);
	reinit_completion(&priv->status_report_received);
	spin_unlock_bh(&priv->status_report_request_lock);

	switch (type) {
	case hwmon_temp: /* Set CPU reference temperature */
		switch (attr) {
		case hwmon_temp_input:
			u8 set_cpu_temp_cmd[SET_CPU_TEMP_CMD_LENGTH];

			/* Clamp out of range CPU temperatures */
			if (val < 0) {
				degrees_c = 0;
			} else {
				degrees_c = DIV_ROUND_CLOSEST(val, 1000);
				if (degrees_c > TEMPERATURE_MAX)
					degrees_c = TEMPERATURE_MAX;
			}
			memcpy(set_cpu_temp_cmd, set_vcpu_temp_cmd_template,
			       SET_CPU_TEMP_CMD_LENGTH);
			set_cpu_temp_cmd[SET_CPU_TEMP_PAYLOAD_OFFSET] = degrees_c & 0xFF;
			ret = hanbo_hid_write_expanded(priv, set_cpu_temp_cmd,
						       SET_CPU_TEMP_CMD_LENGTH);
			if (ret < 0)
				goto unlock_and_return;
			/* Store the final value for reading via sysfs */
			priv->temp_input[1] = degrees_c * 1000;
			break;

		default: /* sysfs unreachable */
			ret = -EOPNOTSUPP;
			goto unlock_and_return;
		}
		break;
	case hwmon_pwm: /* Set a profile */
		switch (attr) {
		case hwmon_pwm_enable:
			ret = hanbo_hid_profile_send(priv, channel, val & 0xFF);
			break;
		default: /* sysfs unreachable */
			ret = -EOPNOTSUPP;
		}
		break;
	default: /* sysfs unreachable */
		ret = -EOPNOTSUPP;
	}

unlock_and_return:
	/* If we've failed to send for whatever reason, cancel the completion */
	if (ret < 0) {
		spin_lock(&priv->status_report_request_lock);
		if (!completion_done(&priv->status_report_received))
			complete_all(&priv->status_report_received);
		spin_unlock(&priv->status_report_request_lock);
	}
	mutex_unlock(&priv->status_report_request_mutex);
	if (ret > 0)
		return 0;
	return ret;
}

/*
 * Consumes curve points from sysfs and stores in global struct.
 * Custom attribute
 */
static ssize_t hanbo_fan_curve_pwm_store(struct device *dev,
					 struct device_attribute *attr,
					 const char *buf, size_t count)
{
	struct sensor_device_attribute_2 *dev_attr = to_sensor_dev_attr_2(attr);
	struct hanbo_data *priv = dev_get_drvdata(dev);
	long val;

	if (kstrtol(buf, 10, &val) < 0)
		return -EINVAL;

	if (val < DUTY_CYCLE_MIN)
		val = DUTY_CYCLE_MIN;

	if (val > DUTY_CYCLE_MAX)
		val = DUTY_CYCLE_MAX;

	priv->channel_info[dev_attr->nr].pwm_points[dev_attr->index] = val & 0xFF;
	return count;
}

/*
 * Presents internal PWM set points from firmware to sysfs, for interest.
 * Custom attribute
 */
static ssize_t hanbo_pwm_setpoint_show(struct device *dev,
				       struct device_attribute *attr,
				       char *buf)
{
	struct sensor_device_attribute *dev_attr = to_sensor_dev_attr(attr);
	struct hanbo_data *priv = dev_get_drvdata(dev);
	u8 value = priv->channel_info[dev_attr->index].commanded_pwm;

	return sysfs_emit(buf, "%d\n", value);
}

/*
 * Define custom attributes for pump and fan curves. Describes 9 points,
 * (10 degrees apart defined in hardware) representing 20C to 100C.
 */
static SENSOR_DEVICE_ATTR_2_WO(temp1_auto_point1_pwm, hanbo_fan_curve_pwm, 0, 0);
static SENSOR_DEVICE_ATTR_2_WO(temp1_auto_point2_pwm, hanbo_fan_curve_pwm, 0, 1);
static SENSOR_DEVICE_ATTR_2_WO(temp1_auto_point3_pwm, hanbo_fan_curve_pwm, 0, 2);
static SENSOR_DEVICE_ATTR_2_WO(temp1_auto_point4_pwm, hanbo_fan_curve_pwm, 0, 3);
static SENSOR_DEVICE_ATTR_2_WO(temp1_auto_point5_pwm, hanbo_fan_curve_pwm, 0, 4);
static SENSOR_DEVICE_ATTR_2_WO(temp1_auto_point6_pwm, hanbo_fan_curve_pwm, 0, 5);
static SENSOR_DEVICE_ATTR_2_WO(temp1_auto_point7_pwm, hanbo_fan_curve_pwm, 0, 6);
static SENSOR_DEVICE_ATTR_2_WO(temp1_auto_point8_pwm, hanbo_fan_curve_pwm, 0, 7);
static SENSOR_DEVICE_ATTR_2_WO(temp1_auto_point9_pwm, hanbo_fan_curve_pwm, 0, 8);

static SENSOR_DEVICE_ATTR_2_WO(temp2_auto_point1_pwm, hanbo_fan_curve_pwm, 1, 0);
static SENSOR_DEVICE_ATTR_2_WO(temp2_auto_point2_pwm, hanbo_fan_curve_pwm, 1, 1);
static SENSOR_DEVICE_ATTR_2_WO(temp2_auto_point3_pwm, hanbo_fan_curve_pwm, 1, 2);
static SENSOR_DEVICE_ATTR_2_WO(temp2_auto_point4_pwm, hanbo_fan_curve_pwm, 1, 3);
static SENSOR_DEVICE_ATTR_2_WO(temp2_auto_point5_pwm, hanbo_fan_curve_pwm, 1, 4);
static SENSOR_DEVICE_ATTR_2_WO(temp2_auto_point6_pwm, hanbo_fan_curve_pwm, 1, 5);
static SENSOR_DEVICE_ATTR_2_WO(temp2_auto_point7_pwm, hanbo_fan_curve_pwm, 1, 6);
static SENSOR_DEVICE_ATTR_2_WO(temp2_auto_point8_pwm, hanbo_fan_curve_pwm, 1, 7);
static SENSOR_DEVICE_ATTR_2_WO(temp2_auto_point9_pwm, hanbo_fan_curve_pwm, 1, 8);

/* Define custom attributes to reveal internal PWM set points */
static SENSOR_DEVICE_ATTR_RO(pwm1_setpoint, hanbo_pwm_setpoint, 0);
static SENSOR_DEVICE_ATTR_RO(pwm2_setpoint, hanbo_pwm_setpoint, 1);

static struct attribute *hanbo_curve_attrs[] = {
	/* Pump control curve */
	&sensor_dev_attr_temp1_auto_point1_pwm.dev_attr.attr,
	&sensor_dev_attr_temp1_auto_point2_pwm.dev_attr.attr,
	&sensor_dev_attr_temp1_auto_point3_pwm.dev_attr.attr,
	&sensor_dev_attr_temp1_auto_point4_pwm.dev_attr.attr,
	&sensor_dev_attr_temp1_auto_point5_pwm.dev_attr.attr,
	&sensor_dev_attr_temp1_auto_point6_pwm.dev_attr.attr,
	&sensor_dev_attr_temp1_auto_point7_pwm.dev_attr.attr,
	&sensor_dev_attr_temp1_auto_point8_pwm.dev_attr.attr,
	&sensor_dev_attr_temp1_auto_point9_pwm.dev_attr.attr,
	/* Fan control curve */
	&sensor_dev_attr_temp2_auto_point1_pwm.dev_attr.attr,
	&sensor_dev_attr_temp2_auto_point2_pwm.dev_attr.attr,
	&sensor_dev_attr_temp2_auto_point3_pwm.dev_attr.attr,
	&sensor_dev_attr_temp2_auto_point4_pwm.dev_attr.attr,
	&sensor_dev_attr_temp2_auto_point5_pwm.dev_attr.attr,
	&sensor_dev_attr_temp2_auto_point6_pwm.dev_attr.attr,
	&sensor_dev_attr_temp2_auto_point7_pwm.dev_attr.attr,
	&sensor_dev_attr_temp2_auto_point8_pwm.dev_attr.attr,
	&sensor_dev_attr_temp2_auto_point9_pwm.dev_attr.attr,
	/* Remaining information */
	&sensor_dev_attr_pwm1_setpoint.dev_attr.attr,
	&sensor_dev_attr_pwm2_setpoint.dev_attr.attr,
	NULL
};

static umode_t hanbo_curve_props_are_visible(struct kobject *kobj,
					     struct attribute *attr,
					     int index)
{
	return attr->mode;
}

static const struct attribute_group hanbo_curves_group = {
	.attrs = hanbo_curve_attrs,
	.is_visible = hanbo_curve_props_are_visible
};

static const struct attribute_group *hanbo_groups[] = {
	&hanbo_curves_group,
	NULL
};

static const struct hwmon_ops hanbo_hwmon_ops = {
	.is_visible = hanbo_hwmon_is_visible,
	.read = hanbo_hwmon_read,
	.read_string = hanbo_hwmon_read_string,
	.write = hanbo_hwmon_write
};

static const struct hwmon_channel_info *hanbo_info[] = {
	HWMON_CHANNEL_INFO(temp,
			   HWMON_T_INPUT | HWMON_T_LABEL,
			   HWMON_T_INPUT | HWMON_T_LABEL),
	HWMON_CHANNEL_INFO(fan,
			   HWMON_F_INPUT | HWMON_F_LABEL,
			   HWMON_F_INPUT | HWMON_F_LABEL),
	HWMON_CHANNEL_INFO(pwm,
			   HWMON_PWM_INPUT | HWMON_PWM_ENABLE,
			   HWMON_PWM_INPUT | HWMON_PWM_ENABLE),
	NULL
};

static const struct hwmon_chip_info hanbo_chip_info = {
	.ops = &hanbo_hwmon_ops,
	.info = hanbo_info,
};

static int firmware_version_show(struct seq_file *seqf, void *unused)
{
	struct hanbo_data *priv = seqf->private;

	seq_printf(seqf, "%s\n", priv->firmware_version);
	return 0;
}

static int serial_number_show(struct seq_file *seqf, void *unused)
{
	struct hanbo_data *priv = seqf->private;
	int i;

	for (i = 0; i < SERIAL_NUMBER_LENGTH; i++)
		seq_printf(seqf, "%c", priv->serial_number[i]);
	seq_puts(seqf, "\n");
	return 0;
}

DEFINE_SHOW_ATTRIBUTE(firmware_version);
DEFINE_SHOW_ATTRIBUTE(serial_number);

static void hanbo_debugfs_init(struct hanbo_data *priv)
{
	char name[64];

	if (priv->firmware_version[0] == '\0')
		return;	/* When here, nothing to show in debugfs */

	scnprintf(name, sizeof(name), "%s-%s", DRIVER_NAME,
		  dev_name(&priv->hdev->dev));

	priv->debugfs = debugfs_create_dir(name, NULL);
	debugfs_create_file("firmware_version", 0444, priv->debugfs, priv,
			    &firmware_version_fops);
	debugfs_create_file("serial_number", 0444, priv->debugfs, priv,
			    &serial_number_fops);
}

/*
 * Parses USB reports and splits the payload into the relevant data structures
 * at the global level for fetching.
 */
static int hanbo_raw_event(struct hid_device *hdev, struct hid_report *report,
			   u8 *data, int size)
{
	struct hanbo_data *priv = hid_get_drvdata(hdev);
	unsigned char rid;
	int ret;

	if (size != MAX_REPORT_LENGTH)
		return -EPROTO;

	rid = data[0];

	switch (rid) {
	/* Status reports with payload */
	case FIRMWARE_STATUS_REPORT_ID:
		ret = hanbo_hid_validate_header(SHORT_ACK_LENGTH, data, 34);
		if (ret < 0)
			goto fail_and_return;
		int i;
		char major = 0x30 + data[FIRMWARE_VERSION_OFFSET];
		char minor = 0x30 + (data[FIRMWARE_VERSION_OFFSET + 1] >> 4 & 0x0F);
		char patch = 0x30 + (data[FIRMWARE_VERSION_OFFSET + 1] & 0x0F);

		snprintf(priv->firmware_version, sizeof(priv->firmware_version),
			 "%c.%c.%c", major, minor, patch);
		for (i = 0; i < SERIAL_NUMBER_LENGTH; i++)
			priv->serial_number[i] = data[SERIAL_NUMBER_OFFSET + i];
		if (!completion_done(&priv->fw_version_processed))
			complete_all(&priv->fw_version_processed);
		break;
	case PUMP_STATUS_REPORT_ID:
		ret = hanbo_hid_validate_header(REG_ACK_LENGTH, data, 11);
		if (ret < 0)
			goto fail_and_return;
		priv->temp_input[0] = (data[5] * 1000) + (data[6] * 100);
		priv->channel_info[PUMP_CHANNEL].tacho = get_unaligned_be16(data + 7);
		priv->channel_info[PUMP_CHANNEL].attained_pwm = data[10];
		priv->channel_info[PUMP_CHANNEL].commanded_pwm = data[9];
		if (!priv->channel_info[PUMP_CHANNEL].profile_sticky)
			priv->channel_info[PUMP_CHANNEL].active_profile = data[3];
		break;
	case FAN_STATUS_REPORT_ID:
		ret = hanbo_hid_validate_header(LONG_ACK_LENGTH, data, 10);
		if (ret < 0)
			goto fail_and_return;
		priv->channel_info[FAN_CHANNEL].tacho = get_unaligned_be16(data + 6);
		priv->channel_info[FAN_CHANNEL].attained_pwm = data[9];
		priv->channel_info[FAN_CHANNEL].commanded_pwm = data[8];
		if (!priv->channel_info[FAN_CHANNEL].profile_sticky)
			priv->channel_info[FAN_CHANNEL].active_profile = data[4];
		break;
	/* Acknowledgment reports for commands */
	case PUMP_CURVE_ACK_REPORT_ID:
	case FAN_CURVE_ACK_REPORT_ID:
	case PUMP_PROFILE_ACK_REPORT_ID:
	case FAN_PROFILE_ACK_REPORT_ID:
	case CPU_TEMP_ACK_REPORT_ID:
	case RGB_MODE_SET_ACK_REPORT_ID:
		ret = hanbo_hid_validate_header(REG_ACK_LENGTH, data, 3);
		if (ret < 0) {
			hid_warn(hdev, "Received corrupted mode ACK report");
			goto fail_and_return;
		}
		/*
		 * Passively update driver state if usermode apps are commanding
		 * the device.
		 */
		if (rid == PUMP_CURVE_ACK_REPORT_ID) {
			priv->channel_info[PUMP_CHANNEL].active_profile = CURVE_PROFILE_ID;
			priv->channel_info[PUMP_CHANNEL].profile_sticky = true;
		} else if (rid == FAN_CURVE_ACK_REPORT_ID) {
			priv->channel_info[FAN_CHANNEL].active_profile = CURVE_PROFILE_ID;
			priv->channel_info[FAN_CHANNEL].profile_sticky = true;
		} else if (rid == PUMP_PROFILE_ACK_REPORT_ID) {
			priv->channel_info[PUMP_CHANNEL].profile_sticky = false;
		} else if (rid == FAN_PROFILE_ACK_REPORT_ID) {
			priv->channel_info[FAN_CHANNEL].profile_sticky = false;
		}
		break;
	/* Here for completeness, unlikely these are triggered from driver */
	case BRIGHTNESS_ACK_REPORT_ID:
	case BRIGHTNESS_STATUS_REPORT_ID:
	case RGB_MODE_STATUS_REPORT_ID:
		ret = hanbo_hid_validate_header(SHORT_ACK_LENGTH, data, 4);
		if (ret < 0) {
			hid_warn(hdev, "Received corrupted lighting ACK report");
			goto fail_and_return;
		}
		break;
	default:
		return -EPROTO;
	}
	spin_lock(&priv->status_report_request_lock);
	if (!completion_done(&priv->status_report_received))
		complete_all(&priv->status_report_received);
	spin_unlock(&priv->status_report_request_lock);
	priv->updated = jiffies;
fail_and_return:
	return ret;
}

static const struct hid_device_id hanbo_table[] = {
	{ HID_USB_DEVICE(USB_VENDOR_ID_RAZER, USB_PRODUCT_ID_HANBO) },
	{ }
};

MODULE_DEVICE_TABLE(hid, hanbo_table);

/* One-shot functions to perform during driver startup */
static int hanbo_drv_init(struct hid_device *hdev)
{
	struct hanbo_data *priv = hid_get_drvdata(hdev);
	int ret;

	priv->firmware_version[0] = '\0';
	ret = hanbo_hid_write_expanded(priv, get_firmware_ver_cmd,
				       GET_FIRMWARE_VER_CMD_LENGTH);
	if (ret < 0)
		return ret;

	ret = wait_for_completion_interruptible_timeout(&priv->fw_version_processed,
							msecs_to_jiffies(STATUS_VALIDITY_MS));
	if (ret == 0)
		return -ETIMEDOUT;
	else if (ret < 0)
		return ret;
	/*
	 * Set CPU reference to 30 degrees C and pre-load default curves.
	 * Curves are not sent to the AIO yet as doing so changes the profile.
	 * This allows activating profile 4 without setting each sysfs pwm node.
	 */
	enum hwmon_sensor_types mytype = hwmon_temp;
	enum hwmon_temp_attributes myattr = hwmon_temp_input;

	ret = hanbo_hwmon_write(&hdev->dev, mytype, myattr, FAN_CHANNEL, 30000);
	memcpy(priv->channel_info[FAN_CHANNEL].pwm_points, default_fan_curve, CUSTOM_CURVE_POINTS);
	memcpy(priv->channel_info[PUMP_CHANNEL].pwm_points, default_pump_curve,
	       CUSTOM_CURVE_POINTS);
	priv->channel_info[FAN_CHANNEL].profile_sticky = false;
	priv->channel_info[PUMP_CHANNEL].profile_sticky = false;
	return ret;
}

static int hanbo_probe(struct hid_device *hdev, const struct hid_device_id *id)
{
	struct hanbo_data *priv;
	int ret;

	priv = devm_kzalloc(&hdev->dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->hdev = hdev;
	hid_set_drvdata(hdev, priv);

	/*
	 * Initialize priv->updated to STATUS_VALIDITY_MS in the past, making
	 * the initial empty data invalid for hanbo_hwmon_read() without the
	 * need for a special case there.
	 */
	priv->updated = jiffies - msecs_to_jiffies(STATUS_VALIDITY_MS);

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
		return ret;
	}

	ret = hid_hw_open(hdev);
	if (ret) {
		hid_err(hdev, "hid hw open failed with %d\n", ret);
		goto fail_and_stop;
	}

	priv->buffer = devm_kzalloc(&hdev->dev, MAX_REPORT_LENGTH, GFP_KERNEL);
	if (!priv->buffer) {
		ret = -ENOMEM;
		goto fail_and_close;
	}

	mutex_init(&priv->status_report_request_mutex);
	mutex_init(&priv->buffer_lock);
	spin_lock_init(&priv->status_report_request_lock);
	init_completion(&priv->status_report_received);
	init_completion(&priv->fw_version_processed);
	hid_device_io_start(hdev);
	/*
	 * The Razer Hanbo Chroma does not have a mandatory startup sequence.
	 * This function ensures a consistent startup for state tracking
	 * purposes.
	 */
	ret = hanbo_drv_init(hdev);
	if (ret < 0) {
		hid_err(hdev, "Driver init failed with %d\n", ret);
		goto fail_and_close;
	}

	priv->hwmon_dev = hwmon_device_register_with_info(&hdev->dev, DRIVER_NAME,
							  priv, &hanbo_chip_info, hanbo_groups);
	if (IS_ERR(priv->hwmon_dev)) {
		ret = PTR_ERR(priv->hwmon_dev);
		hid_err(hdev, "hwmon registration failed with %d\n", ret);
		goto fail_and_close;
	}
	hanbo_debugfs_init(priv);
	return 0;

fail_and_close:
	hid_hw_close(hdev);
fail_and_stop:
	hid_hw_stop(hdev);
	return ret;
}

static void hanbo_remove(struct hid_device *hdev)
{
	struct hanbo_data *priv = hid_get_drvdata(hdev);

	debugfs_remove_recursive(priv->debugfs);
	hwmon_device_unregister(priv->hwmon_dev);
	hid_hw_close(hdev);
	hid_hw_stop(hdev);
}

static struct hid_driver hanbo_driver = {
	.name = DRIVER_NAME,
	.id_table = hanbo_table,
	.probe = hanbo_probe,
	.remove = hanbo_remove,
	.raw_event = hanbo_raw_event,
};

static int __init hanbo_init(void)
{
	return hid_register_driver(&hanbo_driver);
}

static void __exit hanbo_exit(void)
{
	hid_unregister_driver(&hanbo_driver);
}

/* When compiled into the kernel, initialize after the HID bus */
late_initcall(hanbo_init);
module_exit(hanbo_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Joseph East <eastyjr@gmail.com>");
MODULE_DESCRIPTION("Hwmon driver for the Razer Hanbo Chroma cooler");
