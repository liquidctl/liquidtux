// SPDX-License-Identifier: GPL-2.0+
/*
 * hwmon driver for Corsair Hydro Platinum / Pro XT / Elite RGB liquid coolers
 *
 * Supports monitoring of:
 * - Liquid temperature
 * - Pump speed and duty cycle
 * - Fan speeds and duty cycles (up to 3 fans)
 *
 * Supports control of:
 * - Pump mode (Quiet, Balanced, Extreme)
 * - Fan duty cycle (0-100%)
 *
 * Devices supported:
 * - Corsair Hydro H100i Platinum / SE
 * - Corsair Hydro H115i Platinum
 * - Corsair Hydro H60i / H100i / H115i / H150i Pro XT
 * - Corsair iCUE H100i / H115i / H150i Elite RGB
 *
 * Technical Description:
 * The device communicates via USB HID. Unlike typical HID devices, it requires
 * commands to be sent via Control Transfers (Set Report, Endpoint 0).
 * Status reports are received asynchronously via Input Reports on the Interrupt
 * IN endpoint.
 *
 * Initialization:
 * The device requires an initialization command to begin reporting status
 * and to set the fans/pump to a safe default state.
 *
 * Copyright 2026 Jack Greiner <jack@emoss.org>
 */

#include <linux/completion.h>
#include <linux/debugfs.h>
#include <linux/hid.h>
#include <linux/hwmon.h>
#include <linux/hwmon-sysfs.h>
#include <linux/jiffies.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/crc8.h>
#include <generated/uapi/linux/version.h>

#if KERNEL_VERSION(6, 12, 0) <= LINUX_VERSION_CODE
#include <linux/unaligned.h>
#else
#include <asm/unaligned.h>
#endif

#define DRIVER_NAME		"corsair_hydro_platinum"

#define USB_VENDOR_ID_CORSAIR		0x1b1c

#define REPORT_LENGTH			64
#define STATUS_VALIDITY			2000	/* ms; equivalent to two missed updates */
#define MAX_FAN_COUNT			3

#define CMD_WRITE_PREFIX		0x3f
#define FEATURE_COOLING			0x00	/* Main Cooling: Pump + Fan 1 + Fan 2 */
#define FEATURE_COOLING_FAN3		0x03	/* Extension: Fan 3 only (Main report is full) */
#define CMD_GET_STATUS			0xff
#define CMD_SET_COOLING			0x14
#define TRANSACTION_RETRIES		3

/* Pump Modes */
#define PUMP_MODE_QUIET			0x00
#define PUMP_MODE_BALANCED		0x01
#define PUMP_MODE_EXTREME		0x02

/* Fan Modes */
#define FAN_MODE_CUSTOM_PROFILE		0x00
#define FAN_MODE_FIXED_DUTY		0x02
#define FAN_MODE_FIXED_RPM		0x04

/* Offsets in Cooling Payload (Cmd 0x14) */
#define OFFSET_FAN1_MODE		8
#define OFFSET_FAN1_DUTY		13
#define OFFSET_FAN2_MODE		14
#define OFFSET_FAN2_DUTY		19
#define OFFSET_PUMP_MODE		20
#define OFFSET_PROFILE_LEN		26

DECLARE_CRC8_TABLE(corsair_crc8_table);

struct hydro_platinum_data {
	struct hid_device *hdev;
	struct device *hwmon_dev;
	struct mutex lock; /* lock for transfer buffer and data access */
	spinlock_t rx_lock; /* lock for rx_buffer access from raw_event */
	u8 *tx_buffer;
	u8 *rx_buffer;
	u8 sequence; /* protocol sequence number, cycles 1-31 */

	/* Sensor values */
	u16 fan_speeds[MAX_FAN_COUNT];
	u8 fan_duty[MAX_FAN_COUNT];
	u16 pump_speed;
	u8 pump_duty;
	long liquid_temp; /* millidegrees C */

	/* Control targets */
	u8 target_pump_mode;
	u8 target_fan_mode[MAX_FAN_COUNT];
	u8 target_fan_duty[MAX_FAN_COUNT];

