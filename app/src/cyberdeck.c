#include "cyberdeck.h"

#include <stdlib.h>

#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/drivers/led_strip.h>
#include <zephyr/drivers/pwm.h>
#include <zephyr/kernel.h>
#include <zephyr/shell/shell.h>
#include <zephyr/sys/reboot.h>
#include <zephyr/sys/util.h>

#define TCA_REG_CFG            0x01
#define TCA_REG_INT_STAT       0x02
#define TCA_REG_KEY_LCK_EC     0x03
#define TCA_REG_KEY_EVENT_A    0x04
#define TCA_REG_KP_STAT1       0x11
#define TCA_REG_KP_STAT2       0x12
#define TCA_REG_KP_STAT3       0x13
#define TCA_REG_GPIO_INT_STAT1 0x1a
#define TCA_REG_GPIO_INT_STAT2 0x1b
#define TCA_REG_GPIO_INT_STAT3 0x1c
#define TCA_REG_KP_GPIO1       0x1d
#define TCA_REG_KP_GPIO2       0x1e
#define TCA_REG_GPI_EM1        0x20
#define TCA_REG_GPI_EM2        0x21
#define TCA_REG_GPI_EM3        0x22
#define TCA_REG_GPIO_DIR1      0x23
#define TCA_REG_GPIO_DIR2      0x24
#define TCA_REG_GPIO_DIR3      0x25
#define TCA_REG_GPIO_INT_LVL1  0x26
#define TCA_REG_GPIO_INT_LVL2  0x27
#define TCA_REG_GPIO_INT_LVL3  0x28

#define OFN_REG_PID      0x00
#define OFN_REG_MOTION   0x02
#define OFN_REG_DELTA_Y  0x03
#define OFN_REG_DELTA_X  0x04
#define OFN_REG_RECOVERY 0x06
#define OFN_REG_CLICK    0x34
#define OFN_PRODUCT_ID   0x30
#define OFN_MOTION_BIT   BIT(7)
#define OFN_CLICK_BITS   (BIT(4) | BIT(5))

#define KBD_BACKLIGHT_MAX_PERCENT 67U

#define CONTROL_NODE DT_NODELABEL(cyberdeck_hwv)
#define KBD_BACKLIGHT_NODE DT_NODELABEL(kbd_backlight_led)

static const struct i2c_dt_spec tca = I2C_DT_SPEC_GET(DT_NODELABEL(tca8418));
static const struct i2c_dt_spec ofn = I2C_DT_SPEC_GET(DT_NODELABEL(ofn83));
static const struct i2c_dt_spec drv2604 = I2C_DT_SPEC_GET(DT_NODELABEL(drv2604));
static const struct i2c_dt_spec opt3001 = I2C_DT_SPEC_GET(DT_NODELABEL(opt3001));
static const struct i2c_dt_spec da7212 = I2C_DT_SPEC_GET(DT_NODELABEL(da7212));
static const struct i2c_dt_spec lsm6dso = I2C_DT_SPEC_GET(DT_NODELABEL(lsm6dso));
static const struct gpio_dt_spec tca_int =
	GPIO_DT_SPEC_GET(DT_NODELABEL(tca8418), int_gpios);
static const struct gpio_dt_spec ofn_int = GPIO_DT_SPEC_GET(DT_NODELABEL(ofn83), int_gpios);
static const struct gpio_dt_spec ofn_reset =
	GPIO_DT_SPEC_GET(DT_NODELABEL(ofn83), reset_gpios);
static const struct gpio_dt_spec ofn_shutdown =
	GPIO_DT_SPEC_GET(DT_NODELABEL(ofn83), shutdown_gpios);
static const struct gpio_dt_spec peripheral_power =
	GPIO_DT_SPEC_GET(CONTROL_NODE, peripheral_power_gpios);
static const struct gpio_dt_spec lcd_select = GPIO_DT_SPEC_GET(CONTROL_NODE, lcd_select_gpios);
static const struct gpio_dt_spec ec_key = GPIO_DT_SPEC_GET(CONTROL_NODE, ec_key_gpios);
static const struct pwm_dt_spec kbd_backlight = PWM_DT_SPEC_GET(KBD_BACKLIGHT_NODE);
static const struct device *const rgb = DEVICE_DT_GET(DT_CHOSEN(zephyr_led_strip));

static bool initialized;
static bool tca_ready;
static bool ofn_ready;

