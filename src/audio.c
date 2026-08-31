#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/logging/log.h>
#include <zephyr/audio/codec.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/fs/fs.h>
#include <string.h>

#include "audio.h"
#include "decoder.h"

LOG_MODULE_REGISTER(AUDIO, LOG_LEVEL_DBG);

#define CODEC_NODE DT_NODELABEL(tad5212)
static const struct device *codec_dev = DEVICE_DT_GET(CODEC_NODE);

/* Matches the listening level the retired PurePath register dump used */
#define DAC_DEFAULT_VOLUME_DB (-8)

/* The DAC supports -100..+27 dB, but boosting above 0 dB scales full
 * scale samples up before the fixed analog gain and clips
 */
#define DAC_VOLUME_MIN_DB (-100)
#define DAC_VOLUME_MAX_DB 0

static int volume_db = DAC_DEFAULT_VOLUME_DB;

#define I2S_NODE DT_NODELABEL(i2s0)
static const struct device *i2s_dev = DEVICE_DT_GET(I2S_NODE);

K_MEM_SLAB_DEFINE_IN_SECT_STATIC(mem_slab, __nocache, BLOCK_SIZE, BLOCK_COUNT, 4);

static struct audio_decoder decoder;
static uint8_t audio_scratch[BLOCK_SIZE];
static uint32_t song_block_bytes = BLOCK_SIZE;

/* Playback pump: decoding runs in its own thread so e-ink UI
 * refreshes (which block the main thread for seconds) can't starve
 * the I2S queue.
 */
static struct app_state *audio_state;
static atomic_t pump_active;
static K_SEM_DEFINE(audio_start_sem, 0, 1);
/* consecutive unplayable-file auto-skips */
static int prefill_failures;
/* consecutive playback-error auto-skips (underruns etc.) */
static int error_skip_count;

/* Keep the driver queue this deep at most: pause (which purges the
 * queue and resumes from the decoder position) then only skips about
 * this much audio. Blocks carry ~73 ms at 44.1 kHz (200 ms at 16 kHz),
 * so 8 blocks is ~0.58 s of ride-out at the highest rate.
 */
#define QUEUE_SOFT_LIMIT 8

static void audio_stop_nolock(struct app_state *state, bool close_file, bool drop);
static void audio_thread_fn(void *a, void *b, void *c);

/* DRAIN only arms the stop; the queued tail keeps playing while the
 * driver transitions to READY asynchronously. Wait for the tail by
 * polling the slab: every played block is freed back, so once all
 * blocks are free the queue is fully drained.
 */
static void wait_tail_played(void)
{
	int waited_ms = 0;

	while (k_mem_slab_num_free_get(&mem_slab) < BLOCK_COUNT && waited_ms < 5000)
	{
		k_sleep(K_MSEC(20));
		waited_ms += 20;
	}
}

K_THREAD_DEFINE(audio_thread, 6144, audio_thread_fn, NULL, NULL, NULL,
		K_PRIO_PREEMPT(1), 0, 0);

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

/* The nrfx driver reports a spurious "buffers not supplied" error
 * shortly after a DROP (the deferred stop event lands once we are
 * already back in READY) which latches I2S_STATE_ERROR. DROP then
 * PREPARE gets back to READY and clears the pending event window.
 */