	/* Detected configuration */
	int fan_count;
	const char *model_name;
	u8 fw_version[3];

	struct completion wait_for_report;

	unsigned long updated;
	struct dentry *debugfs;
};

/* Device Info Structs */
struct hydro_platinum_device_info {
	int fan_count;
	const char *hwmon_name;
	const char *model_name;
};

/* SMBus standard CRC-8 polynomial x^8 + x^2 + x + 1 (0x07) */
static void hydro_platinum_init_crc(void)
{
	crc8_populate_msb(corsair_crc8_table, 0x07);
}

/**
 * hydro_platinum_send_command - Asynchronously send a command to the device.
 * @priv: Driver data.
 * @feature: Feature ID (e.g. FEATURE_COOLING).
 * @command: Command ID.
 * @data: Optional payload data.
 * @data_len: Length of payload data.
 *
 * Constructs the report buffer with CRC and sends it via `hid_hw_raw_request`.
 */
static int hydro_platinum_send_command(struct hydro_platinum_data *priv, u8 feature, u8 command,
				       u8 *data, int data_len)
{
	int ret;
	int start_at;

	/* Byte 0 is the report number. Report data starts at byte 1. */
	memset(priv->tx_buffer, 0, REPORT_LENGTH + 1);
	priv->tx_buffer[1] = CMD_WRITE_PREFIX;

	/* Sequence and feature/command logic */
	priv->sequence = (priv->sequence % 31) + 1;
	priv->tx_buffer[2] = (priv->sequence << 3) | feature;
	priv->tx_buffer[3] = command;
	start_at = 4;

	if (data && data_len > 0)
		memcpy(priv->tx_buffer + start_at, data,
		       min(data_len, REPORT_LENGTH - start_at));

	/*
	 * CRC-8 (SMBus polynomial) over buf[2] through buf[REPORT_LENGTH-1].
	 * The result is placed in buf[REPORT_LENGTH], the last byte of the
	 * 65-byte report. The device validates this on receipt.
	 */
	priv->tx_buffer[REPORT_LENGTH] = crc8(corsair_crc8_table, priv->tx_buffer + 2,
					      REPORT_LENGTH - 2, 0);

	ret = hid_hw_raw_request(priv->hdev, 0, priv->tx_buffer, REPORT_LENGTH + 1,
				 HID_OUTPUT_REPORT, HID_REQ_SET_REPORT);
	if (ret > 0)
		ret = 0;

	return ret;
}

/**
 * hydro_platinum_transaction - Send a command and wait for a response.
 * @priv: Driver data.
 * @feature: Feature ID.
 * @command: Command ID.
 * @data: Optional payload data.
 * @data_len: Length of payload data.
 *
 * Sends a command and waits up to 500ms for an Input Report on the Interrupt IN endpoint.
 * This ensures strict command-response ordering to prevent device confusion.
 */
