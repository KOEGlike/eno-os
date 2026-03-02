#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/logging/log.h>
#include <zephyr/drivers/i2s.h>
#include <zephyr/drivers/i2c.h>
#include <string.h>

#include "sd_card.h"

LOG_MODULE_REGISTER(MAIN, LOG_LEVEL_DBG);

/* DAC on I2C */
#define DAC_NODE DT_NODELABEL(tad5212)
static const struct i2c_dt_spec dac = I2C_DT_SPEC_GET(DAC_NODE);

/* I2S */
#define I2S_NODE DT_NODELABEL(i2s0)
static const struct device *i2s_dev = DEVICE_DT_GET(I2S_NODE);

#define SAMPLE_FREQUENCY 16000
#define SAMPLE_BIT_WIDTH 16
#define BYTES_PER_SAMPLE 2
#define NUMBER_OF_CHANNELS 2
#define SAMPLES_PER_BLOCK ((SAMPLE_FREQUENCY / 10) * NUMBER_OF_CHANNELS)
#define INITIAL_BLOCKS 4
#define TIMEOUT 2000

#define BLOCK_SIZE (BYTES_PER_SAMPLE * SAMPLES_PER_BLOCK)
#define BLOCK_COUNT (INITIAL_BLOCKS + 4)

K_MEM_SLAB_DEFINE_IN_SECT_STATIC(mem_slab, __nocache, BLOCK_SIZE, BLOCK_COUNT, 4);

static struct i2s_config i2s_cfg = {
	.word_size = SAMPLE_BIT_WIDTH,
	.channels = NUMBER_OF_CHANNELS,
	.format = I2S_FMT_DATA_FORMAT_I2S,
	.options = I2S_OPT_BIT_CLK_MASTER | I2S_OPT_FRAME_CLK_MASTER,
	.frame_clk_freq = SAMPLE_FREQUENCY,
	.mem_slab = &mem_slab,
	.block_size = BLOCK_SIZE,
	.timeout = TIMEOUT,
};

#define AUDIO_FILENAME "audio.mp3"

static uint8_t read_buf[BLOCK_SIZE];

int main(void)
{
	int ret;
	struct fs_file_t audio_file;
	bool i2s_started;
	int blocks_queued;

	LOG_INF("eno-os: audio player starting");

	/* Initialize SD card */
	ret = sd_card_init();
	if (ret)
	{
		LOG_ERR("SD card init failed: %d", ret);
		return ret;
	}
	LOG_INF("SD card initialized");

	/* Initialize and power on DAC via I2C */
	ret = initialize_dac(&dac);
	if (ret)
	{
		LOG_ERR("DAC init failed: %d", ret);
		return ret;
	}
	ret = power_on_dac(&dac);
	if (ret)
	{
		LOG_ERR("DAC power on failed: %d", ret);
		return ret;
	}
	LOG_INF("DAC ready");

	/* Configure I2S TX */
	if (!device_is_ready(i2s_dev))
	{
		LOG_ERR("I2S device not ready");
		return -ENODEV;
	}
	ret = i2s_configure(i2s_dev, I2S_DIR_TX, &i2s_cfg);
	if (ret < 0)
	{
		LOG_ERR("I2S configure failed: %d", ret);
		return ret;
	}
	LOG_INF("I2S configured");

	/* Play audio.mp3 in a loop */
	while (1)
	{
		ret = sd_card_open(AUDIO_FILENAME, &audio_file);
		if (ret)
		{
			LOG_ERR("Failed to open %s: %d", AUDIO_FILENAME, ret);
			k_msleep(1000);
			continue;
		}
		LOG_INF("Playing %s", AUDIO_FILENAME);

		i2s_started = false;
		blocks_queued = 0;

		while (1)
		{
			size_t bytes_read = BLOCK_SIZE;

			ret = sd_card_read((char *)read_buf, &bytes_read,
							   &audio_file);
			if (ret)
			{
				LOG_ERR("SD read error: %d", ret);
				break;
			}
			if (bytes_read == 0)
			{
				break; /* EOF */
			}

			/* Zero-pad a partial final block */
			if (bytes_read < BLOCK_SIZE)
			{
				memset(read_buf + bytes_read, 0,
					   BLOCK_SIZE - bytes_read);
			}

			ret = i2s_buf_write(i2s_dev, read_buf, BLOCK_SIZE);
			if (ret < 0)
			{
				LOG_ERR("I2S write failed: %d", ret);
				i2s_trigger(i2s_dev, I2S_DIR_TX,
							I2S_TRIGGER_DROP);
				i2s_started = false;
				break;
			}
			blocks_queued++;

			/* Start I2S after pre-filling the queue */
			if (!i2s_started && blocks_queued >= INITIAL_BLOCKS)
			{
				ret = i2s_trigger(i2s_dev, I2S_DIR_TX,
								  I2S_TRIGGER_START);
				if (ret < 0)
				{
					LOG_ERR("I2S start failed: %d", ret);
					break;
				}
				i2s_started = true;
			}
		}

		/* Handle short files that never hit INITIAL_BLOCKS */
		if (!i2s_started && blocks_queued > 0)
		{
			i2s_trigger(i2s_dev, I2S_DIR_TX, I2S_TRIGGER_START);
			i2s_started = true;
		}

		/* Drain remaining queued audio, then back to READY state */
		if (i2s_started)
		{
			i2s_trigger(i2s_dev, I2S_DIR_TX, I2S_TRIGGER_DRAIN);
		}

		sd_card_close(&audio_file);
		LOG_INF("Looping %s", AUDIO_FILENAME);
	}

	return 0;
}