static int reg_write(const struct i2c_dt_spec *dev, uint8_t reg, uint8_t value)
{
	return i2c_reg_write_byte_dt(dev, reg, value);
}

static int tca_initialize(void)
{
	static const uint8_t setup[][2] = {
		{ TCA_REG_GPIO_DIR1, 0x00 }, { TCA_REG_GPIO_DIR2, 0x00 },
		{ TCA_REG_GPIO_DIR3, 0x00 }, { TCA_REG_GPI_EM1, 0xff },
		{ TCA_REG_GPI_EM2, 0xff }, { TCA_REG_GPI_EM3, 0xff },
		{ TCA_REG_GPIO_INT_LVL1, 0x00 }, { TCA_REG_GPIO_INT_LVL2, 0x00 },
		{ TCA_REG_GPIO_INT_LVL3, 0x00 }, { TCA_REG_GPIO_INT_STAT1, 0xff },
		{ TCA_REG_GPIO_INT_STAT2, 0xff }, { TCA_REG_GPIO_INT_STAT3, 0xff },
		{ TCA_REG_KP_GPIO1, 0x7f }, { TCA_REG_KP_GPIO2, 0x7f },
		{ TCA_REG_CFG, 0x03 }, { TCA_REG_INT_STAT, 0x0b },
	};
	uint8_t value;
	int ret = i2c_reg_read_byte_dt(&tca, TCA_REG_CFG, &value);

	if (ret < 0) {
		tca_ready = false;
		return ret;
	}
	for (size_t i = 0; i < ARRAY_SIZE(setup); i++) {
		ret = reg_write(&tca, setup[i][0], setup[i][1]);
		if (ret < 0) {
			tca_ready = false;
			return ret;
		}
	}
	k_msleep(50);
	for (unsigned int i = 0; i < 16U; i++) {
		ret = i2c_reg_read_byte_dt(&tca, TCA_REG_KEY_EVENT_A, &value);
		if (ret < 0 || value == 0U) {
			break;
		}
	}
	(void)reg_write(&tca, TCA_REG_INT_STAT, 0x0b);
	tca_ready = true;
	return 0;
}

static int ofn_initialize(void)
{
	static const uint8_t setup[][2] = {
		{ 0x7f, 0x00 }, { 0x09, 0x5a }, { 0x0d, 0x0a }, { 0x1d, 0x1b },
		{ 0x4c, 0x90 }, { 0x4d, 0x17 }, { 0x4f, 0x10 }, { 0x2f, 0x07 },
		{ 0x7f, 0x01 }, { 0x27, 0x47 }, { 0x23, 0x32 }, { 0x2e, 0x48 },
		{ 0x38, 0xf5 }, { 0x7f, 0x00 }, { 0x09, 0x00 },
	};
	uint8_t pid;
	int ret;

	(void)gpio_pin_set_dt(&ofn_shutdown, 0);
	k_msleep(120);
	(void)gpio_pin_set_dt(&ofn_reset, 1);
	k_msleep(1);
	k_busy_wait(5);
	(void)gpio_pin_set_dt(&ofn_reset, 0);
	k_busy_wait(5);
	k_msleep(30);

	ret = i2c_reg_read_byte_dt(&ofn, OFN_REG_PID, &pid);
	if (ret < 0 || pid != OFN_PRODUCT_ID) {
		ofn_ready = false;
		return ret < 0 ? ret : -ENODEV;
	}
	for (size_t i = 0; i < ARRAY_SIZE(setup); i++) {
		ret = reg_write(&ofn, setup[i][0], setup[i][1]);
		if (ret < 0) {
			ofn_ready = false;
			return ret;
		}
	}
	(void)i2c_reg_read_byte_dt(&ofn, OFN_REG_CLICK, &pid);
	ofn_ready = true;
	return 0;
}

static int probe_register(const struct shell *sh, const char *name,
			  const struct i2c_dt_spec *dev, uint8_t reg, int expected)
{
	uint8_t value;
	int ret = i2c_reg_read_byte_dt(dev, reg, &value);

	if (ret < 0) {
		shell_error(sh, "HWV probe %-8s FAIL err=%d", name, ret);
		return ret;
	}
	if (expected >= 0 && value != (uint8_t)expected) {
		shell_error(sh, "HWV probe %-8s FAIL value=0x%02x expected=0x%02x", name, value,
			expected);
		return -ENODEV;
	}

	shell_print(sh, "HWV probe %-8s PASS value=0x%02x", name, value);
	return 0;
}