static int hydro_platinum_transaction(struct hydro_platinum_data *priv, u8 feature, u8 command,
				      u8 *data, int data_len)
{
	u8 rx_copy[REPORT_LENGTH];
	int ret;
	int tries;

	reinit_completion(&priv->wait_for_report);

	ret = hydro_platinum_send_command(priv, feature, command, data, data_len);
	if (ret < 0) {
		hid_err(priv->hdev, "Failed to send command %02x: %d\n",
			command, ret);
		return ret;
	}

	/*
	 * Wait for a valid response, retrying if the CRC check fails.
	 * CRC failures can occur when userspace tools (liquidctl, OpenRGB)
	 * are accessing the device concurrently via HIDRAW, causing us to
	 * intercept their response packets.
	 */
	for (tries = 0; tries < TRANSACTION_RETRIES; tries++) {
		ret = wait_for_completion_interruptible_timeout(&priv->wait_for_report,
								msecs_to_jiffies(500));
		if (ret == 0) {
			hid_warn(priv->hdev,
				 "Timeout waiting for response to command %02x\n",
				 command);
			return -ETIMEDOUT;
		} else if (ret < 0) {
			return ret;
		}

		/*
		 * Copy rx_buffer under the spinlock to prevent a late or
		 * unsolicited raw_event from overwriting it mid-read.
		 */
		spin_lock(&priv->rx_lock);
		memcpy(rx_copy, priv->rx_buffer, sizeof(rx_copy));
		spin_unlock(&priv->rx_lock);

		/*
		 * CRC Verification: checksumming (Data + CRC) should yield 0
		 * for valid packets. The device uses its own sequence counter
		 * so we cannot match by sequence number. CRC is our only
		 * validation option.
		 */
		if (crc8(corsair_crc8_table, rx_copy + 1,
			 REPORT_LENGTH - 1, 0) != 0) {
			hid_dbg(priv->hdev,
				"CRC check failed for command %02x (attempt %d/%d)\n",
				command, tries + 1, TRANSACTION_RETRIES);
			reinit_completion(&priv->wait_for_report);
			continue;
		}

		/* CRC valid -- copy back for callers to consume */
		memcpy(priv->rx_buffer, rx_copy, sizeof(rx_copy));
		return 0;
	}

	hid_warn(priv->hdev,
		 "Failed to get valid response for command %02x after %d attempts\n",
		 command, TRANSACTION_RETRIES);
	return -EIO;
}

/* Initialize the common cooling payload prefix (shared by main and secondary commands) */
static void hydro_platinum_init_cooling_payload(u8 *data, int size)
{
	memset(data, 0, size);
	data[0] = 0x00;
	data[1] = 0xff;
	data[2] = 0x05;
	memset(data + 3, 0xff, 5);
	data[OFFSET_PROFILE_LEN] = 7;
}

/**
 * hydro_platinum_write_cooling - Commit target fan/pump settings to the device.
 * @priv: Driver data.
 *
 * Generates the configuration payload based on `target_*` members and sends it.
 * Handles the split between Main (Fan 1, 2, Pump) and Secondary (Fan 3) cooling features.
 * Sends Main feature FIRST to ensure core cooling is applied, then Secondary.
 */
static int hydro_platinum_write_cooling(struct hydro_platinum_data *priv)
{
	int ret;
	u8 data[60];

	hydro_platinum_init_cooling_payload(data, sizeof(data));

	/* Pump Mode */
	data[OFFSET_PUMP_MODE] = priv->target_pump_mode;

	/* Fan 1 (Index 0) */
	if (priv->fan_count >= 1) {
		data[OFFSET_FAN1_MODE] = priv->target_fan_mode[0];
		/* If fixed duty */
		if (priv->target_fan_mode[0] == FAN_MODE_FIXED_DUTY)
			data[OFFSET_FAN1_DUTY] = priv->target_fan_duty[0];
	}

	/* Fan 2 (Index 1) */
	if (priv->fan_count >= 2) {
		data[OFFSET_FAN2_MODE] = priv->target_fan_mode[1];
		if (priv->target_fan_mode[1] == FAN_MODE_FIXED_DUTY)
			data[OFFSET_FAN2_DUTY] = priv->target_fan_duty[1];
	}

	/* Send Feature Cooling (Fan 1, 2, Pump) */
	ret = hydro_platinum_transaction(priv, FEATURE_COOLING, CMD_SET_COOLING, data,
					 sizeof(data));
	if (ret)
		return ret;

	/*
	 * Command Ordering Note:
	 * The device requires the "Main" cooling command (Feature 0x00) to be sent
	 * BEFORE the "Secondary" cooling command (Feature 0x03, for Fan 3).
	 * reversing this order may cause the device to stall or return -EPIPE.
	 */

	/*
	 * Fan 3 (Index 2) - Uses the secondary cooling feature (0x03).
	 * The secondary payload has the same structure as the main one,
	 * but only the Fan 1 slot is populated (with Fan 3's settings).
	 * Fan 2 slot and pump mode must still be present in the payload.
	 */
	if (priv->fan_count >= 3) {
		u8 data2[60];

		hydro_platinum_init_cooling_payload(data2, sizeof(data2));

		/* Pump mode must be set in both main and secondary commands */
		data2[OFFSET_PUMP_MODE] = priv->target_pump_mode;

		/* Fan 3 settings go into the Fan 1 slot of the secondary command */
		data2[OFFSET_FAN1_MODE] = priv->target_fan_mode[2];
		if (priv->target_fan_mode[2] == FAN_MODE_FIXED_DUTY)
			data2[OFFSET_FAN1_DUTY] = priv->target_fan_duty[2];

		ret = hydro_platinum_transaction(priv, FEATURE_COOLING_FAN3, CMD_SET_COOLING,
						 data2, sizeof(data2));
		if (ret)
			return ret;
	}

	return 0;
}

