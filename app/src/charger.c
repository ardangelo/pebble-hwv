#include <zephyr/drivers/sensor.h>
#include <zephyr/drivers/sensor/npm1300_charger.h>
#include <zephyr/shell/shell.h>

static const struct device *const charger = DEVICE_DT_GET(DT_ALIAS(charger0));
static bool initialized;

static int cmd_charger_status(const struct shell *sh, size_t argc, char **argv)
{
	struct sensor_value volt;
	struct sensor_value current;
	struct sensor_value temp;
	struct sensor_value error;
	struct sensor_value status;
	struct sensor_value vbus_present;
	int ret;

	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	if (!initialized) {
		return -ENODEV;
	}

	ret = sensor_sample_fetch(charger);
	if (ret == 0) {
		ret = sensor_channel_get(charger, SENSOR_CHAN_GAUGE_VOLTAGE, &volt);
	}
	if (ret == 0) {
		ret = sensor_channel_get(charger, SENSOR_CHAN_GAUGE_AVG_CURRENT, &current);
	}
	if (ret == 0) {
		ret = sensor_channel_get(charger, SENSOR_CHAN_GAUGE_TEMP, &temp);
	}
	if (ret == 0) {
		ret = sensor_channel_get(charger,
			(enum sensor_channel)SENSOR_CHAN_NPM1300_CHARGER_STATUS, &status);
	}
	if (ret == 0) {
		ret = sensor_channel_get(charger,
			(enum sensor_channel)SENSOR_CHAN_NPM1300_CHARGER_ERROR, &error);
	}
	if (ret == 0) {
		ret = sensor_attr_get(charger,
			(enum sensor_channel)SENSOR_CHAN_NPM1300_CHARGER_VBUS_STATUS,
			(enum sensor_attribute)SENSOR_ATTR_NPM1300_CHARGER_VBUS_PRESENT,
			&vbus_present);
	}
	if (ret < 0) {
		shell_error(sh, "nPM1300 status read failed (%d)", ret);
		return ret;
	}

	shell_print(sh, "V: %d.%03d ", volt.val1, volt.val2 / 1000);

	shell_print(sh, "I: %s%d.%04d ", ((current.val1 < 0) || (current.val2 < 0)) ? "-" : "",
		    abs(current.val1), abs(current.val2) / 100);

	shell_print(sh, "T: %s%d.%02d\n", ((temp.val1 < 0) || (temp.val2 < 0)) ? "-" : "",
		    abs(temp.val1), abs(temp.val2) / 10000);

	shell_print(sh, "Charger Status: %d, Error: %d, VBUS: %s\n", status.val1, error.val1,
		    vbus_present.val1 ? "connected" : "disconnected");

	return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(sub_charger_cmds, SHELL_CMD(status, NULL, "Get charger status", cmd_charger_status),
			       SHELL_SUBCMD_SET_END);

SHELL_SUBCMD_ADD((hwv), charger, &sub_charger_cmds, "Charger", NULL, 0, 0);

int charger_init(void)
{
	if (!device_is_ready(charger)) {
		return -ENODEV;
	}

	initialized = true;

	return 0;
}