static void i2s_force_ready(void)
{
	(void)i2s_trigger(i2s_dev, I2S_DIR_TX, I2S_TRIGGER_DROP);
	k_sleep(K_MSEC(20));
	(void)i2s_trigger(i2s_dev, I2S_DIR_TX, I2S_TRIGGER_PREPARE);
}

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

	i2s_force_ready();

	i2s_cfg.frame_clk_freq = sample_rate;
	i2s_cfg.block_size = song_block_bytes;
	ret = i2s_configure(i2s_dev, I2S_DIR_TX, &i2s_cfg);
	if (ret < 0)
	{
		/* a stale driver event may have landed between force_ready
		 * and configure; force again and retry once
		 */
		i2s_force_ready();
		ret = i2s_configure(i2s_dev, I2S_DIR_TX, &i2s_cfg);
		if (ret < 0)
		{
			LOG_ERR("I2S reconfigure failed for %u Hz: %d", sample_rate, ret);
			return ret;
		}
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

static void audio_thread_fn(void *a, void *b, void *c)
{
	ARG_UNUSED(a);
	ARG_UNUSED(b);
	ARG_UNUSED(c);

	while (true)
	{
		k_sem_take(&audio_start_sem, K_FOREVER);

		/* claim pump_active before touching shared state: main's
		 * stop_playback waits on this flag, so it must already be
		 * set when the state checks below could make us exit
		 */
		atomic_set(&pump_active, 1);

		if (audio_state == NULL ||
			audio_state->playback_state != PLAYBACK_PLAYING ||
			!audio_state->file_open)
		{
			atomic_set(&pump_active, 0);
			continue;
		}

		int ret = prefill_and_start(audio_state);
		if (ret == -ECANCELED)
		{
			/* paused/stopped while prefilling: exit quietly,
			 * the state owner already handled the stream
			 */
			atomic_set(&pump_active, 0);
			continue;
		}
		if (ret)
		{
			LOG_ERR("Prefill failed: %d", ret);
			audio_stop_nolock(audio_state, true, true);
			/* auto-advance past unplayable tracks (bounded so
			 * an all-bad folder does not loop hot)
			 */
			if (++prefill_failures < 8)
			{
				atomic_set(&audio_state->advance_request, 1);
			}
			atomic_set(&pump_active, 0);
			continue;
		}
		prefill_failures = 0;
		error_skip_count = 0;

		bool eof = false;
		while (audio_state->playback_state == PLAYBACK_PLAYING &&
			   audio_state->file_open)
		{
			if (BLOCK_COUNT - k_mem_slab_num_free_get(&mem_slab) >= QUEUE_SOFT_LIMIT)
			{
				/* enough audio queued: pace here instead of
				 * filling the queue to the brim (bounds the
				 * pause/resume skip)
				 */
				k_sleep(K_MSEC(10));
				continue;
			}

			ret = queue_one_block(audio_state, &eof);
			if (ret == -ENOMEM || ret == -EAGAIN || ret == -EBUSY || ret == -ENOMSG)
			{
				/* queue full: ahead of playback */
				k_sleep(K_MSEC(10));
				continue;
			}
			if (ret)
			{
 				if (audio_state->playback_state == PLAYBACK_PLAYING)
 				{
 					LOG_ERR("Playback error: %d", ret);
 					/* The I2S driver is in the ERROR state
 					 * and has already stopped/un-initialized
 					 * its nrfx instance; PREPARE purges the
 					 * queues and returns to READY without
 					 * touching it (a DROP here would assert
 					 * on the uninitialized nrfx state).
 					 * Teardown first, then drop pump_active:
 					 * main's stop_playback waits for
 					 * pump_active, so clearing it before the
 					 * teardown would let main race the
 					 * decoder/i2s cleanup.
 					 */
 					(void)i2s_trigger(i2s_dev, I2S_DIR_TX, I2S_TRIGGER_PREPARE);
 					audio_stop_nolock(audio_state, true, false);
 					/* skip to the next track (bounded) so a
 					 * transient SD stall does not halt the
 					 * player
 					 */
 					if (++error_skip_count < 8)
 					{
 						atomic_set(&audio_state->advance_request, 1);
 					}
 					atomic_set(&pump_active, 0);
 				}
				else
				{
					LOG_INF("pump write err after stop: %d", ret);
				}
 				break;
 			}
  			if (eof)
  			{
  				/* arm the stop, let the queued tail play out
  				 * (wait_tail_played), then tear down and
  				 * hand over to auto-advance. The advance
  				 * request is only made if the song was
  				 * still playing at that point (a pause or
  				 * stop during the tail is user intent and
  				 * must not trigger it)
  				*/
				LOG_INF("pump: eof, draining");
				(void)i2s_trigger(i2s_dev, I2S_DIR_TX, I2S_TRIGGER_DRAIN);
				wait_tail_played();
				if (audio_state->playback_state == PLAYBACK_PLAYING)
				{
					atomic_set(&audio_state->advance_request, 1);
				}
				audio_stop_nolock(audio_state, true, false);
				atomic_set(&pump_active, 0);
				break;
			}
 		}

		LOG_INF("pump exit: state %d eof %d", audio_state->playback_state, eof);
 		atomic_set(&pump_active, 0);
 	}
 }

void stop_playback(struct app_state *state, bool close_file, bool drop)
{
	/* signal the pump first, then wait for it to leave the decoder
	 * before tearing it down (bounded: a pump that never exits must
	 * not hang the main thread forever)
	 */
	state->playback_state = PLAYBACK_STOPPED;

	int waited_ms = 0;

	while (atomic_get(&pump_active) && waited_ms < 10000)
	{
		k_msleep(2);
		waited_ms += 2;
	}
	if (atomic_get(&pump_active))
	{
		LOG_ERR("pump did not exit; tearing down anyway");
	}

	audio_stop_nolock(state, close_file, drop);
}

static void audio_stop_nolock(struct app_state *state, bool close_file, bool drop)
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
	state->elapsed_s = 0;
	state->total_s = 0;
	state->last_progress_ui_ms = 0;
	state->ui_dirty = true;
}