static int hydro_platinum_raw_event(struct hid_device *hdev, struct hid_report *report, u8 *data,
				    int size)
{
	struct hydro_platinum_data *priv = hid_get_drvdata(hdev);

	/* We only care about Input reports (Responses) */
	if (report->type != HID_INPUT_REPORT)
		return 0;

	/* Clamp incoming report payload to rx_buffer size */
	if (size > REPORT_LENGTH)
		size = REPORT_LENGTH;

	/*
	 * The device uses its own sequence counter in responses rather than
	 * echoing ours, so we cannot filter by sequence number here.
	 * Accept any input report and let the transaction logic (CRC check)
	 * validate the response.
	 */
	spin_lock(&priv->rx_lock);
	memcpy(priv->rx_buffer, data, size);
	spin_unlock(&priv->rx_lock);

	complete(&priv->wait_for_report);
	return 0;
}

/**
 * hydro_platinum_update - Fetch latest status from device.
 * @priv: Driver data.
 *
 * Uses `CMD_GET_STATUS` to poll sensor data.
 * Refresh rate limited by `STATUS_VALIDITY` (1s).
 */
static int hydro_platinum_update(struct hydro_platinum_data *priv)
{
	int ret;

	ret = mutex_lock_interruptible(&priv->lock);
	if (ret)
		return ret;

	if (time_after(jiffies, priv->updated + msecs_to_jiffies(STATUS_VALIDITY))) {
		reinit_completion(&priv->wait_for_report);

		ret = hydro_platinum_transaction(priv, FEATURE_COOLING, CMD_GET_STATUS, NULL, 0);
		if (ret < 0)
			goto out;

		/* Data is now in priv->rx_buffer (populated by raw_event) */

		/* Firmware Version: res[2] (Major << 4 | Minor), res[3] (Patch) */
		priv->fw_version[0] = priv->rx_buffer[2] >> 4;
		priv->fw_version[1] = priv->rx_buffer[2] & 0x0f;
		priv->fw_version[2] = priv->rx_buffer[3];

		/* Temp */
		priv->liquid_temp = ((int)priv->rx_buffer[8] * 1000) +
				    ((int)priv->rx_buffer[7] * 1000 / 255);

		/*
		 * Parse sensor data. Each channel has duty at the base
		 * offset and speed as a le16 at base+1.
		 */
		static const u8 fan_offsets[] = { 14, 21, 42 };
		int i;

		/* Pump (Base 28) */
		priv->pump_speed = get_unaligned_le16(priv->rx_buffer + 28 + 1);
		priv->pump_duty = priv->rx_buffer[28];

		for (i = 0; i < priv->fan_count; i++) {
			u8 base = fan_offsets[i];

			priv->fan_speeds[i] = get_unaligned_le16(priv->rx_buffer + base + 1);
			priv->fan_duty[i] = priv->rx_buffer[base];
		}

		priv->updated = jiffies;
	}
	ret = 0;

out:
	mutex_unlock(&priv->lock);
	return ret;
}

static umode_t hydro_platinum_is_visible(const void *data, enum hwmon_sensor_types type, u32 attr,
					 int channel)
{
	const struct hydro_platinum_data *priv = data;

	switch (type) {
	case hwmon_temp:
		return 0444;
	case hwmon_fan:
	case hwmon_pwm:
		/* Channel 0: Pump */
		/* Channel 1..N: Fans */
		if (channel == 0)
			return 0644; /* Pump (Mode control via PWM) */
		if (channel <= priv->fan_count)
			return 0644; /* Fans */
		return 0;
	default:
		return 0;
	}
}

