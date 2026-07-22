#ifndef DT_DRV_COMPAT
#define DT_DRV_COMPAT sharp_ls013b7dh05
#endif

#ifndef LS013B7DH0X_LOG_MODULE
#define LS013B7DH0X_LOG_MODULE ls013b7dh05
#endif

#include <zephyr/drivers/display.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/led.h>
#include <zephyr/drivers/pwm.h>
#include <zephyr/kernel.h>
#include <zephyr/drivers/spi.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(LS013B7DH0X_LOG_MODULE, CONFIG_DISPLAY_LOG_LEVEL);

#define LS013B7DH05_WRITE BIT(0)
#define LS013B7DH05_CLEAR BIT(2)

struct ls013b7dh05_config {
	struct spi_dt_spec spi;
	struct pwm_dt_spec extcomin;
	struct gpio_dt_spec disp;
	const struct device *backlight;
	uint16_t width;
	uint16_t height;
	uint8_t line_width;
	uint8_t *fb;
	uint8_t *tx;
	uint32_t tx_size;
	uint32_t fb_size;
	bool initially_on;
};

static int ls013b7dh05_blanking_on(const struct device *dev)
{
	const struct ls013b7dh05_config *config = dev->config;
	int ret;

	ret = gpio_pin_set_dt(&config->disp, 0);
	if (ret < 0) {
		return ret;
	}

	ret = pwm_set_pulse_dt(&config->extcomin, 0U);
	if (ret < 0) {
		return ret;
	}

	ret = led_off(config->backlight, 0);
	if (ret < 0) {
		return ret;
	}

	return 0;
}

static int ls013b7dh05_blanking_off(const struct device *dev)
{
	const struct ls013b7dh05_config *config = dev->config;
	int ret;

	ret = gpio_pin_set_dt(&config->disp, 1);
	if (ret < 0) {
		return ret;
	}

	ret = pwm_set_pulse_dt(&config->extcomin, PWM_USEC(100));
	if (ret < 0) {
		return ret;
	}

	ret = led_on(config->backlight, 0);
	if (ret < 0) {
		return ret;
	}

	return 0;
}

static void *ls013b7dh05_get_framebuffer(const struct device *dev)
{
	const struct ls013b7dh05_config *config = dev->config;

	return config->fb;
}

static int ls013b7dh05_write(const struct device *dev, uint16_t x, uint16_t y,
			     const struct display_buffer_descriptor *desc, const void *buf)
{
	const struct ls013b7dh05_config *config = dev->config;
	struct spi_buf sbuf;
	struct spi_buf_set sbufs = {.buffers = &sbuf, .count = 1U};
	int ret;

	if (buf != config->fb) {
		LOG_ERR("Unsupported buffer");
		return -EINVAL;
	}

	if (x != 0U || y != 0U || desc->height != config->height ||
	    desc->pitch != config->width + 16U || desc->buf_size < config->fb_size) {
		LOG_ERR("Only a full wire-format frame is supported");
		return -EINVAL;
	}

	uint8_t clear[2] = {LS013B7DH05_CLEAR, 0U};
	sbuf.buf = clear;
	sbuf.len = sizeof(clear);
	ret = spi_write_dt(&config->spi, &sbufs);
	if (ret < 0) {
		return ret;
	}
	k_busy_wait(4U);

	config->tx[0] = LS013B7DH05_WRITE;
	config->tx[1] = 1U;
	sbuf.buf = config->tx;
	sbuf.len = config->tx_size;
	return spi_write_dt(&config->spi, &sbufs);
}

static int ls013b7dh05_set_brightness(const struct device *dev, uint8_t brightness)
{
	const struct ls013b7dh05_config *config = dev->config;

	if (brightness == 0U) {
		return led_off(config->backlight, 0);
	} else {
		int ret;

		ret = led_set_brightness(config->backlight, 0, brightness);
		if (ret < 0) {
			return ret;
		}

		return led_on(config->backlight, 0);
	}
}

static void ls013b7dh05_get_capabilities(const struct device *dev,
					 struct display_capabilities *capabilities)
{
	const struct ls013b7dh05_config *config = dev->config;

