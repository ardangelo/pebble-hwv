#include <stdlib.h>

#include <zephyr/drivers/sensor.h>
#include <zephyr/shell/shell.h>

static const struct device *const light = DEVICE_DT_GET(DT_ALIAS(light0));
static bool initialized;

static int cmd_light_get(const struct shell *sh, size_t argc, char **argv)
{
	int err;
	struct sensor_value val;

	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	if (!initialized) {
		shell_error(sh, "Light sensor module not initialized");
		return -EPERM;
	}

	err = sensor_sample_fetch(light);
	if (err < 0) {
		shell_error(sh, "Failed to fetch sensor data (%d)", err);
		return err;
	}

	err = sensor_channel_get(light, SENSOR_CHAN_LIGHT, &val);
	if (err < 0) {
		shell_error(sh, "Failed to get illuminance data (%d)", err);
		return err;
	}

	shell_print(sh, "Illuminance: %.2f lux", sensor_value_to_double(&val));

	return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(sub_light_cmds,
			       SHELL_CMD(get, NULL, "Get sensor data", cmd_light_get),
			       SHELL_SUBCMD_SET_END);

SHELL_SUBCMD_ADD((hwv), light, &sub_light_cmds, "Light sensor", NULL, 0, 0);

int light_init(void)
{
	if (!device_is_ready(light)) {
		return -ENODEV;
	}

	initialized = true;

	return 0;
}