static int hydro_platinum_read(struct device *dev, enum hwmon_sensor_types type, u32 attr,
			       int channel, long *val)
{
	struct hydro_platinum_data *priv = dev_get_drvdata(dev);
	int ret = hydro_platinum_update(priv);

	if (ret < 0)
		return ret;

	if (time_after(jiffies, priv->updated + msecs_to_jiffies(STATUS_VALIDITY)))
		return -ENODATA;

	switch (type) {
	case hwmon_fan:
		if (channel == 0)
			*val = priv->pump_speed;
		else
			*val = priv->fan_speeds[channel - 1];
		break;
	case hwmon_pwm:
		if (channel == 0)
			*val = priv->pump_duty;
		else
			*val = priv->fan_duty[channel - 1];
		break;
	case hwmon_temp:
		*val = priv->liquid_temp;
		break;
	default:
		return -EOPNOTSUPP;
	}
	return 0;
}

static int hydro_platinum_write(struct device *dev, enum hwmon_sensor_types type, u32 attr,
				int channel, long val)
{
	struct hydro_platinum_data *priv = dev_get_drvdata(dev);
	int ret;
	int i;

	ret = mutex_lock_interruptible(&priv->lock);
	if (ret)
		return ret;

	switch (type) {
	case hwmon_pwm:
		if (channel == 0) {
			/*
			 * Pump Control:
			 * Map 0-255 PWM to discrete modes:
			 * 0-84: Quiet		(Mode 0)
			 * 85-169: Balanced	(Mode 1)
			 * 170-255: Extreme	(Mode 2)
			 */
			u8 mode;

			val = clamp_val(val, 0, 255);

			if (val < 85)
				mode = PUMP_MODE_QUIET;
			else if (val < 170)
				mode = PUMP_MODE_BALANCED;
			else
				mode = PUMP_MODE_EXTREME;

			priv->target_pump_mode = mode;

		} else {
			/* Fan Control */
			/* Index is channel - 1 */
			i = channel - 1;
			if (i >= priv->fan_count) {
				ret = -EINVAL;
				goto out;
			}

			val = clamp_val(val, 0, 255);
			priv->target_fan_duty[i] = (u8)val;
			priv->target_fan_mode[i] = FAN_MODE_FIXED_DUTY;
		}

		ret = hydro_platinum_write_cooling(priv);
		if (ret) {
			if (channel == 0)
				hid_warn(priv->hdev,
					 "Failed to set Pump speed: %d\n", ret);
			else
				hid_warn(priv->hdev,
					 "Failed to set Fan %d speed: %d\n",
					 channel, ret);
		}
		break;
	default:
		ret = -EOPNOTSUPP;
		break;
	}

out:
	mutex_unlock(&priv->lock);
	return ret;
}

static const char *const hydro_platinum_fan_labels[] = {
	"Pump",
	"Fan 1",
	"Fan 2",
	"Fan 3"
};

static int hydro_platinum_read_string_impl(struct device *dev, enum hwmon_sensor_types type,
					   u32 attr, int channel, const char **str)
{
	switch (type) {
	case hwmon_fan:
		if (attr == hwmon_fan_label) {
			if (channel < ARRAY_SIZE(hydro_platinum_fan_labels)) {
				*str = hydro_platinum_fan_labels[channel];
				return 0;
			}
		}
		break;
	case hwmon_temp:
		if (attr == hwmon_temp_label) {
			*str = "Coolant temp";
			return 0;
		}
		break;
	default:
		break;
	}
	return -EOPNOTSUPP;
}

