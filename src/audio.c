#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/logging/log.h>
#include <zephyr/audio/codec.h>
#include <zephyr/fs/fs.h>
#include <string.h>

#include "audio.h"
#include "decoder.h"

LOG_MODULE_REGISTER(AUDIO, LOG_LEVEL_DBG);

#define CODEC_NODE DT_NODELABEL(tad5212)
static const struct device *codec_dev = DEVICE_DT_GET(CODEC_NODE);

/* Matches the listening level the retired PurePath register dump used */
#define DAC_DEFAULT_VOLUME_DB (-8)

#define I2S_NODE DT_NODELABEL(i2s0)
static const struct device *i2s_dev = DEVICE_DT_GET(I2S_NODE);

K_MEM_SLAB_DEFINE_IN_SECT_STATIC(mem_slab, __nocache, BLOCK_SIZE, BLOCK_COUNT, 4);

static struct audio_decoder decoder;
static uint32_t song_block_bytes = BLOCK_SIZE;

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

static struct audio_codec_cfg codec_cfg = {
	.dai_type = AUDIO_DAI_TYPE_I2S,
	.dai_cfg.i2s = {
		.word_size = SAMPLE_BIT_WIDTH,
		.channels = NUMBER_OF_CHANNELS,
		.frame_clk_freq = SAMPLE_FREQUENCY,
	},
	.dai_route = AUDIO_ROUTE_PLAYBACK,
};

static int configure_stream(uint32_t sample_rate)
{
	uint32_t block_bytes;
	int ret;

	/* ~100 ms blocks at low rates, capped by the slab, 4-byte aligned */
	block_bytes = ((sample_rate / 10) * 4) & ~0x3U;
	if (block_bytes < 4) {
		block_bytes = 4;
	}
	song_block_bytes = MIN(block_bytes, BLOCK_SIZE);

	/* Power the DAC down while its rate code changes, per the TI
	 * recommended sequence, and bring it back up after.
	 */
	audio_codec_stop_output(codec_dev);

	i2s_cfg.frame_clk_freq = sample_rate;
	i2s_cfg.block_size = song_block_bytes;
	ret = i2s_configure(i2s_dev, I2S_DIR_TX, &i2s_cfg);
	if (ret < 0)
	{
		LOG_ERR("I2S reconfigure failed for %u Hz: %d", sample_rate, ret);
		return ret;
	}

	codec_cfg.dai_cfg.i2s.frame_clk_freq = sample_rate;
	ret = audio_codec_configure(codec_dev, &codec_cfg);
	if (ret)
	{
		LOG_ERR("Codec reconfigure failed: %d", ret);
		return ret;
	}

	audio_codec_start_output(codec_dev);

	return 0;
}

int init_audio(void)
{
	int ret;
	audio_property_value_t val;

	if (!device_is_ready(codec_dev))
	{
		LOG_ERR("Codec device not ready");
		return -ENODEV;
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

	ret = audio_codec_configure(codec_dev, &codec_cfg);
	if (ret)
	{
		LOG_ERR("Codec configure failed: %d", ret);
		return ret;
	}

	val.vol = DAC_DEFAULT_VOLUME_DB;
	ret = audio_codec_set_property(codec_dev, AUDIO_PROPERTY_OUTPUT_VOLUME, AUDIO_CHANNEL_ALL, val);
	if (ret)
	{
		LOG_ERR("Codec volume failed: %d", ret);
		return ret;
	}
	ret = audio_codec_apply_properties(codec_dev);
	if (ret)
	{
		LOG_ERR("Codec apply properties failed: %d", ret);
		return ret;
	}

	audio_codec_start_output(codec_dev);

	return 0;
}

void stop_playback(struct app_state *state, bool close_file, bool drop)
{
	if (drop)
	{
		(void)i2s_trigger(i2s_dev, I2S_DIR_TX, I2S_TRIGGER_DROP);
	}
	else
	{
		/* drain: let the already queued blocks finish playing */
		(void)i2s_trigger(i2s_dev, I2S_DIR_TX, I2S_TRIGGER_DRAIN);
	}
	state->i2s_started = false;

	decoder_close(&decoder);

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
	void *block;
	size_t frames;
	size_t bytes;
	int64_t now_ms;
	int ret;

	*eof = false;

	ret = k_mem_slab_alloc(&mem_slab, &block, K_NO_WAIT);
	if (ret)
	{
		/* queue full: we are ahead of playback, try again later */
		return ret;
	}

	frames = decoder_fill(&decoder, (int16_t *)block, song_block_bytes / 4);
	if (frames == 0)
	{
		k_mem_slab_free(&mem_slab, block);
		*eof = true;
		return 0;
	}

	bytes = frames * 4;
	if (bytes < song_block_bytes)
	{
		memset((uint8_t *)block + bytes, 0, song_block_bytes - bytes);
	}

	/* ownership of the block moves to the I2S driver */
	ret = i2s_buf_write(i2s_dev, block, song_block_bytes);
	if (ret < 0)
	{
		k_mem_slab_free(&mem_slab, block);
		return ret;
	}

	state->data_streamed_bytes = decoder.progress_num;
	now_ms = k_uptime_get();
	if (decoder.progress_den > 0)
	{
		uint32_t percent = (decoder.progress_num * 100U) / decoder.progress_den;
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
	}	if (queued == 0)
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
	if (file_size <= 0)
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

	ret = decoder_open(&decoder, &state->file, (uint32_t)file_size);
	if (ret)
	{
		LOG_ERR("Failed to open %s: %d", state->songs[state->selected_index], ret);
		stop_playback(state, true, true);
		return ret;
	}

	LOG_INF("%s: %u Hz", state->songs[state->selected_index], decoder.sample_rate);

	ret = configure_stream(decoder.sample_rate);
	if (ret)
	{
		stop_playback(state, true, true);
		return ret;
	}

	state->playing_index = state->selected_index;
	state->data_total_bytes = decoder.progress_den;
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

	/* keep the I2S queue topped up; each block costs one SD read
	 * plus decode time, so do a few per loop iteration
	 */
	for (int i = 0; i < 4; i++)
	{
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
			/* drain the queued blocks so the tail isn't cut off */
			stop_playback(state, true, false);
			return;
		}
	}
}
