#include "speaker.h"
#include "cyberdeck_i2s_pins.h"

#include <nrfx_i2s.h>
#include <zephyr/device.h>
#include <zephyr/drivers/clock_control/nrf_clock_control.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/kernel.h>
#include <zephyr/shell/shell.h>
#include <zephyr/sys/onoff.h>

#define DA7212_PLL_STATUS          0x03
#define DA7212_CIF_CTRL            0x1d
#define DA7212_DIG_ROUTING_DAI     0x21
#define DA7212_SR                  0x22
#define DA7212_REFERENCES          0x23
#define DA7212_PLL_FRAC_TOP        0x24
#define DA7212_PLL_FRAC_BOT        0x25
#define DA7212_PLL_INTEGER         0x26
#define DA7212_PLL_CTRL            0x27
#define DA7212_DAI_CLK_MODE        0x28
#define DA7212_DAI_CTRL            0x29
#define DA7212_DIG_ROUTING_DAC     0x2a
#define DA7212_DAC_FILTERS5        0x40
#define DA7212_DAC_L_GAIN          0x45
#define DA7212_DAC_R_GAIN          0x46
#define DA7212_CP_CTRL             0x47
#define DA7212_HP_L_GAIN           0x48
#define DA7212_HP_R_GAIN           0x49
#define DA7212_LINE_GAIN           0x4a
#define DA7212_MIXOUT_L_SELECT     0x4b
#define DA7212_MIXOUT_R_SELECT     0x4c
#define DA7212_SYSTEM_MODES_OUTPUT 0x51
#define DA7212_DAC_L_CTRL          0x69
#define DA7212_DAC_R_CTRL          0x6a
#define DA7212_HP_L_CTRL           0x6b
#define DA7212_HP_R_CTRL           0x6c
#define DA7212_LINE_CTRL           0x6d
#define DA7212_MIXOUT_L_CTRL       0x6e
#define DA7212_MIXOUT_R_CTRL       0x6f
#define DA7212_LDO_CTRL            0x90
#define DA7212_GAIN_RAMP_CTRL      0x92
#define DA7212_CP_VOL_THRESHOLD1   0x95
#define DA7212_CP_DELAY            0x96
#define DA7212_SYSTEM_ACTIVE       0xfd

#define BLOCK_FRAMES     256U
#define BLOCK_WORDS      BLOCK_FRAMES
#define BUFFER_COUNT     2U
#define PLAY_DURATION_MS 600
#define STOP_TIMEOUT_MS  100
#define HFXO_START_TIMEOUT_MS 100

static const struct i2c_dt_spec codec = I2C_DT_SPEC_GET(DT_NODELABEL(da7212));
static const struct device *const i2s_dev = DEVICE_DT_GET(DT_NODELABEL(i2s0));
static const nrfx_i2s_t i2s_instance = NRFX_I2S_INSTANCE(0);
static uint32_t speaker_buffers[BUFFER_COUNT][BLOCK_WORDS];
static struct onoff_manager *hfclk_manager;
static struct onoff_client hfclk_client;
static bool hfclk_requested;
static bool i2s_initialized;
static bool i2s_started;
static volatile bool speaker_running;
static volatile int speaker_stream_error;
static uint8_t next_buffer;
static unsigned int tone_block;
static bool initialized;
K_SEM_DEFINE(speaker_transfer_stopped, 0, 1);

static int hfclk_request(void)
{
	int result;
	int ret;

	hfclk_manager = z_nrf_clock_control_get_onoff(CLOCK_CONTROL_NRF_SUBSYS_HF);
	if (hfclk_manager == NULL) {
		return -ENODEV;
	}

	sys_notify_init_spinwait(&hfclk_client.notify);
	ret = onoff_request(hfclk_manager, &hfclk_client);
	if (ret < 0) {
		return ret;
	}
	int64_t deadline = k_uptime_get() + HFXO_START_TIMEOUT_MS;
	while (sys_notify_fetch_result(&hfclk_client.notify, &result) != 0) {
		if (k_uptime_get() >= deadline) {
			(void)onoff_cancel_or_release(hfclk_manager, &hfclk_client);
			return -ETIMEDOUT;
		}
		k_busy_wait(50);
	}
	if (result < 0) {
		return result;
	}

	hfclk_requested = true;
	return 0;
}

