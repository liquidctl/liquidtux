// SPDX-License-Identifier: GPL-2.0+
/*
 * hwmon driver for NZXT Kraken X53/X63/X73 coolers
 *
 * Copyright 2021  Jonas Malaco <jonas@protocubo.io>
 * Copyright 2022  Aleksa Savic <savicaleksa83@gmail.com>
 */

#include <linux/debugfs.h>
#include <linux/hid.h>
#include <linux/hwmon.h>
#include <linux/jiffies.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <asm/unaligned.h>

#define DRIVER_NAME		"nzxt-kraken3"
#define STATUS_REPORT_ID	0x75
#define FIRMWARE_REPORT_ID	0x11
#define STATUS_INTERVAL		1	/* seconds */
#define STATUS_VALIDITY		(4 * STATUS_INTERVAL)	/* seconds */

/* Register offsets for Kraken X53/X63/X73 */
#define X53_TEMP_SENSOR_START_OFFSET	15
#define X53_TEMP_SENSOR_END_OFFSET	16
#define X53_PUMP_SPEED_OFFSET		17
#define X53_PUMP_DUTY_OFFSET		19
#define X53_FIRMWARE_VERSION_OFFSET	0x11

static u8 x53_set_interval_cmd[] = { 0x70, 0x02, 0x01, 0xB8, STATUS_INTERVAL };
static u8 x53_finish_init_cmd[] = { 0x70, 0x01 };
static u8 x53_get_fw_version_cmd[] = { 0x10, 0x01 };

#define X53_SET_INTERVAL_CMD_LENGTH	5
#define X53_FINISH_INIT_CMD_LENGTH	2
#define X53_GET_FW_VERSION_CMD_LENGTH	2
#define X53_MAX_REPORT_LENGTH		64
#define X53_MIN_REPORT_LENGTH		20

static const char *const kraken3_temp_label[] = {
	"Coolant temp",
};

static const char *const kraken3_fan_label[] = {
	"Pump speed",
	"Pump duty [%]"
};

struct kraken3_priv_data {
	struct hid_device *hdev;
	struct device *hwmon_dev;
	struct dentry *debugfs;
	struct mutex buffer_lock;
	struct completion wait_completion;

	u8 *buffer;

	/* Sensor values */
	s32 temp_input[1];
	u16 fan_input[2];

	u8 firmware_version[3];

	unsigned long updated;	/* jiffies */
};

static umode_t kraken3_is_visible(const void *data, enum hwmon_sensor_types type, u32 attr,
				  int channel)
{
	switch (type) {
	case hwmon_temp:
		if (channel < 1)
			return 0444;
		break;
	case hwmon_fan:
		if (channel < 2)
			return 0444;
		break;
	default:
		break;
	}

	return 0;
}

static int kraken3_read(struct device *dev, enum hwmon_sensor_types type, u32 attr, int channel,
			long *val)
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

static int kraken3_read_string(struct device *dev, enum hwmon_sensor_types type, u32 attr,
			       int channel, const char **str)
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
			   HWMON_F_INPUT | HWMON_F_LABEL,
			   HWMON_F_INPUT | HWMON_F_LABEL),
	NULL
};

static const struct hwmon_chip_info kraken3_chip_info = {
	.ops = &kraken3_hwmon_ops,
	.info = kraken3_info,
};

static int kraken3_raw_event(struct hid_device *hdev, struct hid_report *report, u8 *data,
			     int size)
{
	int i;
	struct kraken3_priv_data *priv = hid_get_drvdata(hdev);

	if (size < X53_MIN_REPORT_LENGTH)
		return 0;

	if (report->id == FIRMWARE_REPORT_ID) {
		// Read firmware version
		for (i = 0; i < 3; i++)
			priv->firmware_version[i] = data[X53_FIRMWARE_VERSION_OFFSET + i];
		complete(&priv->wait_completion);
		return 0;
	}

	if (report->id != STATUS_REPORT_ID)
		return 0;

	/* Firmware/device is possibly damaged */
	if (data[X53_TEMP_SENSOR_START_OFFSET] == 0xff && data[X53_TEMP_SENSOR_END_OFFSET] == 0xff)
		return 0;

	/* Temperature and fan sensor readings */
	priv->temp_input[0] =
	    data[X53_TEMP_SENSOR_START_OFFSET] * 1000 + data[X53_TEMP_SENSOR_END_OFFSET] * 100;

	priv->fan_input[0] = get_unaligned_le16(data + X53_PUMP_SPEED_OFFSET);
	priv->fan_input[1] = data[X53_PUMP_DUTY_OFFSET];

	priv->updated = jiffies;

	return 0;
}

