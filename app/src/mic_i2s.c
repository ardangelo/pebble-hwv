#include "mic_i2s.h"
#include "cyberdeck_i2s_pins.h"

#include <stdlib.h>
#include <string.h>

#include <nrfx_i2s.h>
#include <zephyr/device.h>
#include <zephyr/drivers/clock_control/nrf_clock_control.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/kernel.h>
#include <zephyr/shell/shell.h>
#include <zephyr/sys/onoff.h>

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
#define DA7212_ALC_CTRL1       0x2b
#define DA7212_MIXIN_L_SELECT  0x32
#define DA7212_MIXIN_R_SELECT  0x33
#define DA7212_MIC_1_GAIN      0x39
#define DA7212_SYSTEM_MODES_INPUT 0x50
#define DA7212_MICBIAS_CTRL    0x62
#define DA7212_MIC_1_CTRL      0x63
#define DA7212_MIC_2_CTRL      0x64
#define DA7212_MIXIN_L_CTRL    0x65
#define DA7212_MIXIN_R_CTRL    0x66
#define DA7212_ADC_L_CTRL      0x67
#define DA7212_ADC_R_CTRL      0x68
#define DA7212_LDO_CTRL        0x90
#define DA7212_GAIN_RAMP_CTRL  0x92
#define DA7212_SYSTEM_ACTIVE   0xfd

#define MIC_CTRL_POSITIVE_INPUT_MUTED 0xc4
#define MIC_CTRL_POSITIVE_INPUT       0x84
#define MIC_GAIN_30_DB                 0x06
#define PLL_STATUS_REQUIRED           0x07
#define ALC_AUTO_CALIB_EN              BIT(4)
#define ALC_CALIB_OVERFLOW             BIT(5)

#define BLOCK_FRAMES   256U
#define BLOCK_WORDS    BLOCK_FRAMES
#define BUFFER_COUNT   2U
#define DEFAULT_BLOCKS 16U
#define WARMUP_BLOCKS  8U
#define STOP_TIMEOUT_MS 100
#define HFXO_START_TIMEOUT_MS 100
#define CAPTURE_TIMEOUT_BASE_MS 1000U
#define CAPTURE_BLOCK_TIMEOUT_MS 20U
#define MIC_MIN_RMS 40U
#define MIC_MAX_ABS_DC 8000
#define MIC_CLIP_LEVEL 32000
#define MIC_MAX_CLIPPED_PER_MILLE 5U

static const struct i2c_dt_spec codec = I2C_DT_SPEC_GET(DT_NODELABEL(da7212));
static const struct device *const i2s_dev = DEVICE_DT_GET(DT_NODELABEL(i2s0));
static const nrfx_i2s_t i2s_instance = NRFX_I2S_INSTANCE(0);
static uint32_t rx_buffers[BUFFER_COUNT][BLOCK_WORDS];
static uint32_t tx_silence_buffers[BUFFER_COUNT][BLOCK_WORDS];
static struct onoff_manager *hfclk_manager;
static struct onoff_client hfclk_client;
static bool hfclk_requested;
static bool i2s_initialized;
static bool i2s_started;
static volatile bool capture_running;
static uint8_t next_buffer;
static bool initialized;
K_SEM_DEFINE(capture_complete, 0, 1);
K_SEM_DEFINE(transfer_stopped, 0, 1);

static struct {
	unsigned long target_blocks;
	unsigned long captured_blocks;
	unsigned int warmup_blocks;
	int16_t minimum;
	int16_t maximum;
	int32_t dc_estimate;
	int32_t ac_minimum;
	int32_t ac_maximum;
	int64_t sum;
	uint64_t absolute_sum;
	uint64_t ac_absolute_sum;
	uint64_t sum_squares;
	uint32_t samples;
	uint32_t clipped;
	uint32_t boundary_clipped;
} capture;

static int codec_write(uint8_t reg, uint8_t value)
{
	return i2c_reg_write_byte_dt(&codec, reg, value);
}

static uint32_t isqrt64(uint64_t value)
{
	uint64_t root = 0U;
	uint64_t bit = 1ULL << 62;

	while (bit > value) {
		bit >>= 2;
	}
	while (bit != 0U) {
		if (value >= root + bit) {
			value -= root + bit;
			root = (root >> 1) + bit;
		} else {
			root >>= 1;
		}
		bit >>= 2;
	}

	return (uint32_t)root;
}

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