static int hfclk_release(void)
{
	int ret = 0;

	if (hfclk_requested) {
		ret = onoff_release(hfclk_manager);
		hfclk_requested = false;
	}

	return MIN(ret, 0);
}

static int codec_write(uint8_t reg, uint8_t value)
{
	return i2c_reg_write_byte_dt(&codec, reg, value);
}

static int codec_prepare(bool headphones)
{
	static const uint8_t base[][2] = {
		{ DA7212_CIF_CTRL, 0x80 },
		{ DA7212_SYSTEM_ACTIVE, 0x01 },
		{ DA7212_REFERENCES, 0x08 },
		{ DA7212_LDO_CTRL, 0x80 },
		{ DA7212_PLL_FRAC_TOP, 0x04 },
		{ DA7212_PLL_FRAC_BOT, 0xdd },
		{ DA7212_PLL_INTEGER, 0x31 },
		{ DA7212_PLL_CTRL, 0xc0 },
		{ 0xf0, 0x8b }, { 0xf2, 0x03 }, { 0xf0, 0x00 },
		{ DA7212_GAIN_RAMP_CTRL, 0x00 },
		{ DA7212_SR, 0x05 },
		{ DA7212_DIG_ROUTING_DAI, 0x32 },
		{ DA7212_DIG_ROUTING_DAC, 0xba },
		{ DA7212_DAC_L_GAIN, 0x6f },
		{ DA7212_DAC_R_GAIN, 0x6f },
		{ DA7212_DAC_FILTERS5, 0x00 },
	};
	int ret;

	for (size_t i = 0; i < ARRAY_SIZE(base); i++) {
		ret = codec_write(base[i][0], base[i][1]);
		if (ret < 0) {
			return ret;
		}
		if (i == 0U) {
			k_msleep(10);
		} else if (i == 2U) {
			k_msleep(30);
		} else if (i == 10U) {
			k_msleep(40);
		}
	}

	uint8_t pll_status;
	ret = i2c_reg_read_byte_dt(&codec, DA7212_PLL_STATUS, &pll_status);
	if (ret < 0) {
		return ret;
	}
	printk("DA7212 PLL status: 0x%02x\n", pll_status);
	if ((pll_status & 0x07U) != 0x07U) {
		return -EIO;
	}

	if (headphones) {
		static const uint8_t route[][2] = {
			{ DA7212_CP_CTRL, 0xf1 }, { DA7212_CP_VOL_THRESHOLD1, 0x36 },
			{ DA7212_CP_DELAY, 0xa5 }, { DA7212_HP_L_GAIN, 0x39 },
			{ DA7212_HP_R_GAIN, 0x39 }, { DA7212_DAC_L_CTRL, 0x80 },
			{ DA7212_DAC_R_CTRL, 0x80 }, { DA7212_MIXOUT_L_SELECT, 0x08 },
			{ DA7212_MIXOUT_R_SELECT, 0x08 }, { DA7212_MIXOUT_L_CTRL, 0x88 },
			{ DA7212_MIXOUT_R_CTRL, 0x88 }, { DA7212_HP_L_CTRL, 0xa8 },
			{ DA7212_HP_R_CTRL, 0xa8 }, { DA7212_LINE_CTRL, 0x00 },
			{ DA7212_SYSTEM_MODES_OUTPUT, 0xf1 },
		};
		for (size_t i = 0; i < ARRAY_SIZE(route); i++) {
			ret = codec_write(route[i][0], route[i][1]);
			if (ret < 0) {
				return ret;
			}
		}
	} else {
		static const uint8_t route[][2] = {
			{ DA7212_DAC_R_CTRL, 0x80 }, { DA7212_MIXOUT_R_SELECT, 0x08 },
			{ DA7212_MIXOUT_R_CTRL, 0x90 }, { DA7212_LINE_GAIN, 0x30 },
			{ DA7212_LINE_CTRL, 0x80 }, { DA7212_HP_L_CTRL, 0x00 },
			{ DA7212_HP_R_CTRL, 0x00 }, { DA7212_SYSTEM_MODES_OUTPUT, 0x89 },
		};
		for (size_t i = 0; i < ARRAY_SIZE(route); i++) {
			ret = codec_write(route[i][0], route[i][1]);
			if (ret < 0) {
				return ret;
			}
		}
	}

	return 0;
}

