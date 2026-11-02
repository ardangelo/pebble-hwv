#include "charger.h"

#include <hal/nrf_power.h>
#include <zephyr/device.h>
#include <zephyr/drivers/adc.h>
#include <zephyr/shell/shell.h>

#define BATTERY_NODE DT_NODELABEL(battery_voltage)

static const struct adc_dt_spec battery_adc = ADC_DT_SPEC_GET_BY_IDX(BATTERY_NODE, 0);
static bool initialized;

static int cmd_charger_status(const struct shell *sh, size_t argc, char **argv)
{
	int16_t raw;
	struct adc_sequence sequence = { .buffer = &raw, .buffer_size = sizeof(raw) };
	int ret;
	int32_t millivolts;

	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	if (!initialized) {
		shell_error(sh, "Battery ADC is not initialized");
		return -ENODEV;
	}

	ret = adc_sequence_init_dt(&battery_adc, &sequence);
	if (ret < 0) {
		return ret;
	}
	ret = adc_read(battery_adc.dev, &sequence);
	if (ret < 0) {
		shell_error(sh, "Battery ADC read failed (%d)", ret);
		return ret;
	}
	millivolts = raw;
	ret = adc_raw_to_millivolts_dt(&battery_adc, &millivolts);
	if (ret < 0) {
		shell_error(sh, "Battery ADC conversion failed (%d)", ret);
		return ret;
	}
	if (millivolts < 0) {
		return -ERANGE;
	}
	/* EVT3: VBAT -> R76 300K -> VBAT_VOL -> R77 100K -> GND. */
	millivolts = millivolts * DT_PROP(BATTERY_NODE, full_ohms) /
		DT_PROP(BATTERY_NODE, output_ohms);
	shell_print(sh, "Battery: %d mV (VBAT_VOL P0.31/AIN7, 4:1 divider)", millivolts);
	shell_print(sh, "USB VBUS: %s (nRF VBUS detect)",
		    nrf_power_usbregstatus_vbusdet_get(NRF_POWER) ? "present" : "absent");
	shell_print(sh, "Charge current, battery temperature, charge state, capacity: unavailable");
	shell_print(sh, "TP4054 CHRG# yellow LED, current and termination: external measurement required");
	return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(sub_charger_cmds,
	SHELL_CMD(status, NULL, "Measure VBAT and VBUS; TP4054 needs external checks",
		  cmd_charger_status), SHELL_SUBCMD_SET_END);
SHELL_SUBCMD_ADD((hwv), charger, &sub_charger_cmds, "Battery and charger", NULL, 0, 0);

int charger_init(void)
{
	if (!adc_is_ready_dt(&battery_adc)) {
		return -ENODEV;
	}
	int ret = adc_channel_setup_dt(&battery_adc);
	if (ret == 0) {
		initialized = true;
	}
	return ret;
}
