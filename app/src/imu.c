#include <zephyr/drivers/sensor.h>
#include <stdlib.h>

#include <zephyr/kernel.h>
#include <zephyr/shell/shell.h>

static const struct device *const imu = DEVICE_DT_GET(DT_ALIAS(imu0));
static bool initialized;
K_SEM_DEFINE(imu_interrupt, 0, 1);
static volatile int imu_interrupt_fetch_result;
static struct sensor_trigger imu_accel_trigger = {
	.type = SENSOR_TRIG_DATA_READY,
	.chan = SENSOR_CHAN_ACCEL_XYZ,
};

static void imu_trigger_handler(const struct device *dev, const struct sensor_trigger *trigger)
{
	ARG_UNUSED(trigger);

	/* Reading OUTX_L_A through OUTZ_H_A clears the LSM6DSO data-ready condition. */
	imu_interrupt_fetch_result = sensor_sample_fetch_chan(dev, SENSOR_CHAN_ACCEL_XYZ);
	k_sem_give(&imu_interrupt);
}

static int cmd_imu_get(const struct shell *sh, size_t argc, char **argv)
{
	int err;
	struct sensor_value odr, acc_data[3], gyro_data[3];

	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	if (!initialized) {
		shell_error(sh, "IMU sensor module not initialized");
		return -EPERM;
	}

	/* ODR: 12.5 Hz */
	odr.val1 = 12;
	odr.val2 = 500000;

	err = sensor_attr_set(imu, SENSOR_CHAN_ACCEL_XYZ, SENSOR_ATTR_SAMPLING_FREQUENCY, &odr);
	if (err < 0) {
		return err;
	}

	err = sensor_attr_set(imu, SENSOR_CHAN_GYRO_XYZ, SENSOR_ATTR_SAMPLING_FREQUENCY, &odr);
	if (err < 0) {
		return err;
	}
	k_msleep(100);

	err = sensor_sample_fetch(imu);
	if (err < 0) {
		shell_error(sh, "Failed to fetch sensor data (%d)", err);
		return err;
	}

	err = sensor_channel_get(imu, SENSOR_CHAN_ACCEL_XYZ, acc_data);
	if (err < 0) {
		shell_error(sh, "Failed to get accelerometer data (%d)", err);
		return err;
	}

	err = sensor_channel_get(imu, SENSOR_CHAN_GYRO_XYZ, gyro_data);
	if (err < 0) {
		shell_error(sh, "Failed to get gyroscope data (%d)", err);
		return err;
	}

	shell_print(sh, "Acceleration (m/s2): %.6f, %.6f, %.6f",
		    sensor_value_to_double(&acc_data[0]), sensor_value_to_double(&acc_data[1]),
		    sensor_value_to_double(&acc_data[2]));
	shell_print(sh, "Angular velocity (rad/s): %.6f, %.6f, %.6f",
		    sensor_value_to_double(&gyro_data[0]), sensor_value_to_double(&gyro_data[1]),
		    sensor_value_to_double(&gyro_data[2]));

	/* ODR: 0 (power-down) */
	odr.val1 = 0;
	odr.val2 = 0;

	err = sensor_attr_set(imu, SENSOR_CHAN_ACCEL_XYZ, SENSOR_ATTR_SAMPLING_FREQUENCY, &odr);
	if (err < 0) {
		return err;
	}

	err = sensor_attr_set(imu, SENSOR_CHAN_GYRO_XYZ, SENSOR_ATTR_SAMPLING_FREQUENCY, &odr);
	if (err < 0) {
		return err;
	}

	return 0;
}

static int cmd_imu_interrupt(const struct shell *sh, size_t argc, char **argv)
{
	struct sensor_value odr = { .val1 = 12, .val2 = 500000 };
	unsigned long timeout_s = argc > 1 ? strtoul(argv[1], NULL, 0) : 5U;
	int cleanup_ret = 0;
	int ret;

	if (!initialized) {
		return -EPERM;
	}
	if (timeout_s == 0U || timeout_s > 60U) {
		return -EINVAL;
	}

	k_sem_reset(&imu_interrupt);
	imu_interrupt_fetch_result = -EINPROGRESS;
	ret = sensor_attr_set(imu, SENSOR_CHAN_ACCEL_XYZ, SENSOR_ATTR_SAMPLING_FREQUENCY, &odr);
	if (ret < 0) {
		return ret;
	}

	ret = sensor_trigger_set(imu, &imu_accel_trigger, imu_trigger_handler);
	if (ret < 0) {
		shell_error(sh, "Failed to enable IMU INT1 (%d)", ret);
		goto cleanup;
	}

	ret = k_sem_take(&imu_interrupt, K_SECONDS(timeout_s));
	if (ret < 0) {
		shell_error(sh, "No IMU INT1 event within %lu seconds", timeout_s);
	} else if (imu_interrupt_fetch_result < 0) {
		ret = imu_interrupt_fetch_result;
		shell_error(sh, "IMU INT1 sample fetch failed (%d)", ret);
	} else {
		shell_print(sh, "IMU INT1 data-ready event received and cleared");
	}

cleanup:
	/* Stop new samples and clear any pending DRDY before removing the callback. */
	odr.val1 = 0;
	odr.val2 = 0;
	int power_down_ret = sensor_attr_set(imu, SENSOR_CHAN_ACCEL_XYZ,
					     SENSOR_ATTR_SAMPLING_FREQUENCY, &odr);
	int fetch_ret = sensor_sample_fetch_chan(imu, SENSOR_CHAN_ACCEL_XYZ);
	int disable_ret = sensor_trigger_set(imu, &imu_accel_trigger, NULL);

	if (disable_ret < 0) {
		/* The driver clears its handler before writing INT1_CTRL; restore it on failure. */
		(void)sensor_trigger_set(imu, &imu_accel_trigger, imu_trigger_handler);
	}
	cleanup_ret = power_down_ret < 0 ? power_down_ret :
		(fetch_ret < 0 ? fetch_ret : disable_ret);
	if (ret == 0 && cleanup_ret < 0) {
		ret = cleanup_ret;
	} else if (cleanup_ret < 0) {
		shell_warn(sh, "IMU INT1 cleanup failed (%d)", cleanup_ret);
	}

	return ret;
}

SHELL_STATIC_SUBCMD_SET_CREATE(sub_imu_cmds, SHELL_CMD(get, NULL, "Get sensor data", cmd_imu_get),
	SHELL_CMD_ARG(interrupt, NULL, "Wait for IMU INT1 data-ready [seconds]", cmd_imu_interrupt,
		      1, 1),
	SHELL_SUBCMD_SET_END);

SHELL_SUBCMD_ADD((hwv), imu, &sub_imu_cmds, "IMU sensor", NULL, 0, 0);

int imu_init(void)
{
	if (!device_is_ready(imu)) {
		return -ENODEV;
	}

	initialized = true;

	return 0;
}