static int cmd_probe(const struct shell *sh, size_t argc, char **argv)
{
	int result = 0;
	int ret;

	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	ret = probe_register(sh, "DRV2604", &drv2604, 0x00, -1);
	result = result == 0 ? ret : result;
	ret = probe_register(sh, "OPT3001", &opt3001, 0x7e, 0x54);
	result = result == 0 ? ret : result;
	ret = probe_register(sh, "DA7212", &da7212, 0x00, -1);
	result = result == 0 ? ret : result;
	ret = probe_register(sh, "LSM6DSO", &lsm6dso, 0x0f, 0x6c);
	result = result == 0 ? ret : result;
	ret = probe_register(sh, "TCA8418", &tca, TCA_REG_CFG, -1);
	result = result == 0 ? ret : result;
	ret = probe_register(sh, "OFN83", &ofn, OFN_REG_PID, OFN_PRODUCT_ID);
	result = result == 0 ? ret : result;

	shell_print(sh, "HWV probe summary: %s", result == 0 ? "PASS" : "FAIL");
	return result;
}

static int cmd_status(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	shell_print(sh, "HWV status power=%d lcd_sel=%d ec=%d kbd_int=%d ofn_int=%d",
		    gpio_pin_get_dt(&peripheral_power), gpio_pin_get_dt(&lcd_select),
		    gpio_pin_get_dt(&ec_key), gpio_pin_get_dt(&tca_int), gpio_pin_get_dt(&ofn_int));
	shell_print(sh, "HWV status tca=%s ofn83=%s rgb=%s kbd_backlight=%s battery_adc=SKIP",
		    tca_ready ? "PASS" : "FAIL", ofn_ready ? "PASS" : "FAIL",
		    device_is_ready(rgb) ? "PASS" : "FAIL",
		    pwm_is_ready_dt(&kbd_backlight) ? "PASS" : "FAIL");
	return tca_ready && ofn_ready && device_is_ready(rgb) && pwm_is_ready_dt(&kbd_backlight)
		       ? 0
		       : -ENODEV;
}

static int set_power(const struct shell *sh, bool on)
{
	int ret = gpio_pin_set_dt(&peripheral_power, on ? 1 : 0);

	if (ret < 0) {
		return ret;
	}
	if (!on) {
		tca_ready = false;
		ofn_ready = false;
		shell_print(sh, "nRF peripheral power OFF");
		return 0;
	}

	k_msleep(150);
	ret = tca_initialize();
	if (ret == 0) {
		ret = ofn_initialize();
	}
	if (ret < 0) {
		shell_error(sh, "Peripheral power restored but device reinitialization failed (%d)", ret);
		return ret;
	}
	shell_print(sh, "nRF peripheral power ON; TCA8418 and OFN83 restored");
	return 0;
}

static int cmd_power_on(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	return set_power(sh, true);
}

static int cmd_power_off(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	return set_power(sh, false);
}

static int cmd_power_cycle(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	int ret = set_power(sh, false);
	if (ret < 0) {
		return ret;
	}
	k_msleep(100);
	return set_power(sh, true);
}

static int cmd_rgb(const struct shell *sh, size_t argc, char **argv)
{
	struct led_rgb pixel = { 0 };
	int ret;

	if (argc < 2) {
		return -EINVAL;
	}
	if (!strcmp(argv[1], "red")) {
		pixel.r = 32;
	} else if (!strcmp(argv[1], "green")) {
		pixel.g = 32;
	} else if (!strcmp(argv[1], "blue")) {
		pixel.b = 32;
	} else if (!strcmp(argv[1], "white")) {
		pixel.r = pixel.g = pixel.b = 32;
	} else if (strcmp(argv[1], "off")) {
		shell_error(sh, "Color must be red, green, blue, white, or off");
		return -EINVAL;
	}

	ret = led_strip_update_rgb(rgb, &pixel, 1);
	if (ret < 0) {
		return ret;
	}
	shell_print(sh, "RGB %s", argv[1]);
	return 0;
}