int queue_one_block(struct app_state *state, bool *eof)
{
	size_t frames;
	size_t bytes;
	int64_t now_ms;
	int ret;

	*eof = false;

	/* SD refills stall for hundreds of ms; do them while the queue
	 * is still full so playback rides on buffered blocks
	 */
	decoder_prefetch(&decoder);

	frames = decoder_fill(&decoder, (int16_t *)audio_scratch, song_block_bytes / 4);

	if (frames == 0)
	{
		*eof = true;
		return 0;
	}

	bytes = frames * 4;
	if (bytes < song_block_bytes)
	{
		memset(audio_scratch + bytes, 0, song_block_bytes - bytes);
	}

	/* i2s_buf_write copies the scratch into its own slab block
	 * (blocking for pacing while the queue is full) and hands it
	 * to the driver
	 */
	ret = i2s_buf_write(i2s_dev, audio_scratch, song_block_bytes);
	if (ret < 0)
	{
		return ret;
	}

	state->elapsed_s = decoder.elapsed_ms / 1000;
	state->total_s = decoder.total_ms / 1000;
	now_ms = k_uptime_get();
	if ((now_ms - state->last_progress_ui_ms) >= PROGRESS_UI_UPDATE_MS)
	{
		state->last_progress_ui_ms = now_ms;
		state->ui_dirty = true;
	}
	return 0;
}

int prefill_and_start(struct app_state *state)
{
	int ret;
	int queued = 0;

	for (int i = 0; i < INITIAL_BLOCKS; i++)
	{
		/* a pause/stop during prefill must not start the stream */
		if (state->playback_state != PLAYBACK_PLAYING)
		{
			return -ECANCELED;
		}

		bool eof = false;
		ret = queue_one_block(state, &eof);
		if (ret == -ENOMEM || ret == -EAGAIN || ret == -EBUSY || ret == -ENOMSG)
		{
			LOG_INF("prefill blocked at %d: %d", queued, ret);
			break;
		}
		if (ret)
		{
			LOG_ERR("prefill error at %d: %d", queued, ret);
			return ret;
		}
		if (eof)
		{
			LOG_INF("prefill eof at %d", queued);
			break;
		}
		queued++;
	}

	if (state->playback_state != PLAYBACK_PLAYING)
	{
		return -ECANCELED;
	}

	if (queued == 0)
	{
		return -ENODATA;
	}

	LOG_INF("prefill done: %d blocks, starting", queued);
	ret = i2s_trigger(i2s_dev, I2S_DIR_TX, I2S_TRIGGER_START);
	if (ret < 0)
	{
		LOG_ERR("I2S start failed: %d", ret);
		return ret;
	}

	state->i2s_started = true;
	return 0;
}

int audio_volume_step(int step_db)
{
	int ret;
	audio_property_value_t val;
	int new_db = CLAMP(volume_db + step_db, DAC_VOLUME_MIN_DB, DAC_VOLUME_MAX_DB);

	if (new_db == volume_db)
	{
		return 0;
	}

	val.vol = new_db;
	ret = audio_codec_set_property(codec_dev, AUDIO_PROPERTY_OUTPUT_VOLUME, AUDIO_CHANNEL_ALL, val);
	if (ret)
	{
		LOG_ERR("Codec volume set failed: %d", ret);
		return ret;
	}

	ret = audio_codec_apply_properties(codec_dev);
	if (ret)
	{
		LOG_ERR("Codec volume apply failed: %d", ret);
		return ret;
	}

	volume_db = new_db;
	LOG_INF("Volume: %d dB", volume_db);
	return 0;
}

int start_song(struct app_state *state, const char *path)
{
	off_t file_size;
	int ret;

	if (path == NULL || path[0] == '\0')
	{
		return -ENOENT;
	}

	stop_playback(state, true, true);

	ret = sd_card_open(path, &state->file);
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
		LOG_ERR("Failed to open %s: %d", path, ret);
		stop_playback(state, true, true);
		return ret;
	}

	LOG_INF("%s: %u Hz", path, decoder.sample_rate);

	ret = configure_stream(decoder.sample_rate);
	if (ret)
	{
		stop_playback(state, true, true);
		return ret;
	}

	state->elapsed_s = 0;
	state->total_s = decoder.total_ms / 1000;
	state->last_progress_ui_ms = 0;
	state->playback_state = PLAYBACK_PLAYING;
	state->ui_dirty = true;

	/* hand playback over to the pump thread */
	audio_state = state;
	k_sem_give(&audio_start_sem);

	return 0;
}

int pause_song(struct app_state *state)
{
	int ret;

	if (state->playback_state != PLAYBACK_PLAYING || !state->file_open)
	{
		return 0;
	}

	/* go PAUSED before the DROP: the pump may be inside
	 * i2s_buf_write and must see the paused state when the write
	 * errors out, otherwise it tears the song down
	 */
	state->playback_state = PLAYBACK_PAUSED;

	ret = i2s_trigger(i2s_dev, I2S_DIR_TX, I2S_TRIGGER_DROP);
	if (ret < 0)
	{
		state->playback_state = PLAYBACK_PLAYING;
		return ret;
	}

	state->i2s_started = false;
	state->ui_dirty = true;
	return 0;
}

int resume_song(struct app_state *state)
{
	if (state->playback_state != PLAYBACK_PAUSED || !state->file_open)
	{
		return 0;
	}

	state->playback_state = PLAYBACK_PLAYING;
	state->last_progress_ui_ms = 0;
	state->ui_dirty = true;

	/* the decoder keeps its position; the pump continues from there */
	audio_state = state;
	k_sem_give(&audio_start_sem);
	return 0;
}

