#include "mic_i2s.h"

#include <stdlib.h>

#include <zephyr/drivers/i2c.h>
#include <zephyr/drivers/i2s.h>
#include <zephyr/kernel.h>
#include <zephyr/shell/shell.h>
#include <hal/nrf_i2s.h>

#define DA7212_PLL_STATUS      0x03
#define DA7212_CIF_CTRL        0x1d
#define DA7212_DIG_ROUTING_DAI 0x21
#define DA7212_SR              0x22
#define DA7212_REFERENCES      0x23
#define DA7212_PLL_FRAC_TOP    0x24
#define DA7212_PLL_FRAC_BOT    0x25
#define DA7212_PLL_INTEGER     0x26
#define DA7212_PLL_CTRL        0x27
#define DA7212_DAI_CLK_MODE    0x28
#define DA7212_DAI_CTRL        0x29
#define DA7212_ALC_CTRL3       0x2b
#define DA7212_MIXIN_L_SELECT  0x32
#define DA7212_MIXIN_R_SELECT  0x33
#define DA7212_MIC_1_GAIN      0x39
#define DA7212_MICBIAS_CTRL    0x62
#define DA7212_MIC_1_CTRL      0x63
#define DA7212_MIXIN_L_CTRL    0x65
#define DA7212_MIXIN_R_CTRL    0x66
#define DA7212_ADC_L_CTRL      0x67
#define DA7212_ADC_R_CTRL      0x68
#define DA7212_LDO_CTRL        0x90
#define DA7212_GAIN_RAMP_CTRL  0x92
#define DA7212_SYSTEM_ACTIVE   0xfd

#define SAMPLE_RATE_HZ 16000U
/* Zephyr's generic API cannot select a fixed MCK ratio, so apply the authoritative nrfx setup. */
#define BLOCK_FRAMES   256U
#define BLOCK_SIZE     (BLOCK_FRAMES * 2U * sizeof(int16_t))
#define BLOCK_COUNT    12U
#define DEFAULT_BLOCKS 16U
#define I2S_TIMEOUT_MS 1000
#define WARMUP_BLOCKS 8U

static const struct i2c_dt_spec codec = I2C_DT_SPEC_GET(DT_NODELABEL(da7212));
static const struct device *const i2s = DEVICE_DT_GET(DT_NODELABEL(i2s0));
K_MEM_SLAB_DEFINE_STATIC(mic_slab, BLOCK_SIZE, BLOCK_COUNT, 4);
static bool initialized;