/* Writes the command to the device with the rest of the report (up to 64 bytes) filled
 * with zeroes
 */
static int kraken3_write_expanded(struct kraken3_priv_data *priv, u8 *cmd, int cmd_length)
{
	int ret;

	mutex_lock(&priv->buffer_lock);

	memset(priv->buffer, 0x00, X53_MAX_REPORT_LENGTH);
	memcpy(priv->buffer, cmd, cmd_length);
	ret = hid_hw_output_report(priv->hdev, priv->buffer, X53_MAX_REPORT_LENGTH);

	mutex_unlock(&priv->buffer_lock);
	return ret;
}

static int kraken3_init_device(struct hid_device *hdev)
{
	int ret;
	struct kraken3_priv_data *priv = hid_get_drvdata(hdev);

	/* Set the polling interval */
	ret = kraken3_write_expanded(priv, x53_set_interval_cmd, X53_SET_INTERVAL_CMD_LENGTH);
	if (ret < 0)
		return ret;

	/* Finalize the init process */
	ret = kraken3_write_expanded(priv, x53_finish_init_cmd, X53_FINISH_INIT_CMD_LENGTH);
	if (ret < 0)
		return ret;

	return 0;
}

static int __maybe_unused kraken3_reset_resume(struct hid_device *hdev)
{
	int ret;

	ret = kraken3_init_device(hdev);
	if (ret)
		hid_err(hdev, "req init (reset_resume) failed with %d\n", ret);

	return ret;
}

#ifdef CONFIG_DEBUG_FS

static int firmware_version_show(struct seq_file *seqf, void *unused)
{
	int ret;
	struct kraken3_priv_data *priv = seqf->private;

	reinit_completion(&priv->wait_completion);

	ret = kraken3_write_expanded(priv, x53_get_fw_version_cmd, X53_GET_FW_VERSION_CMD_LENGTH);
	if (ret < 0)
		return -ENODATA;

	/* The response to this request that the device sends is only catchable in
	 * kraken3_raw_event(), so we have to wait until it's processed there
	 */

	wait_for_completion(&priv->wait_completion);	/* Wait till report 0x11 */

	seq_printf(seqf, "%u.%u.%u\n", priv->firmware_version[0], priv->firmware_version[1],
		   priv->firmware_version[2]);

	return 0;
}

DEFINE_SHOW_ATTRIBUTE(firmware_version);

static void kraken3_debugfs_init(struct kraken3_priv_data *priv)
{
	char name[64];

	scnprintf(name, sizeof(name), "%s-%s", DRIVER_NAME, dev_name(&priv->hdev->dev));

	priv->debugfs = debugfs_create_dir(name, NULL);
	debugfs_create_file("firmware_version", 0444, priv->debugfs, priv, &firmware_version_fops);
}

#else

static void kraken3_debugfs_init(struct aqc_data *priv)
{
}

#endif

static int kraken3_probe(struct hid_device *hdev, const struct hid_device_id *id)
{
	struct kraken3_priv_data *priv;
	int ret;

	priv = devm_kzalloc(&hdev->dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->hdev = hdev;
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

	priv->buffer = devm_kzalloc(&hdev->dev, X53_MAX_REPORT_LENGTH, GFP_KERNEL);
	if (!priv->buffer) {
		ret = -ENOMEM;
		goto fail_and_close;
	}

	mutex_init(&priv->buffer_lock);
	init_completion(&priv->wait_completion);

	ret = kraken3_init_device(hdev);
	if (ret) {
		hid_err(hdev, "device init failed with %d\n", ret);
		goto fail_and_close;
	}

	priv->hwmon_dev = hwmon_device_register_with_info(&hdev->dev, DRIVER_NAME,
							  priv, &kraken3_chip_info, NULL);
	if (IS_ERR(priv->hwmon_dev)) {
		ret = PTR_ERR(priv->hwmon_dev);
		hid_err(hdev, "hwmon registration failed with %d\n", ret);
		goto fail_and_close;
	}

	kraken3_debugfs_init(priv);

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

	debugfs_remove_recursive(priv->debugfs);
	hwmon_device_unregister(priv->hwmon_dev);

	hid_hw_close(hdev);
	hid_hw_stop(hdev);
}

static const struct hid_device_id kraken3_table[] = {
	/* NZXT Kraken X53/X63/X73 have two possible product IDs */
	{ HID_USB_DEVICE(0x1e71, 0x2007) },
	{ HID_USB_DEVICE(0x1e71, 0x2014) },
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
	.name = DRIVER_NAME,
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
MODULE_AUTHOR("Aleksa Savic <savicaleksa83@gmail.com>");
MODULE_DESCRIPTION("Hwmon driver for NZXT Kraken X53/X63/X73 coolers");