static int cmd_kbd_backlight(const struct shell *sh, size_t argc, char **argv)
{
	unsigned long brightness;
	uint64_t pulse;
	int ret;

	if (argc < 2) {
		return -EINVAL;
	}
	brightness = strtoul(argv[1], NULL, 0);
	if (brightness > 100U) {
		return -EINVAL;
	}
	pulse = (uint64_t)kbd_backlight.period * brightness * KBD_BACKLIGHT_MAX_PERCENT / 10000U;
	ret = pwm_set_pulse_dt(&kbd_backlight, (uint32_t)pulse);
	if (ret < 0) {
		return ret;
	}
	shell_print(sh, "Keyboard backlight %lu%%", brightness);
	return 0;
}

static int cmd_ec_check(const struct shell *sh, size_t argc, char **argv)
{
	unsigned long timeout_s = argc > 1 ? strtoul(argv[1], NULL, 0) : 10U;
	bool pressed = false;
	bool released = false;
	int last = gpio_pin_get_dt(&ec_key);
	int64_t deadline = k_uptime_get() + timeout_s * MSEC_PER_SEC;

	shell_print(sh, "Press and release the EC/end key");
	while (k_uptime_get() < deadline && !(pressed && released)) {
		int current = gpio_pin_get_dt(&ec_key);
		if (current < 0) {
			return current;
		}
		if (current != last) {
			shell_print(sh, "EC key %s", current ? "pressed" : "released");
			pressed |= current != 0;
			released |= current == 0 && pressed;
			last = current;
		}
		k_msleep(10);
	}
	if (!pressed || !released) {
		shell_error(sh, "EC key check timed out");
		return -ETIMEDOUT;
	}
	shell_print(sh, "EC key check passed");
	return 0;
}

static int tca_read_event(uint8_t *event)
{
	uint8_t count;
	int ret = i2c_reg_read_byte_dt(&tca, TCA_REG_KEY_LCK_EC, &count);

	if (ret < 0) {
		return ret;
	}
	if ((count & 0x0fU) == 0U) {
		*event = 0;
		return 0;
	}
	return i2c_reg_read_byte_dt(&tca, TCA_REG_KEY_EVENT_A, event);
}

static const uint8_t expected_keys[] = {
	2, 3, 4, 5, 6, 12, 13, 14, 15, 16, 21, 22, 23, 24, 25, 26, 32, 33, 34,
	35, 36, 41, 42, 43, 44, 45, 46, 51, 52, 53, 54, 55, 56, 62, 63, 64, 65, 66,
};

static int cmd_keyboard_check(const struct shell *sh, size_t argc, char **argv)
{
	unsigned long timeout_s = argc > 1 ? strtoul(argv[1], NULL, 0) : 30U;
	bool down[128] = { 0 };
	bool up[128] = { 0 };
	int64_t deadline = k_uptime_get() + timeout_s * MSEC_PER_SEC;
	int ret;

	if (!tca_ready || timeout_s == 0U || timeout_s > 300U) {
		return !tca_ready ? -ENODEV : -EINVAL;
	}
	shell_print(sh, "Press and release every Q20 key; raw TCA8418 events follow");
	while (k_uptime_get() < deadline) {
		uint8_t event;
		ret = tca_read_event(&event);
		if (ret < 0) {
			return ret;
		}
		if (event != 0U) {
			uint8_t code = event & 0x7fU;
			bool is_down = (event & 0x80U) != 0U;
			down[code] |= is_down;
			up[code] |= !is_down;
			shell_print(sh, "KBD raw=%u %s", code, is_down ? "down" : "up");
		}
		(void)reg_write(&tca, TCA_REG_INT_STAT, 0x01);
		k_msleep(5);
	}

	unsigned int passed = 0;
	for (size_t i = 0; i < ARRAY_SIZE(expected_keys); i++) {
		uint8_t code = expected_keys[i];
		if (down[code] && up[code]) {
			passed++;
		} else {
			shell_error(sh, "KBD missing raw=%u%s%s", code, down[code] ? "" : " down",
				    up[code] ? "" : " up");
		}
	}
	shell_print(sh, "KBD summary %u/%u keys passed", passed, ARRAY_SIZE(expected_keys));
	return passed == ARRAY_SIZE(expected_keys) ? 0 : -ENODATA;
}