static const struct hwmon_channel_info *hydro_platinum_info[] = {
	HWMON_CHANNEL_INFO(fan,
			   HWMON_F_INPUT | HWMON_F_LABEL, /* Pump */
			   HWMON_F_INPUT | HWMON_F_LABEL, /* Fan 1 */
			   HWMON_F_INPUT | HWMON_F_LABEL, /* Fan 2 */
			   HWMON_F_INPUT | HWMON_F_LABEL), /* Fan 3 */
	HWMON_CHANNEL_INFO(pwm,
			   HWMON_PWM_INPUT, /* Pump */
			   HWMON_PWM_INPUT, /* Fan 1 */
			   HWMON_PWM_INPUT, /* Fan 2 */
			   HWMON_PWM_INPUT), /* Fan 3 */
	HWMON_CHANNEL_INFO(temp,
			   HWMON_T_INPUT | HWMON_T_LABEL),
	NULL
};

static const struct hwmon_ops hydro_platinum_hwmon_ops = {
	.is_visible = hydro_platinum_is_visible,
	.read = hydro_platinum_read,
	.write = hydro_platinum_write,
	.read_string = hydro_platinum_read_string_impl,
};

static ssize_t label_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct hydro_platinum_data *priv = dev_get_drvdata(dev);

	return sysfs_emit(buf, "%s\n", priv->model_name);
}
static DEVICE_ATTR_RO(label);

static struct attribute *hydro_platinum_attrs[] = {
	&dev_attr_label.attr,
	NULL
};

static const struct attribute_group hydro_platinum_group = {
	.attrs = hydro_platinum_attrs,
};

static int firmware_version_show(struct seq_file *seq, void *offset)
{
	struct hydro_platinum_data *priv = seq->private;

	seq_printf(seq, "%d.%d.%d\n",
		   priv->fw_version[0], priv->fw_version[1], priv->fw_version[2]);

	return 0;
}
DEFINE_SHOW_ATTRIBUTE(firmware_version);

static void hydro_platinum_debugfs_init(struct hydro_platinum_data *priv)
{
	char name[64];

	snprintf(name, sizeof(name), "corsair_hydro_platinum-%s",
		 dev_name(&priv->hdev->dev));

	priv->debugfs = debugfs_create_dir(name, NULL);
	debugfs_create_file("firmware_version", 0444, priv->debugfs, priv,
			    &firmware_version_fops);
}

static const struct attribute_group *hydro_platinum_groups[] = {
	&hydro_platinum_group,
	NULL
};

static const struct hwmon_chip_info hydro_platinum_chip_info = {
	.ops = &hydro_platinum_hwmon_ops,
	.info = hydro_platinum_info,
};

