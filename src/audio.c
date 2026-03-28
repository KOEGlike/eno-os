#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/logging/log.h>
#include <zephyr/fs/fs.h>
#include <string.h>

#include "audio.h"

extern int initialize_dac(const struct i2c_dt_spec *dac);
extern int power_on_dac(const struct i2c_dt_spec *dac);

LOG_MODULE_REGISTER(AUDIO, LOG_LEVEL_DBG);

#define DAC_NODE DT_NODELABEL(tad5212)
static const struct i2c_dt_spec dac = I2C_DT_SPEC_GET(DAC_NODE);

#define I2S_NODE DT_NODELABEL(i2s0)
static const struct device *i2s_dev = DEVICE_DT_GET(I2S_NODE);

#define SONG_LIST_BUF_SIZE 2048

K_MEM_SLAB_DEFINE_IN_SECT_STATIC(mem_slab, __nocache, BLOCK_SIZE, BLOCK_COUNT, 4);
static uint8_t read_buf[BLOCK_SIZE];

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

int init_audio(void)
{
	int ret;

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

	return 0;
}

void stop_playback(struct app_state *state, bool close_file, bool drop_i2s)
{
	if (drop_i2s)
	{
		(void)i2s_trigger(i2s_dev, I2S_DIR_TX, I2S_TRIGGER_DROP);
		(void)i2s_trigger(i2s_dev, I2S_DIR_TX, I2S_TRIGGER_PREPARE);
	}
	state->i2s_started = false;

	if (close_file && state->file_open)
	{
		(void)sd_card_close(&state->file);
		state->file_open = false;
	}

	state->playback_state = PLAYBACK_STOPPED;
	state->playing_index = -1;
	state->data_total_bytes = 0;
	state->data_streamed_bytes = 0;
	state->progress_step = 0;
	state->last_progress_ui_ms = 0;
	state->ui_dirty = true;
	state->list_dirty = true;
}

int queue_one_block(struct app_state *state, bool *eof)
{
	size_t bytes_read = BLOCK_SIZE;
	int64_t now_ms;
	int ret;

	*eof = false;
	ret = sd_card_read((char *)read_buf, &bytes_read, &state->file);
	if (ret)
	{
		return ret;
	}

	if (bytes_read == 0)
	{
		*eof = true;
		return 0;
	}

	if (bytes_read < BLOCK_SIZE)
	{
		memset(read_buf + bytes_read, 0, BLOCK_SIZE - bytes_read);
	}

	ret = i2s_buf_write(i2s_dev, read_buf, BLOCK_SIZE);
	if (ret < 0)
	{
		return ret;
	}

	state->data_streamed_bytes += (uint32_t)bytes_read;
	now_ms = k_uptime_get();
	if (state->data_total_bytes > 0)
	{
		uint32_t percent = (state->data_streamed_bytes * 100U) / state->data_total_bytes;
		uint8_t step = (uint8_t)(percent / PROGRESS_UI_STEP_PCT);

		if (step > state->progress_step && (now_ms - state->last_progress_ui_ms) >= PROGRESS_UI_UPDATE_MS)
		{
			state->progress_step = step;
			state->last_progress_ui_ms = now_ms;
			state->ui_dirty = true;
		}
	}
	return 0;
}

int prefill_and_start(struct app_state *state)
{
	int ret;
	int queued = 0;

	for (int i = 0; i < INITIAL_BLOCKS; i++)
	{
		bool eof = false;
		ret = queue_one_block(state, &eof);
		if (ret == -ENOMEM || ret == -EAGAIN || ret == -EBUSY || ret == -ENOMSG)
		{
			break;
		}
		if (ret)
		{
			return ret;
		}
		if (eof)
		{
			break;
		}
		queued++;
	}

	if (queued == 0)
	{
		return -ENODATA;
	}

	ret = i2s_trigger(i2s_dev, I2S_DIR_TX, I2S_TRIGGER_START);
	if (ret < 0)
	{
		return ret;
	}

	state->i2s_started = true;
	return 0;
}

int start_selected_song(struct app_state *state)
{
	uint8_t wav_header[WAV_HEADER_SIZE];
	size_t header_len = sizeof(wav_header);
	off_t file_size;
	int ret;

	if (state->song_count == 0)
	{
		return -ENOENT;
	}

	stop_playback(state, true, true);

	ret = sd_card_open(state->songs[state->selected_index], &state->file);
	if (ret)
	{
		return ret;
	}
	state->file_open = true;

	ret = fs_seek(&state->file, 0, FS_SEEK_END);
	if (ret)
	{
		stop_playback(state, true, true);
		return ret;
	}

	file_size = fs_tell(&state->file);
	if (file_size <= WAV_HEADER_SIZE)
	{
		stop_playback(state, true, true);
		return -EINVAL;
	}

	ret = fs_seek(&state->file, 0, FS_SEEK_SET);
	if (ret)
	{
		stop_playback(state, true, true);
		return ret;
	}

	ret = sd_card_read((char *)wav_header, &header_len, &state->file);
	if (ret || header_len < WAV_HEADER_SIZE)
	{
		stop_playback(state, true, true);
		return ret ? ret : -EINVAL;
	}

	if (memcmp(wav_header, "RIFF", 4) != 0 || memcmp(&wav_header[8], "WAVE", 4) != 0)
	{
		LOG_ERR("%s is not a valid WAV file", state->songs[state->selected_index]);
		stop_playback(state, true, true);
		return -EINVAL;
	}

	state->playing_index = state->selected_index;
	state->data_total_bytes = (uint32_t)(file_size - WAV_HEADER_SIZE);
	state->data_streamed_bytes = 0;
	state->progress_step = 0;
	state->last_progress_ui_ms = 0;
	state->playback_state = PLAYBACK_PLAYING;
	state->ui_dirty = true;
	state->list_dirty = true;

	ret = prefill_and_start(state);
	if (ret)
	{
		stop_playback(state, true, true);
		return ret;
	}

	return 0;
}

int pause_song(struct app_state *state)
{
	int ret;

	if (state->playback_state != PLAYBACK_PLAYING || !state->file_open)
	{
		return 0;
	}

	ret = i2s_trigger(i2s_dev, I2S_DIR_TX, I2S_TRIGGER_DROP);
	if (ret < 0)
	{
		return ret;
	}
	(void)i2s_trigger(i2s_dev, I2S_DIR_TX, I2S_TRIGGER_PREPARE);

	state->i2s_started = false;
	state->playback_state = PLAYBACK_PAUSED;
	state->ui_dirty = true;
	return 0;
}

int resume_song(struct app_state *state)
{
	int ret;

	if (state->playback_state != PLAYBACK_PAUSED || !state->file_open)
	{
		return 0;
	}

	ret = prefill_and_start(state);
	if (ret)
	{
		stop_playback(state, true, true);
		return ret;
	}

	state->playback_state = PLAYBACK_PLAYING;
	state->last_progress_ui_ms = 0;
	state->ui_dirty = true;
	return 0;
}

void process_playback(struct app_state *state)
{
	bool eof = false;
	int ret;

	if (state->playback_state != PLAYBACK_PLAYING || !state->file_open)
	{
		return;
	}

	ret = queue_one_block(state, &eof);
	if (ret == -ENOMEM || ret == -EAGAIN || ret == -EBUSY || ret == -ENOMSG)
	{
		return;
	}
	if (ret)
	{
		LOG_ERR("Playback error: %d", ret);
		stop_playback(state, true, true);
		return;
	}

	if (eof)
	{
		stop_playback(state, true, true);
		return;
	}
}