static void fill_tone(int16_t *samples, unsigned int block)
{
	for (unsigned int frame = 0; frame < BLOCK_FRAMES; frame++) {
		unsigned int phase = (block * BLOCK_FRAMES + frame) % 64U;
		int16_t sample = phase < 32U ? (int16_t)(phase * 384 - 5952) :
			(int16_t)((63U - phase) * 384 - 5952);
		samples[frame * 2U] = sample;
		samples[frame * 2U + 1U] = sample;
	}
}

static void speaker_data_handler(const nrfx_i2s_buffers_t *released, uint32_t status)
{
	ARG_UNUSED(released);

	if ((status & NRFX_I2S_STATUS_NEXT_BUFFERS_NEEDED) != 0U && speaker_running) {
		nrfx_i2s_buffers_t next = {
			.p_tx_buffer = speaker_buffers[next_buffer],
			.p_rx_buffer = NULL,
			.buffer_size = BLOCK_WORDS,
		};

		fill_tone((int16_t *)speaker_buffers[next_buffer], tone_block++);
		if (nrfx_i2s_next_buffers_set(&i2s_instance, &next) != NRFX_SUCCESS) {
			speaker_stream_error = -EIO;
		}
		next_buffer = (next_buffer + 1U) % BUFFER_COUNT;
	}

	if ((status & NRFX_I2S_STATUS_TRANSFER_STOPPED) != 0U) {
		k_sem_give(&speaker_transfer_stopped);
	}
}

static int direct_i2s_start(void)
{
	nrfx_i2s_config_t config = NRFX_I2S_DEFAULT_CONFIG(
		CYBERDECK_I2S_SCK_PIN, CYBERDECK_I2S_LRCK_PIN, CYBERDECK_I2S_MCK_PIN,
		CYBERDECK_I2S_SDOUT_PIN, CYBERDECK_I2S_SDIN_PIN);
	nrfx_i2s_buffers_t initial = {
		.p_tx_buffer = speaker_buffers[0],
		.p_rx_buffer = NULL,
		.buffer_size = BLOCK_WORDS,
	};
	nrfx_err_t err;
	int ret;

	if (nrfx_i2s_init_check(&i2s_instance)) {
		return -EBUSY;
	}
	ret = hfclk_request();
	if (ret < 0) {
		return ret;
	}

	config.irq_priority = DT_IRQ(DT_NODELABEL(i2s0), priority);
	config.mode = NRF_I2S_MODE_SLAVE;
	config.format = NRF_I2S_FORMAT_I2S;
	config.alignment = NRF_I2S_ALIGN_LEFT;
	config.sample_width = NRF_I2S_SWIDTH_16BIT;
	config.channels = NRF_I2S_CHANNELS_STEREO;
	config.mck_setup = NRF_I2S_MCK_32MDIV8;
	config.ratio = NRF_I2S_RATIO_256X;

	err = nrfx_i2s_init(&i2s_instance, &config, speaker_data_handler);
	if (err != NRFX_SUCCESS) {
		(void)hfclk_release();
		return err == NRFX_ERROR_ALREADY ? -EBUSY : -EIO;
	}
	i2s_initialized = true;
	fill_tone((int16_t *)speaker_buffers[0], 0U);
	next_buffer = 1U;
	tone_block = 1U;
	speaker_stream_error = 0;
	speaker_running = true;

	err = nrfx_i2s_start(&i2s_instance, &initial, 0);
	if (err != NRFX_SUCCESS) {
		speaker_running = false;
		nrfx_i2s_uninit(&i2s_instance);
		i2s_initialized = false;
		(void)hfclk_release();
		return -EIO;
	}
	i2s_started = true;
	return 0;
}