static void apply_authoritative_i2s_clock(void)
{
	const nrf_i2s_config_t config = {
		.mode = NRF_I2S_MODE_MASTER,
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

static int codec_start_capture(void)
{
	static const uint8_t setup[][2] = {
		{ DA7212_CIF_CTRL, 0x80 }, { DA7212_SYSTEM_ACTIVE, 0x01 },
		{ DA7212_REFERENCES, 0x08 }, { DA7212_LDO_CTRL, 0x80 },
		{ DA7212_PLL_FRAC_TOP, 0x04 }, { DA7212_PLL_FRAC_BOT, 0xdd },
		{ DA7212_PLL_INTEGER, 0x31 }, { DA7212_PLL_CTRL, 0xc0 },
		{ 0xf0, 0x8b }, { 0xf2, 0x03 }, { 0xf0, 0x00 },
		{ DA7212_GAIN_RAMP_CTRL, 0x00 }, { DA7212_SR, 0x05 },
		{ DA7212_DIG_ROUTING_DAI, 0x10 }, { DA7212_DAI_CLK_MODE, 0x00 },
		{ DA7212_DAI_CTRL, 0xc0 }, { DA7212_MICBIAS_CTRL, 0x0a },
		{ DA7212_MIC_1_GAIN, 0x06 }, { DA7212_MIC_1_CTRL, 0xc4 },
		{ DA7212_MIXIN_L_SELECT, 0x02 }, { DA7212_MIXIN_R_SELECT, 0x04 },
		{ DA7212_MIXIN_L_CTRL, 0xa8 }, { DA7212_MIXIN_R_CTRL, 0xa8 },
		{ DA7212_ADC_L_CTRL, 0xf0 }, { DA7212_ADC_R_CTRL, 0xf0 },
		{ DA7212_ALC_CTRL3, 0x10 },
	};
	int ret;

	for (size_t i = 0; i < ARRAY_SIZE(setup); i++) {
		ret = codec_write(setup[i][0], setup[i][1]);
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
	printk("DA7212 mic PLL status: 0x%02x\n", pll_status);
	if (ret < 0 || (pll_status & 0x01U) == 0U) {
		return ret < 0 ? ret : -EIO;
	}

	for (unsigned int i = 0; i < 10U; i++) {
		uint8_t alc;
		k_msleep(2);
		if (i2c_reg_read_byte_dt(&codec, DA7212_ALC_CTRL3, &alc) == 0 && alc == 0U) {
			break;
		}
	}
	ret = codec_write(DA7212_MIC_1_CTRL, 0x84);
	ret |= codec_write(DA7212_ADC_L_CTRL, 0xa0);
	ret |= codec_write(DA7212_ADC_R_CTRL, 0xa0);
	k_msleep(20);
	return ret;
}

static void codec_stop_capture(void)
{
	(void)codec_write(DA7212_DAI_CTRL, 0x00);
	(void)codec_write(DA7212_MIC_1_CTRL, 0x00);
	(void)codec_write(DA7212_MICBIAS_CTRL, 0x00);
	(void)codec_write(DA7212_ADC_L_CTRL, 0x00);
	(void)codec_write(DA7212_ADC_R_CTRL, 0x00);
	(void)codec_write(DA7212_MIXIN_L_CTRL, 0x00);
	(void)codec_write(DA7212_MIXIN_R_CTRL, 0x00);
	(void)codec_write(DA7212_SYSTEM_ACTIVE, 0x00);
}

static int cmd_mic_capture(const struct shell *sh, size_t argc, char **argv)
{
	unsigned long blocks = argc > 1 ? strtoul(argv[1], NULL, 0) : DEFAULT_BLOCKS;
	struct i2s_config config = {
		.word_size = 16,
		.channels = 2,
		.format = I2S_FMT_DATA_FORMAT_I2S,
		.options = I2S_OPT_BIT_CLK_MASTER | I2S_OPT_FRAME_CLK_MASTER,
		.frame_clk_freq = SAMPLE_RATE_HZ,
		.mem_slab = &mic_slab,
		.block_size = BLOCK_SIZE,
		.timeout = I2S_TIMEOUT_MS,
	};
	int16_t minimum = INT16_MAX;
	int16_t maximum = INT16_MIN;
	int64_t sum = 0;
	uint32_t samples = 0;
	uint64_t absolute_sum = 0U;
	uint32_t clipped = 0U;
	int ret;

	if (!initialized) {
		return -EPERM;
	}
	if (blocks == 0U || blocks > 125U) {
		shell_error(sh, "Capture blocks must be 1-125 (16 ms each)");
		return -EINVAL;
	}

	ret = i2s_configure(i2s, I2S_DIR_RX, &config);
	if (ret < 0) {
		return ret;
	}
	apply_authoritative_i2s_clock();
	ret = i2s_trigger(i2s, I2S_DIR_RX, I2S_TRIGGER_START);
	if (ret < 0) {
		shell_error(sh, "I2S start failed (%d)", ret);
		goto out;
	}
	k_msleep(5);
	ret = codec_start_capture();
	if (ret < 0) {
		shell_error(sh, "DA7212 microphone setup failed (%d)", ret);
		goto stop;
	}

	for (unsigned int block = 0; block < WARMUP_BLOCKS; block++) {
		void *buffer;
		size_t size;

		ret = i2s_read(i2s, &buffer, &size);
		if (ret < 0) {
			shell_error(sh, "I2S warmup block %u failed (%d)", block, ret);
			goto stop;
		}
		k_mem_slab_free(&mic_slab, buffer);
	}

	for (unsigned long block = 0; block < blocks; block++) {
		void *buffer;
		size_t size;

		ret = i2s_read(i2s, &buffer, &size);
		if (ret < 0) {
			shell_error(sh, "I2S capture block %lu failed (%d)", block, ret);
			goto stop;
		}
		int16_t *pcm = buffer;
		/* DA7212 routes MIC1 to the left slot; the right slot is not valid microphone data. */
		for (size_t i = 0; i < size / sizeof(*pcm); i += 2U) {
			int32_t sample = pcm[i];

			minimum = MIN(minimum, sample);
			maximum = MAX(maximum, sample);
			sum += sample;
			absolute_sum += sample < 0 ? (uint32_t)-sample : (uint32_t)sample;
			clipped += sample == INT16_MIN || sample == INT16_MAX;
			samples++;
		}
		k_mem_slab_free(&mic_slab, buffer);
	}

stop:
	(void)i2s_trigger(i2s, I2S_DIR_RX, I2S_TRIGGER_DROP);
out:
	codec_stop_capture();
	if (ret < 0) {
		shell_error(sh, "Microphone capture failed (%d)", ret);
		return ret;
	}

	int32_t dc = samples == 0U ? 0 : (int32_t)(sum / samples);
	uint32_t mean_abs = samples == 0U ? 0U : (uint32_t)(absolute_sum / samples);
	int32_t span = (int32_t)maximum - minimum;
	shell_print(sh, "MIC samples=%u min=%d max=%d dc=%d mean_abs=%u clipped=%u span=%d", samples,
		    minimum, maximum, dc, mean_abs, clipped, span);
	if (span < 64) {
		shell_error(sh, "Microphone signal is missing or implausibly quiet");
		return -ENODATA;
	}
	if (clipped > samples / 20U) {
		shell_error(sh, "Microphone signal is implausibly clipped (%u/%u samples)", clipped,
			    samples);
		return -ERANGE;
	}

	shell_print(sh, "Microphone capture passed");
	return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(
	sub_mic_cmds,
	SHELL_CMD_ARG(capture, NULL, "Capture DA7212 MIC1 [16-ms blocks]", cmd_mic_capture, 1, 1),
	SHELL_SUBCMD_SET_END);

SHELL_SUBCMD_ADD((hwv), mic, &sub_mic_cmds, "DA7212 I2S microphone", NULL, 0, 0);

int mic_i2s_init(void)
{
	if (!i2c_is_ready_dt(&codec) || !device_is_ready(i2s)) {
		return -ENODEV;
	}
	initialized = true;
	return 0;
}