static int hydro_platinum_probe(struct hid_device *hdev, const struct hid_device_id *id)
{
	struct hydro_platinum_data *priv;
	int ret;
	int i;

	priv = devm_kzalloc(&hdev->dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->hdev = hdev;
	priv->tx_buffer = devm_kzalloc(&hdev->dev, REPORT_LENGTH + 1, GFP_KERNEL);
	if (!priv->tx_buffer)
		return -ENOMEM;

	priv->rx_buffer = devm_kzalloc(&hdev->dev, REPORT_LENGTH, GFP_KERNEL);
	if (!priv->rx_buffer)
		return -ENOMEM;

	mutex_init(&priv->lock);
	spin_lock_init(&priv->rx_lock);
	init_completion(&priv->wait_for_report);

	/*
	 * Initialize ->updated to STATUS_VALIDITY in the past, making
	 * the initial empty data invalid for hydro_platinum_read without
	 * the need for a special case there.
	 */
	priv->updated = jiffies - msecs_to_jiffies(STATUS_VALIDITY);

	hid_set_drvdata(hdev, priv);

	/*
	 * Retrieve device specific info from the ID table directly.
	 * This avoids a large switch statement and redundant data.
	 */
	const struct hydro_platinum_device_info *info =
		(const struct hydro_platinum_device_info *)id->driver_data;

	priv->fan_count = info->fan_count;
	priv->model_name = info->model_name;

	ret = hid_parse(hdev);
	if (ret)
		return ret;

	ret = hid_hw_start(hdev, HID_CONNECT_HIDRAW);
	if (ret)
		return ret;

	ret = hid_hw_open(hdev);
	if (ret)
		goto fail_and_stop;

	/* Enable HID input during probe (required for raw_event) */
	hid_device_io_start(hdev);

	/* Initialize CRC table */
	hydro_platinum_init_crc();

	/*
	 * Initialize Device
	 * Default: Pump Balanced, Fans 50%
	 */
	priv->target_pump_mode = PUMP_MODE_BALANCED;
	for (i = 0; i < MAX_FAN_COUNT; i++) {
		priv->target_fan_mode[i] = FAN_MODE_FIXED_DUTY;
		priv->target_fan_duty[i] = 128; /* 50% approx */
	}

	ret = hydro_platinum_write_cooling(priv);
	if (ret) {
		hid_err(hdev, "initialization command failed: %d\n", ret);
		goto fail_and_close;
	}

	/* Initial update to get firmware version */
	ret = hydro_platinum_update(priv);
	if (ret) {
		hid_err(hdev, "initial status update failed: %d\n", ret);
		goto fail_and_close;
	}

	hid_dbg(hdev, "Firmware version: %d.%d.%d\n",
		priv->fw_version[0], priv->fw_version[1], priv->fw_version[2]);

	priv->hwmon_dev = hwmon_device_register_with_info(&hdev->dev, info->hwmon_name,
							  priv, &hydro_platinum_chip_info,
							  hydro_platinum_groups);
	if (IS_ERR(priv->hwmon_dev)) {
		ret = PTR_ERR(priv->hwmon_dev);
		goto fail_and_close;
	}

	hydro_platinum_debugfs_init(priv);

	return 0;

fail_and_close:
	hid_hw_close(hdev);
fail_and_stop:
	hid_hw_stop(hdev);
	mutex_destroy(&priv->lock);
	return ret;
}

static int __maybe_unused hydro_platinum_reset_resume(struct hid_device *hdev)
{
	struct hydro_platinum_data *priv = hid_get_drvdata(hdev);
	int ret;

	mutex_lock(&priv->lock);
	ret = hydro_platinum_write_cooling(priv);
	mutex_unlock(&priv->lock);

	if (ret)
		hid_err(hdev, "re-initialization (reset_resume) failed with %d\n", ret);

	return ret;
}

static void hydro_platinum_remove(struct hid_device *hdev)
{
	struct hydro_platinum_data *priv = hid_get_drvdata(hdev);

	debugfs_remove_recursive(priv->debugfs);
	hwmon_device_unregister(priv->hwmon_dev);
	hid_hw_close(hdev);
	hid_hw_stop(hdev);
	mutex_destroy(&priv->lock);
}

/* Driver Data used for Fan Count */
#define HYDRO_PLATINUM_INFO(_fans, _name, _model) \
	static const struct hydro_platinum_device_info info_##_name = { \
		.fan_count = _fans, \
		.hwmon_name = #_name, \
		.model_name = _model \
	}

HYDRO_PLATINUM_INFO(2, corsair_h100i_plat, "Corsair Hydro H100i Platinum");
HYDRO_PLATINUM_INFO(2, corsair_h100i_plat_se, "Corsair Hydro H100i Platinum SE");
HYDRO_PLATINUM_INFO(2, corsair_h115i_plat, "Corsair Hydro H115i Platinum");
HYDRO_PLATINUM_INFO(2, corsair_h60i_xt, "Corsair Hydro H60i Pro XT");
HYDRO_PLATINUM_INFO(2, corsair_h100i_xt, "Corsair Hydro H100i Pro XT");
HYDRO_PLATINUM_INFO(2, corsair_h115i_xt, "Corsair Hydro H115i Pro XT");
HYDRO_PLATINUM_INFO(3, corsair_h150i_xt, "Corsair Hydro H150i Pro XT");
HYDRO_PLATINUM_INFO(2, corsair_h100i_elite, "Corsair iCUE H100i Elite RGB");
HYDRO_PLATINUM_INFO(2, corsair_h115i_elite, "Corsair iCUE H115i Elite RGB");
HYDRO_PLATINUM_INFO(3, corsair_h150i_elite, "Corsair iCUE H150i Elite RGB");
HYDRO_PLATINUM_INFO(2, corsair_h100i_elite_w, "Corsair iCUE H100i Elite RGB (White)");
HYDRO_PLATINUM_INFO(3, corsair_h150i_elite_w, "Corsair iCUE H150i Elite RGB (White)");

