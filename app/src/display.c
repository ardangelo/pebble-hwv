#include <stdlib.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/display.h>
#include <zephyr/shell/shell.h>
#include <zephyr/sys/util.h>

#define DISP_WIDTH       DT_PROP(DT_CHOSEN(zephyr_display), width)
#define DISP_HEIGHT      DT_PROP(DT_CHOSEN(zephyr_display), height)
#define PIXEL_ROW_BYTES  DIV_ROUND_UP(DISP_WIDTH, 8)
#define FRAME_PITCH_BITS (DISP_WIDTH + 16)
#define FRAME_ROW_BYTES  DIV_ROUND_UP(FRAME_PITCH_BITS, 8)

static const struct device *const disp = DEVICE_DT_GET(DT_CHOSEN(zephyr_display));
static const struct display_buffer_descriptor desc = {
	.buf_size = FRAME_ROW_BYTES * DISP_HEIGHT,
	.width = DISP_WIDTH,
	.height = DISP_HEIGHT,
	.pitch = FRAME_PITCH_BITS,
};
static bool initialized;

static uint8_t *framebuffer(void)
{
	return display_get_framebuffer(disp);
}

static void fill_pixels(bool white)
{
	uint8_t *buf = framebuffer();

	for (uint16_t y = 0; y < DISP_HEIGHT; y++) {
		memset(&buf[y * FRAME_ROW_BYTES], white ? 0xff : 0x00, PIXEL_ROW_BYTES);
	}
}

static void set_pixel(uint16_t x, uint16_t y, bool white)
{
	uint8_t *pixel = &framebuffer()[y * FRAME_ROW_BYTES + x / 8U];
	uint8_t mask = BIT(x % 8U);

	if (white) {
		*pixel |= mask;
	} else {
		*pixel &= (uint8_t)~mask;
	}
}

static int flush(const struct shell *sh, const char *name)
{
	int ret = display_write(disp, 0, 0, &desc, framebuffer());

	if (ret < 0) {
		shell_error(sh, "Failed to write display pattern (%d)", ret);
		return ret;
	}

	shell_print(sh, "%s pattern displayed", name);
	return 0;
}

static int require_initialized(const struct shell *sh)
{
	if (!initialized) {
		shell_error(sh, "Display module not initialized");
		return -EPERM;
	}

	return 0;
}

static int cmd_display_on(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	int ret = require_initialized(sh);
	if (ret < 0) {
		return ret;
	}

	ret = display_blanking_off(disp);
	if (ret < 0) {
		shell_error(sh, "Failed to turn on display (%d)", ret);
		return ret;
	}

	shell_print(sh, "Display ON");
	return 0;
}

static int cmd_display_off(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	int ret = require_initialized(sh);
	if (ret < 0) {
		return ret;
	}

	ret = display_blanking_on(disp);
	if (ret < 0) {
		shell_error(sh, "Failed to turn off display (%d)", ret);
		return ret;
	}

	shell_print(sh, "Display OFF");
	return 0;
}

static int cmd_display_white(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	if (require_initialized(sh) < 0) {
		return -EPERM;
	}
	fill_pixels(true);
	return flush(sh, "White");
}

static int cmd_display_black(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	if (require_initialized(sh) < 0) {
		return -EPERM;
	}
	fill_pixels(false);
	return flush(sh, "Black");
}

static int draw_pattern(const struct shell *sh, unsigned int mode)
{
	if (require_initialized(sh) < 0) {
		return -EPERM;
	}

	fill_pixels(true);
	for (uint16_t y = 0; y < DISP_HEIGHT; y++) {
		for (uint16_t x = 0; x < DISP_WIDTH; x++) {
			bool white = true;

			switch (mode) {
			case 0:
				white = (x % 4U) == 0U;
				break;
			case 1:
				white = (y % 4U) == 0U;
				break;
			case 2:
				white = (((x / 8U) + (y / 8U)) & 1U) == 0U;
				break;
			case 3:
				white = x == 0U || y == 0U || x == DISP_WIDTH - 1U ||
					y == DISP_HEIGHT - 1U;
				break;
			default:
				return -EINVAL;
			}
			set_pixel(x, y, white);
		}
	}

	static const char *const names[] = { "Vertical", "Horizontal", "Checkerboard", "Border" };
	return flush(sh, names[mode]);
}

static int cmd_display_vpattern(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	return draw_pattern(sh, 0);
}

static int cmd_display_hpattern(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	return draw_pattern(sh, 1);
}

static int cmd_display_checker(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	return draw_pattern(sh, 2);
}

static int cmd_display_border(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	return draw_pattern(sh, 3);
}

static int cmd_display_brightness(const struct shell *sh, size_t argc, char **argv)
{
	if (require_initialized(sh) < 0) {
		return -EPERM;
	}
	if (argc < 2) {
		shell_error(sh, "Missing brightness value");
		return -EINVAL;
	}

	unsigned long brightness = strtoul(argv[1], NULL, 0);
	if (brightness > 100U) {
		shell_error(sh, "Invalid brightness value");
		return -EINVAL;
	}

	int ret = display_set_brightness(disp, (uint8_t)brightness);
	if (ret < 0) {
		shell_error(sh, "Failed to set brightness (%d)", ret);
		return ret;
	}

	shell_print(sh, "Brightness set to %lu%%", brightness);
	return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(
	sub_display_cmds, SHELL_CMD(on, NULL, "Turn on display", cmd_display_on),
	SHELL_CMD(off, NULL, "Turn off display", cmd_display_off),
	SHELL_CMD(white, NULL, "Display all-white pattern", cmd_display_white),
	SHELL_CMD(black, NULL, "Display all-black pattern", cmd_display_black),
	SHELL_CMD(vpattern, NULL, "Display vertical-line pattern", cmd_display_vpattern),
	SHELL_CMD(hpattern, NULL, "Display horizontal-line pattern", cmd_display_hpattern),
	SHELL_CMD(checker, NULL, "Display checkerboard pattern", cmd_display_checker),
	SHELL_CMD(border, NULL, "Display border/addressing pattern", cmd_display_border),
	SHELL_CMD_ARG(brightness, NULL, "Set display brightness (0-100)",
		      cmd_display_brightness, 2, 0),
	SHELL_SUBCMD_SET_END);

SHELL_SUBCMD_ADD((hwv), display, &sub_display_cmds, "Display", NULL, 0, 0);

int display_init(void)
{
	if (!device_is_ready(disp)) {
		return -ENODEV;
	}

	initialized = true;
	return 0;
}
