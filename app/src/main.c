#include "display.h"
#include "flash.h"
#include "haptic.h"
#include "imu.h"
#include "lfxo.h"
#include "light.h"
#include "speaker.h"

#if defined(CONFIG_BOARD_ASTERIX)
#include "buttons.h"
#include "charger.h"
#include "mag.h"
#include "mic.h"
#include "press.h"
#elif defined(CONFIG_BOARD_CYBERDECK_EVT3)
#include "cyberdeck.h"
#include "mic_i2s.h"
#endif

#include <stdio.h>

#include <zephyr/shell/shell.h>
#include <zephyr/sys/util.h>

#include <app_version.h>

SHELL_SUBCMD_SET_CREATE(hwv_cmds, (hwv));
SHELL_CMD_REGISTER(hwv, &hwv_cmds, "HWV commands", NULL);

static void report_init(const char *name, int ret)
{
	if (ret < 0) {
		printf("Failed to initialize %s module (%d)\n", name, ret);
	}
}

int main(void)
{
#if defined(CONFIG_BOARD_CYBERDECK_EVT3)
	report_init("Cyberdeck", cyberdeck_init());
#endif

	printf("HWV v%s-%s\n", APP_VERSION_STRING, STRINGIFY(APP_BUILD_VERSION));

#if defined(CONFIG_BOARD_ASTERIX)
	report_init("buttons", buttons_init());
	report_init("charger", charger_init());
	report_init("magnetometer", mag_init());
	report_init("microphone", mic_init());
	report_init("pressure sensor", press_init());
#elif defined(CONFIG_BOARD_CYBERDECK_EVT3)
	report_init("I2S microphone", mic_i2s_init());
#endif

	report_init("display", display_init());
	report_init("flash", flash_init());
	report_init("haptic", haptic_init());
	report_init("light sensor", light_init());
	report_init("IMU", imu_init());
	report_init("speaker", speaker_init());
	report_init("LFXO", lfxo_init());

	return 0;
}