static void capture_block(const uint32_t *buffer)
{
	const int16_t *pcm = (const int16_t *)buffer;
	bool collect = capture.warmup_blocks == 0U;

	if (capture.captured_blocks >= capture.target_blocks) {
		return;
	}

	for (size_t i = 0; i < BLOCK_FRAMES * 2U; i += 2U) {
		int32_t sample = pcm[i + 1U];
		int32_t dc = capture.dc_estimate >> 8;
		int32_t ac = sample - dc;
		int16_t ac_sample = CLAMP(ac, INT16_MIN, INT16_MAX);

		capture.dc_estimate += ac;
		if (!collect) {
			continue;
		}
		capture.minimum = MIN(capture.minimum, sample);
		capture.maximum = MAX(capture.maximum, sample);
		capture.ac_minimum = MIN(capture.ac_minimum, ac_sample);
		capture.ac_maximum = MAX(capture.ac_maximum, ac_sample);
		capture.sum += sample;
		capture.absolute_sum += sample < 0 ? (uint32_t)-sample : (uint32_t)sample;
		capture.ac_absolute_sum +=
			ac_sample < 0 ? (uint32_t)-ac_sample : (uint32_t)ac_sample;
		capture.sum_squares += (uint64_t)((int32_t)ac_sample * ac_sample);
		bool clipped = sample <= -MIC_CLIP_LEVEL || sample >= MIC_CLIP_LEVEL;
		capture.clipped += clipped;
		capture.boundary_clipped += clipped && (i < 4U || i >= (BLOCK_FRAMES - 2U) * 2U);
		capture.samples++;
	}

	if (!collect) {
		capture.warmup_blocks--;
		return;
	}
	capture.captured_blocks++;
	if (capture.captured_blocks == capture.target_blocks) {
		k_sem_give(&capture_complete);
	}
}

static void i2s_data_handler(const nrfx_i2s_buffers_t *released, uint32_t status)
{
	if ((status & NRFX_I2S_STATUS_NEXT_BUFFERS_NEEDED) != 0U && capture_running) {
		nrfx_i2s_buffers_t next = {
			.p_tx_buffer = tx_silence_buffers[next_buffer],
			.p_rx_buffer = rx_buffers[next_buffer],
			.buffer_size = BLOCK_WORDS,
		};

		(void)nrfx_i2s_next_buffers_set(&i2s_instance, &next);
		next_buffer = (next_buffer + 1U) % BUFFER_COUNT;
	}

	if (released != NULL && released->p_rx_buffer != NULL && capture_running) {
		capture_block(released->p_rx_buffer);
	}

	if ((status & NRFX_I2S_STATUS_TRANSFER_STOPPED) != 0U) {
		k_sem_give(&transfer_stopped);
	}
}