static int cmd_keyboard_status(const struct shell *sh, size_t argc, char **argv)
{
	uint8_t cfg;
	uint8_t stat;
	uint8_t count;
	int ret;

	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	ret = i2c_reg_read_byte_dt(&tca, TCA_REG_CFG, &cfg);
	ret |= i2c_reg_read_byte_dt(&tca, TCA_REG_INT_STAT, &stat);
	ret |= i2c_reg_read_byte_dt(&tca, TCA_REG_KEY_LCK_EC, &count);
	if (ret < 0) {
		return ret;
	}
	shell_print(sh, "TCA8418 cfg=0x%02x stat=0x%02x count=%u int=%d", cfg, stat,
		    count & 0x0fU, gpio_pin_get_dt(&tca_int));
	return 0;
}

static int cmd_ofn_id(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	return probe_register(sh, "OFN83", &ofn, OFN_REG_PID, OFN_PRODUCT_ID);
}

static int cmd_ofn_motion(const struct shell *sh, size_t argc, char **argv)
{
	unsigned long timeout_s = argc > 1 ? strtoul(argv[1], NULL, 0) : 15U;
	bool x_pos = false, x_neg = false, y_pos = false, y_neg = false;
	unsigned int events = 0, clicks = 0, errors = 0;
	int64_t deadline = k_uptime_get() + timeout_s * MSEC_PER_SEC;

	if (!ofn_ready || timeout_s == 0U || timeout_s > 120U) {
		return !ofn_ready ? -ENODEV : -EINVAL;
	}
	shell_print(sh, "Move the 83_OFN in all four directions");
	while (k_uptime_get() < deadline && !(x_pos && x_neg && y_pos && y_neg)) {
		uint8_t motion, click;
		int ret = i2c_reg_read_byte_dt(&ofn, OFN_REG_MOTION, &motion);
		ret |= i2c_reg_read_byte_dt(&ofn, OFN_REG_CLICK, &click);
		if (ret < 0) {
			errors++;
			k_msleep(8);
			continue;
		}
		clicks += (click & OFN_CLICK_BITS) != 0U;
		if (motion & OFN_MOTION_BIT) {
			uint8_t raw_x, raw_y;
			ret = i2c_reg_read_byte_dt(&ofn, OFN_REG_DELTA_Y, &raw_y);
			ret |= i2c_reg_read_byte_dt(&ofn, OFN_REG_DELTA_X, &raw_x);
			if (ret < 0) {
				errors++;
			} else {
				int8_t dx = (int8_t)raw_x;
				int8_t dy = (int8_t)raw_y;
				x_pos |= dx > 0;
				x_neg |= dx < 0;
				y_pos |= dy > 0;
				y_neg |= dy < 0;
				events++;
				shell_print(sh, "OFN dx=%d dy=%d click=0x%02x int=%d", dx, dy, click,
					    gpio_pin_get_dt(&ofn_int));
			}
		}
		k_msleep(8);
	}

	bool pass = x_pos && x_neg && y_pos && y_neg && errors == 0U;
	shell_print(sh, "OFN summary events=%u clicks=%u errors=%u directions=%c%c%c%c %s", events,
		    clicks, errors, x_pos ? '+' : '-', x_neg ? '+' : '-', y_pos ? '+' : '-',
		    y_neg ? '+' : '-', pass ? "PASS" : "FAIL");
	if (!pass && errors > 0U) {
		(void)reg_write(&ofn, OFN_REG_RECOVERY, 0x82);
	}
	return pass ? 0 : -ENODATA;
}

static int cmd_usb_echo(const struct shell *sh, size_t argc, char **argv)
{
	if (argc < 2) {
		return -EINVAL;
	}
	shell_print(sh, "USB RX/TX PASS token=%s", argv[1]);
	return 0;
}

static int cmd_reset(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	shell_print(sh, "RESET requested; expect a fresh HWV boot banner");
	k_msleep(100);
	sys_reboot(SYS_REBOOT_COLD);
	return 0;
}

