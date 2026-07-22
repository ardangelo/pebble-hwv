#include "speaker.h"

#include <zephyr/drivers/i2c.h>
#include <zephyr/drivers/i2s.h>
#include <zephyr/kernel.h>
#include <zephyr/shell/shell.h>
#include <hal/nrf_i2s.h>

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

#define SAMPLE_RATE_HZ 16000U
/* Zephyr's generic API cannot select a fixed MCK ratio, so apply the authoritative nrfx setup. */
#define BLOCK_FRAMES   256U
#define BLOCK_SIZE     (BLOCK_FRAMES * 2U * sizeof(int16_t))
#define BLOCK_COUNT    4U
#define PLAY_BLOCKS    32U
#define I2S_TIMEOUT_MS 1000

static const struct i2c_dt_spec codec = I2C_DT_SPEC_GET(DT_NODELABEL(da7212));
static const struct device *const i2s = DEVICE_DT_GET(DT_NODELABEL(i2s0));
K_MEM_SLAB_DEFINE_STATIC(speaker_slab, BLOCK_SIZE, BLOCK_COUNT, 4);
static bool initialized;

static void apply_authoritative_i2s_clock(void)
{
	const nrf_i2s_config_t config = {
		.mode = NRF_I2S_MODE_SLAVE,
		.format = NRF_I2S_FORMAT_I2S,
		.alignment = NRF_I2S_ALIGN_LEFT,
		.sample_width = NRF_I2S_SWIDTH_16BIT,
		.channels = NRF_I2S_CHANNELS_STEREO,
		.mck_setup = NRF_I2S_MCK_32MDIV8,
		.ratio = NRF_I2S_RATIO_256X,
	};

	nrf_i2s_configure(NRF_I2S0, &config);
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
	printk("DA7212 PLL status: 0x%02x\n", pll_status);
	if (ret < 0 || (pll_status & 0x03U) != 0x03U) {
		return ret < 0 ? ret : -EIO;
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

static int queue_tone_block(unsigned int block)
{
	void *buffer;
	int ret = k_mem_slab_alloc(&speaker_slab, &buffer, K_MSEC(I2S_TIMEOUT_MS));

	if (ret < 0) {
		return ret;
	}
	fill_tone(buffer, block);
	ret = i2s_write(i2s, buffer, BLOCK_SIZE);
	if (ret < 0) {
		k_mem_slab_free(&speaker_slab, buffer);
	}
	return ret;
}

static int play_tone(const struct shell *sh, bool headphones)
{
	struct i2s_config config = {
		.word_size = 16,
		.channels = 2,
		.format = I2S_FMT_DATA_FORMAT_I2S,
		.options = I2S_OPT_BIT_CLK_SLAVE | I2S_OPT_FRAME_CLK_SLAVE,
		.frame_clk_freq = SAMPLE_RATE_HZ,
		.mem_slab = &speaker_slab,
		.block_size = BLOCK_SIZE,
		.timeout = I2S_TIMEOUT_MS,
	};
	int ret;

	if (!initialized) {
		return -EPERM;
	}

	ret = i2s_configure(i2s, I2S_DIR_TX, &config);
	if (ret < 0) {
		goto out;
	}
	apply_authoritative_i2s_clock();
	ret = queue_tone_block(0);
	if (ret < 0) {
		goto out;
	}
	ret = queue_tone_block(1);
	if (ret < 0) {
		goto out;
	}
	ret = i2s_trigger(i2s, I2S_DIR_TX, I2S_TRIGGER_START);
	if (ret < 0) {
		goto out;
	}
	k_msleep(10);

	ret = codec_prepare(headphones);
	if (ret < 0) {
		goto drop;
	}
	ret = codec_write(DA7212_DAI_CTRL, 0x80);
	if (ret < 0) {
		goto drop;
	}
	ret = codec_write(DA7212_DAI_CLK_MODE, 0x81);
	if (ret < 0) {
		goto drop;
	}

	for (unsigned int block = 2; block < PLAY_BLOCKS; block++) {
		ret = queue_tone_block(block);
		if (ret < 0) {
			goto drop;
		}
	}
	ret = i2s_trigger(i2s, I2S_DIR_TX, I2S_TRIGGER_DRAIN);
	if (ret == 0) {
		k_msleep(100);
	}
	goto out;

drop:
	(void)i2s_trigger(i2s, I2S_DIR_TX, I2S_TRIGGER_DROP);
out:
	(void)codec_write(DA7212_DAI_CLK_MODE, 0x00);
	(void)codec_write(DA7212_DAI_CTRL, 0x00);
	(void)codec_write(DA7212_SYSTEM_ACTIVE, 0x00);
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
	if (!i2c_is_ready_dt(&codec) || !device_is_ready(i2s)) {
		return -ENODEV;
	}
	initialized = true;
	return 0;
}