static int direct_i2s_start(void)
{
	nrfx_i2s_config_t config = NRFX_I2S_DEFAULT_CONFIG(
		CYBERDECK_I2S_SCK_PIN, CYBERDECK_I2S_LRCK_PIN, CYBERDECK_I2S_MCK_PIN,
		CYBERDECK_I2S_SDOUT_PIN, CYBERDECK_I2S_SDIN_PIN);
	nrfx_i2s_buffers_t initial = {
		.p_tx_buffer = tx_silence_buffers[0],
		.p_rx_buffer = rx_buffers[0],
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

	err = nrfx_i2s_init(&i2s_instance, &config, i2s_data_handler);
	if (err != NRFX_SUCCESS) {
		(void)hfclk_release();
		return err == NRFX_ERROR_ALREADY ? -EBUSY : -EIO;
	}
	i2s_initialized = true;
	next_buffer = 1U;
	capture_running = true;

	err = nrfx_i2s_start(&i2s_instance, &initial, 0);
	if (err != NRFX_SUCCESS) {
		capture_running = false;
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

	capture_running = false;
	if (i2s_started) {
		nrfx_i2s_stop(&i2s_instance);
		ret = k_sem_take(&transfer_stopped, K_MSEC(STOP_TIMEOUT_MS));
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
		{ DA7212_MIC_1_GAIN, MIC_GAIN_30_DB },
		{ DA7212_MIC_1_CTRL, MIC_CTRL_POSITIVE_INPUT_MUTED },
		{ DA7212_MIXIN_L_SELECT, 0x02 }, { DA7212_MIXIN_R_SELECT, 0x04 },
		{ DA7212_MIXIN_L_CTRL, 0xa8 }, { DA7212_MIXIN_R_CTRL, 0xa8 },
		{ DA7212_ADC_L_CTRL, 0xf0 }, { DA7212_ADC_R_CTRL, 0xf0 },
		{ DA7212_DAI_CLK_MODE, 0x81 },
		{ DA7212_ALC_CTRL1, 0x10 },
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
	if (ret < 0) {
		return ret;
	}
	printk("DA7212 mic PLL status: 0x%02x\n", pll_status);
	if ((pll_status & PLL_STATUS_REQUIRED) != PLL_STATUS_REQUIRED) {
		return -EIO;
	}

	bool calibration_complete = false;
	uint8_t alc_status = ALC_AUTO_CALIB_EN;

	for (unsigned int i = 0; i < 50U; i++) {
		k_msleep(2);
		ret = i2c_reg_read_byte_dt(&codec, DA7212_ALC_CTRL1, &alc_status);
		if (ret < 0) {
			return ret;
		}
		if ((alc_status & ALC_AUTO_CALIB_EN) == 0U) {
			calibration_complete = true;
			break;
		}
	}
	if (!calibration_complete) {
		return -ETIMEDOUT;
	}
	printk("DA7212 mic ALC status: 0x%02x%s\n", alc_status,
	       (alc_status & ALC_CALIB_OVERFLOW) != 0U ? " (overflow)" : "");
	ret = codec_write(DA7212_MIC_1_CTRL, MIC_CTRL_POSITIVE_INPUT);
	if (ret == 0) {
		ret = codec_write(DA7212_ADC_L_CTRL, 0xa0);
	}
	if (ret == 0) {
		ret = codec_write(DA7212_ADC_R_CTRL, 0xa0);
	}
	k_msleep(20);
	static const uint8_t diag_registers[] = {
		DA7212_SYSTEM_MODES_INPUT, DA7212_MICBIAS_CTRL, DA7212_MIC_1_GAIN,
		DA7212_MIC_1_CTRL, DA7212_MIXIN_L_SELECT, DA7212_MIXIN_R_SELECT,
		DA7212_MIXIN_L_CTRL, DA7212_MIXIN_R_CTRL, DA7212_ADC_L_CTRL,
		DA7212_ADC_R_CTRL,
	};
	uint8_t diag[ARRAY_SIZE(diag_registers)];

	for (size_t i = 0; i < ARRAY_SIZE(diag_registers); i++) {
		ret = i2c_reg_read_byte_dt(&codec, diag_registers[i], &diag[i]);
		if (ret < 0) {
			return ret;
		}
	}
	printk("DA7212 MIC1 regs modes=%02x bias=%02x gain=%02x mic=%02x "
	       "sel=%02x/%02x mix=%02x/%02x adc=%02x/%02x\n",
	       diag[0], diag[1], diag[2], diag[3], diag[4], diag[5],
	       diag[6], diag[7], diag[8], diag[9]);
	return ret;
}

static int codec_stop_capture(void)
{
	static const uint8_t shutdown[][2] = {
		{ DA7212_DAI_CLK_MODE, 0x00 }, { DA7212_DAI_CTRL, 0x00 },
		{ DA7212_MIC_1_CTRL, 0x00 }, { DA7212_MIC_2_CTRL, 0x00 },
		{ DA7212_MICBIAS_CTRL, 0x00 }, { DA7212_ADC_L_CTRL, 0x00 },
		{ DA7212_ADC_R_CTRL, 0x00 }, { DA7212_MIXIN_L_CTRL, 0x00 },
		{ DA7212_MIXIN_R_CTRL, 0x00 }, { DA7212_SYSTEM_MODES_INPUT, 0x00 },
		{ DA7212_PLL_CTRL, 0x00 }, { DA7212_SYSTEM_ACTIVE, 0x00 },
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

static int cmd_mic_capture(const struct shell *sh, size_t argc, char **argv)
{
	unsigned long blocks = argc > 1 ? strtoul(argv[1], NULL, 0) : DEFAULT_BLOCKS;
	uint32_t timeout_ms;
	int ret;
	int stop_ret;
	int codec_stop_ret;

	if (!initialized) {
		return -EPERM;
	}
	if (blocks == 0U || blocks > 125U) {
		shell_error(sh, "Capture blocks must be 1-125 (16 ms each)");
		return -EINVAL;
	}

	memset(&capture, 0, sizeof(capture));
	memset(tx_silence_buffers, 0, sizeof(tx_silence_buffers));
	capture.target_blocks = blocks;
	capture.ac_minimum = INT32_MAX;
	capture.ac_maximum = INT32_MIN;
	capture.warmup_blocks = WARMUP_BLOCKS;
	capture.minimum = INT16_MAX;
	capture.maximum = INT16_MIN;
	k_sem_reset(&capture_complete);
	k_sem_reset(&transfer_stopped);

	ret = direct_i2s_start();
	if (ret < 0) {
		shell_error(sh, "Fixed-clock nrfx I2S start failed (%d)", ret);
		return ret;
	}

	k_msleep(5);
	ret = codec_start_capture();
	if (ret < 0) {
		shell_error(sh, "DA7212 microphone setup failed (%d)", ret);
		goto stop;
	}

	timeout_ms = CAPTURE_TIMEOUT_BASE_MS +
		(uint32_t)(blocks + WARMUP_BLOCKS) * CAPTURE_BLOCK_TIMEOUT_MS;
	ret = k_sem_take(&capture_complete, K_MSEC(timeout_ms));
	if (ret < 0) {
		shell_error(sh, "Timed out waiting for nrfx I2S microphone data");
	}

stop:
	codec_stop_ret = codec_stop_capture();
	stop_ret = direct_i2s_stop();
	if (ret == 0 && codec_stop_ret < 0) {
		ret = codec_stop_ret;
	}
	if (ret == 0 && stop_ret < 0) {
		ret = stop_ret;
	}
	if (ret < 0) {
		shell_error(sh, "Microphone capture failed (%d)", ret);
		return ret;
	}

	int32_t dc = capture.samples == 0U ? 0 : (int32_t)(capture.sum / capture.samples);
	uint32_t mean_abs = capture.samples == 0U ? 0U :
		(uint32_t)(capture.absolute_sum / capture.samples);
	uint32_t ac_mean_abs = capture.samples == 0U ? 0U :
		(uint32_t)(capture.ac_absolute_sum / capture.samples);
	uint32_t rms = capture.samples == 0U ? 0U :
		isqrt64(capture.sum_squares / capture.samples);
	uint32_t negative_peak = capture.ac_minimum < 0 ? (uint32_t)-capture.ac_minimum : 0U;
	uint32_t positive_peak = capture.ac_maximum > 0 ? (uint32_t)capture.ac_maximum : 0U;
	uint32_t ac_peak = MAX(negative_peak, positive_peak);
	int32_t span = (int32_t)capture.maximum - capture.minimum;
	shell_print(sh,
		"MIC1 samples=%u min=%d max=%d dc=%d rms=%u mean_abs=%u ac_mean_abs=%u "
		"ac_peak=%u clipped=%u boundary_clipped=%u span=%d",
		capture.samples, capture.minimum, capture.maximum, dc, rms, mean_abs, ac_mean_abs,
		ac_peak, capture.clipped, capture.boundary_clipped, span);
	if (rms < MIC_MIN_RMS) {
		shell_error(sh, "Microphone signal is missing or implausibly quiet (RMS %u)", rms);
		return -ENODATA;
	}
	if (dc < -MIC_MAX_ABS_DC || dc > MIC_MAX_ABS_DC) {
		shell_error(sh, "Microphone DC offset is implausible (%d)", dc);
		return -ERANGE;
	}
	if ((uint64_t)capture.clipped * 1000ULL >
	    (uint64_t)capture.samples * MIC_MAX_CLIPPED_PER_MILLE) {
		shell_error(sh, "Microphone signal is implausibly clipped (%u/%u samples)",
			capture.clipped, capture.samples);
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
	if (!i2c_is_ready_dt(&codec) || !device_is_ready(i2s_dev)) {
		return -ENODEV;
	}
	initialized = true;
	return 0;
}