static const struct hid_device_id hydro_platinum_table[] = {
	/* H100i Platinum */
	{ HID_USB_DEVICE(USB_VENDOR_ID_CORSAIR, 0x0c18),
	  .driver_data = (kernel_ulong_t)&info_corsair_h100i_plat },
	/* H100i Platinum SE */
	{ HID_USB_DEVICE(USB_VENDOR_ID_CORSAIR, 0x0c19),
	  .driver_data = (kernel_ulong_t)&info_corsair_h100i_plat_se },
	/* H115i Platinum */
	{ HID_USB_DEVICE(USB_VENDOR_ID_CORSAIR, 0x0c17),
	  .driver_data = (kernel_ulong_t)&info_corsair_h115i_plat },
	/* H60i Pro XT */
	{ HID_USB_DEVICE(USB_VENDOR_ID_CORSAIR, 0x0c29),
	  .driver_data = (kernel_ulong_t)&info_corsair_h60i_xt },
	/* H100i Pro XT */
	{ HID_USB_DEVICE(USB_VENDOR_ID_CORSAIR, 0x0c20),
	  .driver_data = (kernel_ulong_t)&info_corsair_h100i_xt },
	/* H115i Pro XT */
	{ HID_USB_DEVICE(USB_VENDOR_ID_CORSAIR, 0x0c21),
	  .driver_data = (kernel_ulong_t)&info_corsair_h115i_xt },
	/* H150i Pro XT */
	{ HID_USB_DEVICE(USB_VENDOR_ID_CORSAIR, 0x0c22),
	  .driver_data = (kernel_ulong_t)&info_corsair_h150i_xt },
	/* iCUE H100i Elite RGB */
	{ HID_USB_DEVICE(USB_VENDOR_ID_CORSAIR, 0x0c35),
	  .driver_data = (kernel_ulong_t)&info_corsair_h100i_elite },
	/* iCUE H115i Elite RGB */
	{ HID_USB_DEVICE(USB_VENDOR_ID_CORSAIR, 0x0c36),
	  .driver_data = (kernel_ulong_t)&info_corsair_h115i_elite },
	/* iCUE H150i Elite RGB */
	{ HID_USB_DEVICE(USB_VENDOR_ID_CORSAIR, 0x0c37),
	  .driver_data = (kernel_ulong_t)&info_corsair_h150i_elite },
	/* iCUE H100i Elite RGB (White) */
	{ HID_USB_DEVICE(USB_VENDOR_ID_CORSAIR, 0x0c40),
	  .driver_data = (kernel_ulong_t)&info_corsair_h100i_elite_w },
	/* iCUE H150i Elite RGB (White) */
	{ HID_USB_DEVICE(USB_VENDOR_ID_CORSAIR, 0x0c41),
	  .driver_data = (kernel_ulong_t)&info_corsair_h150i_elite_w },
	{ }
};
MODULE_DEVICE_TABLE(hid, hydro_platinum_table);

static struct hid_driver hydro_platinum_driver = {
	.name = DRIVER_NAME,
	.id_table = hydro_platinum_table,
	.probe = hydro_platinum_probe,
	.remove = hydro_platinum_remove,
	.raw_event = hydro_platinum_raw_event,
#ifdef CONFIG_PM
	.reset_resume = hydro_platinum_reset_resume,
#endif
};

static int __init hydro_platinum_init(void)
{
	return hid_register_driver(&hydro_platinum_driver);
}

static void __exit hydro_platinum_exit(void)
{
	hid_unregister_driver(&hydro_platinum_driver);
}

/* When compiled into the kernel, initialize after the HID bus */
late_initcall(hydro_platinum_init);
module_exit(hydro_platinum_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Hwmon driver for Corsair Hydro Platinum / Pro XT / Elite RGB");