	memset(capabilities, 0, sizeof(*capabilities));

	capabilities->x_resolution = config->width;
	capabilities->y_resolution = config->height;
	capabilities->supported_pixel_formats = PIXEL_FORMAT_MONO01;
	capabilities->current_pixel_format = PIXEL_FORMAT_MONO01;
	capabilities->current_orientation = 0;
}

static int ls013b7dh05_init(const struct device *dev)
{
	const struct ls013b7dh05_config *config = dev->config;
	int ret;

	if (!spi_is_ready_dt(&config->spi)) {
		LOG_ERR("SPI not ready");
		return -ENODEV;
	}

	if (!pwm_is_ready_dt(&config->extcomin)) {
		LOG_ERR("EXTCOMIN PWM not ready");
		return -ENODEV;
	}

	if (!gpio_is_ready_dt(&config->disp)) {
		LOG_ERR("DISP GPIO not ready");
		return -ENODEV;
	}

	if (!device_is_ready(config->backlight)) {
		LOG_ERR("Backlight device not ready");
		return -ENODEV;
	}

	ret = gpio_pin_configure_dt(&config->disp,
				    config->initially_on ? GPIO_OUTPUT_ACTIVE : GPIO_OUTPUT_INACTIVE);
	if (ret < 0) {
		LOG_ERR("Failed to configure DISP GPIO");
		return ret;
	}

	ret = pwm_set_pulse_dt(&config->extcomin, 0U);
	if (ret < 0) {
		return ret;
	}

	ret = led_off(config->backlight, 0);
	if (ret < 0) {
		return ret;
	}

	for (uint8_t line = 0U; line < (config->height - 1U); line++) {
		config->fb[line * (config->width / 8U + 2U) + config->width / 8U + 1U] = line + 2U;
	}

	return 0;
}

static const struct display_driver_api ls013b7dh05_api = {
	.blanking_on = ls013b7dh05_blanking_on,
	.blanking_off = ls013b7dh05_blanking_off,
	.get_framebuffer = ls013b7dh05_get_framebuffer,
	.write = ls013b7dh05_write,
	.set_brightness = ls013b7dh05_set_brightness,
	.get_capabilities = ls013b7dh05_get_capabilities,
};

#define LS013B7DH05_DEFINE(n)                                                                      \
	static uint8_t tx##n[2U + (DT_INST_PROP(n, width) * DT_INST_PROP(n, height)) / 8U +        \
			     DT_INST_PROP(n, height) * 2U];                                       \
                                                                                                   \
	static const struct ls013b7dh05_config ls013b7dh05_config_##n = {                          \
		.spi = SPI_DT_SPEC_INST_GET(n,                                                     \
					    SPI_OP_MODE_MASTER | SPI_WORD_SET(8U) |                \
						    SPI_TRANSFER_LSB | SPI_CS_ACTIVE_HIGH,          \
					    7U),                                                         \
		.disp = GPIO_DT_SPEC_INST_GET(n, disp_gpios),                                      \
		.extcomin = PWM_DT_SPEC_INST_GET(n),                                               \
		.backlight = DEVICE_DT_GET(DT_INST_PHANDLE(n, backlight)),                         \
		.initially_on = DT_INST_PROP_OR(n, initially_on, 0),                                \
		.width = DT_INST_PROP(n, width),                                                   \
		.height = DT_INST_PROP(n, height),                                                 \
		.line_width = DIV_ROUND_UP(DT_INST_PROP(n, width), 8U),                            \
		.tx = tx##n,                                                                        \
		.tx_size = ARRAY_SIZE(tx##n),                                                       \
		.fb = &tx##n[2],                                                                     \
		.fb_size = ARRAY_SIZE(tx##n) - 2U,                                                  \
	};                                                                                         \
                                                                                                   \
	DEVICE_DT_INST_DEFINE(n, &ls013b7dh05_init, NULL, NULL, &ls013b7dh05_config_##n,           \
			      POST_KERNEL, CONFIG_DISPLAY_INIT_PRIORITY, &ls013b7dh05_api);

DT_INST_FOREACH_STATUS_OKAY(LS013B7DH05_DEFINE)