static int direct_i2s_stop(void)
{
	int ret = 0;

	speaker_running = false;
	if (i2s_started) {
		nrfx_i2s_stop(&i2s_instance);
		ret = k_sem_take(&speaker_transfer_stopped, K_MSEC(STOP_TIMEOUT_MS));
		i2s_started = false;
	}
	if (i2s_initialized) {
		nrfx_i2s_uninit(&i2s_instance);
		i2s_initialized = false;
	}
	if (ret == 0) {
		ret = hfclk_release();
	} else {
		(void)hfclk_release();
	}

	return ret;
}

static int codec_shutdown(void)
{
	static const uint8_t shutdown[][2] = {
		{ DA7212_DAI_CLK_MODE, 0x00 },
		{ DA7212_DAI_CTRL, 0x00 },
		{ DA7212_SYSTEM_ACTIVE, 0x00 },
	};
	int first_error = 0;

	for (size_t i = 0; i < ARRAY_SIZE(shutdown); i++) {
		int ret = codec_write(shutdown[i][0], shutdown[i][1]);

		if (first_error == 0 && ret < 0) {
			first_error = ret;
		}
	}

	return first_error;
}

static int play_tone(const struct shell *sh, bool headphones)
{
	int codec_stop_ret;
	int i2s_stop_ret;
	int ret;

	if (!initialized) {
		return -EPERM;
	}

	k_sem_reset(&speaker_transfer_stopped);
	ret = direct_i2s_start();
	if (ret < 0) {
		shell_error(sh, "Fixed-clock nrfx I2S start failed (%d)", ret);
		return ret;
	}
	k_msleep(10);

	ret = codec_prepare(headphones);
	if (ret < 0) {
		goto out;
	}
	ret = codec_write(DA7212_DAI_CTRL, 0x80);
	if (ret < 0) {
		goto out;
	}
	ret = codec_write(DA7212_DAI_CLK_MODE, 0x81);
	if (ret < 0) {
		goto out;
	}

	k_msleep(PLAY_DURATION_MS);
	if (speaker_stream_error < 0) {
		ret = speaker_stream_error;
	}

out:
	codec_stop_ret = codec_shutdown();
	i2s_stop_ret = direct_i2s_stop();
	if (ret == 0 && codec_stop_ret < 0) {
		ret = codec_stop_ret;
	}
	if (ret == 0 && i2s_stop_ret < 0) {
		ret = i2s_stop_ret;
	}
	if (ret < 0) {
		shell_error(sh, "Audio test failed (%d)", ret);
		return ret;
	}

	shell_print(sh, "%s output test passed", headphones ? "Headphone" : "Speaker");
	return 0;
}

static int cmd_speaker_play(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	return play_tone(sh, false);
}

static int cmd_speaker_headphone(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	return play_tone(sh, true);
}

static int cmd_speaker_codec(const struct shell *sh, size_t argc, char **argv)
{
	uint8_t value;
	int ret;

	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	for (uint8_t reg = 0x00; reg <= 0x03; reg++) {
		ret = i2c_reg_read_byte_dt(&codec, reg, &value);
		if (ret < 0) {
			return ret;
		}
		shell_print(sh, "DA7212[0x%02x] = 0x%02x", reg, value);
	}
	return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(
	sub_speaker_cmds, SHELL_CMD(play, NULL, "Play 250 Hz through speaker", cmd_speaker_play),
	SHELL_CMD(headphone, NULL, "Play 250 Hz through both headphone channels", cmd_speaker_headphone),
	SHELL_CMD(codec, NULL, "Read DA7212 identification/status registers", cmd_speaker_codec),
	SHELL_SUBCMD_SET_END);

SHELL_SUBCMD_ADD((hwv), speaker, &sub_speaker_cmds, "DA7212 audio", NULL, 0, 0);

int speaker_init(void)
{
	if (!i2c_is_ready_dt(&codec) || !device_is_ready(i2s_dev)) {
		return -ENODEV;
	}
	initialized = true;
	return 0;
}