static int cmd_fixture(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	shell_print(sh, "GUIDED charge: verify TP4054 yellow CHRG# LED/current/termination");
	shell_print(sh, "GUIDED power: verify USB-only, battery-only, and source switchover");
	shell_print(sh, "GUIDED rails: verify 5V, 3V3, and 1V8 test points under load");
	shell_print(sh, "GUIDED debug: verify SWD program/halt/reset/read and USB CDC traffic");
	shell_print(sh, "SKIP battery ADC: no confirmed EVT3 SAADC assignment");
	return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(
	sub_power_cmds, SHELL_CMD(on, NULL, "Enable nRF peripheral power", cmd_power_on),
	SHELL_CMD(off, NULL, "Disable nRF peripheral power", cmd_power_off),
	SHELL_CMD(cycle, NULL, "Cycle nRF peripheral power and reprobe", cmd_power_cycle),
	SHELL_SUBCMD_SET_END);

SHELL_STATIC_SUBCMD_SET_CREATE(
	sub_keyboard_cmds, SHELL_CMD(status, NULL, "Show TCA8418 status", cmd_keyboard_status),
	SHELL_CMD_ARG(check, NULL, "Check all Q20 keys [seconds]", cmd_keyboard_check, 1, 1),
	SHELL_SUBCMD_SET_END);

SHELL_STATIC_SUBCMD_SET_CREATE(
	sub_ofn_cmds, SHELL_CMD(id, NULL, "Verify OFN83 product ID", cmd_ofn_id),
	SHELL_CMD_ARG(motion, NULL, "Check OFN83 motion [seconds]", cmd_ofn_motion, 1, 1),
	SHELL_SUBCMD_SET_END);

SHELL_STATIC_SUBCMD_SET_CREATE(
	sub_cyberdeck_cmds, SHELL_CMD(probe, NULL, "Probe expected I2C devices", cmd_probe),
	SHELL_CMD(status, NULL, "Show fixture-readable Cyberdeck status", cmd_status),
	SHELL_CMD(power, &sub_power_cmds, "Peripheral power", NULL),
	SHELL_CMD_ARG(rgb, NULL, "Set RGB: red|green|blue|white|off", cmd_rgb, 2, 0),
	SHELL_CMD_ARG(backlight, NULL, "Set Q20 backlight 0-100", cmd_kbd_backlight, 2, 0),
	SHELL_CMD_ARG(ec, NULL, "Check EC/end key [seconds]", cmd_ec_check, 1, 1),
	SHELL_CMD(keyboard, &sub_keyboard_cmds, "Q20 keyboard", NULL),
	SHELL_CMD(ofn, &sub_ofn_cmds, "83_OFN optical navigation", NULL),
	SHELL_CMD(fixture, NULL, "Print guided fixture checks", cmd_fixture),
	SHELL_CMD_ARG(usb, NULL, "Echo a token over USB CDC", cmd_usb_echo, 2, 0),
	SHELL_CMD(reset, NULL, "Cold-reset nRF and print a fresh boot banner", cmd_reset),
	SHELL_SUBCMD_SET_END);

SHELL_SUBCMD_ADD((hwv), cyberdeck, &sub_cyberdeck_cmds, "Cyberdeck EVT3", NULL, 0, 0);

int cyberdeck_init(void)
{
	if (!i2c_is_ready_dt(&tca) || !i2c_is_ready_dt(&ofn) ||
	    !gpio_is_ready_dt(&peripheral_power) || !gpio_is_ready_dt(&lcd_select) ||
	    !gpio_is_ready_dt(&ec_key) || !gpio_is_ready_dt(&tca_int) ||
	    !gpio_is_ready_dt(&ofn_int) || !gpio_is_ready_dt(&ofn_reset) ||
	    !gpio_is_ready_dt(&ofn_shutdown) || !device_is_ready(rgb) ||
	    !pwm_is_ready_dt(&kbd_backlight)) {
		return -ENODEV;
	}

	int ret = gpio_pin_configure_dt(&peripheral_power, GPIO_OUTPUT_ACTIVE);
	ret |= gpio_pin_configure_dt(&lcd_select, GPIO_OUTPUT_INACTIVE);
	ret |= gpio_pin_configure_dt(&ec_key, GPIO_INPUT);
	ret |= gpio_pin_configure_dt(&tca_int, GPIO_INPUT);
	ret |= gpio_pin_configure_dt(&ofn_int, GPIO_INPUT);
	ret |= gpio_pin_configure_dt(&ofn_reset, GPIO_OUTPUT_INACTIVE);
	ret |= gpio_pin_configure_dt(&ofn_shutdown, GPIO_OUTPUT_INACTIVE);
	ret |= pwm_set_pulse_dt(&kbd_backlight, 0U);
	if (ret < 0) {
		return ret;
	}

	k_msleep(150);
	ret = tca_initialize();
	if (ret < 0) {
		return ret;
	}
	ret = ofn_initialize();
	if (ret < 0) {
		return ret;
	}

	initialized = true;
	return 0;
}
