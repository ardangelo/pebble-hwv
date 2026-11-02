#include <stdlib.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/clock_control.h>
#include <zephyr/drivers/clock_control/nrf_clock_control.h>
#include <zephyr/drivers/counter.h>
#include <zephyr/kernel.h>
#include <zephyr/shell/shell.h>
#include <zephyr/sys/timeutil.h>
#include <zephyr/sys/onoff.h>

#define MEASURE_TIME_S 10

static const struct device *const sys_clock = DEVICE_DT_GET_ONE(nordic_nrf_clock);
static const struct device *const timer = DEVICE_DT_GET(DT_NODELABEL(timer2));
static bool initialized;

static int cmd_lfxo_test(const struct shell *sh, size_t argc, char **argv)
{
	struct timeutil_sync_config sync_config = { 0 };
	struct timeutil_sync_state sync_state = { 0 };
	struct onoff_manager *hf_mgr;
	struct onoff_client hf_cli;
	uint64_t counter_ref = 0U;
	int64_t deadline;
	bool timer_started = false;
	int ret;

	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	if (!initialized) {
		return -ENODEV;
	}
	hf_mgr = z_nrf_clock_control_get_onoff(CLOCK_CONTROL_NRF_SUBSYS_HF);
	if (hf_mgr == NULL) {
		return -ENODEV;
	}
	sys_notify_init_spinwait(&hf_cli.notify);
	ret = onoff_request(hf_mgr, &hf_cli);
	if (ret < 0) {
		return ret;
	}
	deadline = k_uptime_get() + 1000;
	while (sys_notify_fetch_result(&hf_cli.notify, &ret) != 0) {
		if (k_uptime_get() >= deadline) {
			ret = -ETIMEDOUT;
			(void)onoff_cancel_or_release(hf_mgr, &hf_cli);
			shell_error(sh, "HFXO startup timed out");
			return ret;
		}
		k_busy_wait(50);
	}
	if (ret < 0) {
		(void)onoff_release(hf_mgr);
		return ret;
	}

	sync_config.ref_Hz = counter_get_frequency(timer);
	sync_config.local_Hz = CONFIG_SYS_CLOCK_TICKS_PER_SEC;
	sync_state.cfg = &sync_config;
	if (sync_config.ref_Hz == 0 || counter_get_top_value(timer) != UINT32_MAX) {
		ret = -ENOTSUP;
		goto cleanup;
	}
	ret = counter_start(timer);
	if (ret < 0) {
		goto cleanup;
	}
	timer_started = true;
	shell_print(sh, "Comparing LFXO-based ticks to HFXO timer at %u Hz over %d s",
		    sync_config.ref_Hz, MEASURE_TIME_S);
	deadline = k_uptime_get() + (MEASURE_TIME_S + 5) * MSEC_PER_SEC;
	for (;;) {
		uint32_t ctr;
		struct timeutil_sync_instant inst;

		if (k_uptime_get() >= deadline) {
			ret = -ETIMEDOUT;
			shell_error(sh, "LFXO comparison timed out");
			break;
		}
		ret = counter_get_value(timer, &ctr);
		if (ret < 0) {
			break;
		}
		counter_ref += ctr - (uint32_t)counter_ref;
		inst.ref = counter_ref;
		inst.local = k_uptime_ticks();
		ret = timeutil_sync_state_update(&sync_state, &inst);
		if (ret < 0) {
			break;
		}
		if (ret != 0) {
			float skew = timeutil_sync_estimate_skew(&sync_state);
			int32_t ppb = timeutil_sync_skew_to_ppb(skew);

			shell_print(sh, "LFXO error: %d ppb", ppb);
			ret = 0;
			break;
		}
		k_sleep(K_SECONDS(MEASURE_TIME_S));
	}

cleanup:
	if (timer_started) {
		int stop_ret = counter_stop(timer);
		if (ret == 0 && stop_ret < 0) {
			ret = stop_ret;
		}
	}
	int release_ret = onoff_release(hf_mgr);
	if (ret == 0 && release_ret < 0) {
		ret = release_ret;
	}
	if (ret < 0) {
		shell_error(sh, "LFXO test failed (%d)", ret);
	}
	return ret;
}

SHELL_STATIC_SUBCMD_SET_CREATE(sub_lfxo_cmds,
			       SHELL_CMD(test, NULL, "Test LFXO accuracy", cmd_lfxo_test),
			       SHELL_SUBCMD_SET_END);

SHELL_SUBCMD_ADD((hwv), lfxo, &sub_lfxo_cmds, "LFXO", NULL, 0, 0);

int lfxo_init(void)
{
	if (!device_is_ready(sys_clock) || !device_is_ready(timer)) {
		return -ENODEV;
	}

	initialized = true;

	return 0;
}
