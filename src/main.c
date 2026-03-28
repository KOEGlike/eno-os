#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/logging/log.h>
#include <zephyr/drivers/display.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/drivers/i2s.h>
#include <zephyr/fs/fs.h>
#include <zephyr/sys/atomic.h>
#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include <lvgl.h>
#include <lvgl_zephyr.h>

#include "sd_card.h"

/* DAC functions from lib/i2c_dac.c */
extern int initialize_dac(const struct i2c_dt_spec *dac);
extern int power_on_dac(const struct i2c_dt_spec *dac);

LOG_MODULE_REGISTER(MAIN, LOG_LEVEL_DBG);

#define DAC_NODE DT_NODELABEL(tad5212)
static const struct i2c_dt_spec dac = I2C_DT_SPEC_GET(DAC_NODE);

#define I2S_NODE DT_NODELABEL(i2s0)
static const struct device *i2s_dev = DEVICE_DT_GET(I2S_NODE);

#define DISPLAY_NODE DT_CHOSEN(zephyr_display)
static const struct device *display = DEVICE_DT_GET(DISPLAY_NODE);

#define BUTTON_POWER_NODE DT_NODELABEL(button_power)
#define BUTTON_LEFT_NODE DT_NODELABEL(button_side_left)
#define BUTTON_RIGHT_NODE DT_NODELABEL(button_side_right)
static const struct gpio_dt_spec button_power = GPIO_DT_SPEC_GET(BUTTON_POWER_NODE, gpios);
static const struct gpio_dt_spec button_left = GPIO_DT_SPEC_GET(BUTTON_LEFT_NODE, gpios);
static const struct gpio_dt_spec button_right = GPIO_DT_SPEC_GET(BUTTON_RIGHT_NODE, gpios);

#define SAMPLE_FREQUENCY 16000
#define SAMPLE_BIT_WIDTH 16
#define BYTES_PER_SAMPLE 2
#define NUMBER_OF_CHANNELS 2
#define SAMPLES_PER_BLOCK ((SAMPLE_FREQUENCY / 10) * NUMBER_OF_CHANNELS)
#define INITIAL_BLOCKS 8
#define TIMEOUT 2000

#define BLOCK_SIZE (BYTES_PER_SAMPLE * SAMPLES_PER_BLOCK)
#define BLOCK_COUNT (INITIAL_BLOCKS + 6)

#define MAX_SONGS 32
#define MAX_SONG_NAME_LEN 96
#define SONG_LIST_BUF_SIZE 2048
#define SONG_LIST_TEXT_BUF_SIZE 4096
#define WAV_HEADER_SIZE 44
#define PROGRESS_UI_UPDATE_MS 10000
#define PROGRESS_UI_STEP_PCT 10

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

enum playback_state
{
	PLAYBACK_STOPPED = 0,
	PLAYBACK_PLAYING,
	PLAYBACK_PAUSED,
};

struct app_state
{
	char songs[MAX_SONGS][MAX_SONG_NAME_LEN];
	int song_count;
	int selected_index;
	int playing_index;
	enum playback_state playback_state;
	struct fs_file_t file;
	bool file_open;
	bool i2s_started;
	uint32_t data_total_bytes;
	uint32_t data_streamed_bytes;
	uint8_t progress_step;
	int64_t last_progress_ui_ms;
	bool ui_dirty;
	bool list_dirty;
};

static struct app_state app = {
	.song_count = 0,
	.selected_index = 0,
	.playing_index = -1,
	.playback_state = PLAYBACK_STOPPED,
	.file_open = false,
	.i2s_started = false,
	.data_total_bytes = 0,
	.data_streamed_bytes = 0,
	.progress_step = 0,
	.last_progress_ui_ms = 0,
	.ui_dirty = true,
	.list_dirty = true,
};

static lv_obj_t *top_label;
static lv_obj_t *progress_label;
static lv_obj_t *list_label;
static char cached_top_text[128];
static char cached_progress_text[128];
static char cached_list_text[SONG_LIST_TEXT_BUF_SIZE];

static atomic_t power_events;
static atomic_t left_events;
static atomic_t right_events;

static struct gpio_callback power_cb;
static struct gpio_callback left_cb;
static struct gpio_callback right_cb;

static bool ends_with_wav(const char *name)
{
	size_t len = strlen(name);

	if (len < 4)
	{
		return false;
	}

	return tolower((unsigned char)name[len - 4]) == '.' &&
		   tolower((unsigned char)name[len - 3]) == 'w' &&
		   tolower((unsigned char)name[len - 2]) == 'a' &&
		   tolower((unsigned char)name[len - 1]) == 'v';
}

static void button_power_pressed(const struct device *dev, struct gpio_callback *cb, uint32_t pins)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(cb);
	ARG_UNUSED(pins);
	atomic_inc(&power_events);
}

static void button_left_pressed(const struct device *dev, struct gpio_callback *cb, uint32_t pins)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(cb);
	ARG_UNUSED(pins);
	atomic_inc(&left_events);
}

static void button_right_pressed(const struct device *dev, struct gpio_callback *cb, uint32_t pins)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(cb);
	ARG_UNUSED(pins);
	atomic_inc(&right_events);
}

static int init_buttons(void)
{
	int ret;

	if (!device_is_ready(button_power.port) || !device_is_ready(button_left.port) || !device_is_ready(button_right.port))
	{
		LOG_ERR("Button GPIO device not ready");
		return -ENODEV;
	}

	ret = gpio_pin_configure_dt(&button_power, GPIO_INPUT);
	if (ret)
	{
		return ret;
	}
	ret = gpio_pin_configure_dt(&button_left, GPIO_INPUT);
	if (ret)
	{
		return ret;
	}
	ret = gpio_pin_configure_dt(&button_right, GPIO_INPUT);
	if (ret)
	{
		return ret;
	}

	ret = gpio_pin_interrupt_configure_dt(&button_power, GPIO_INT_EDGE_TO_ACTIVE);
	if (ret)
	{
		return ret;
	}
	ret = gpio_pin_interrupt_configure_dt(&button_left, GPIO_INT_EDGE_TO_ACTIVE);
	if (ret)
	{
		return ret;
	}
	ret = gpio_pin_interrupt_configure_dt(&button_right, GPIO_INT_EDGE_TO_ACTIVE);
	if (ret)
	{
		return ret;
	}

	gpio_init_callback(&power_cb, button_power_pressed, BIT(button_power.pin));
	gpio_add_callback(button_power.port, &power_cb);

	gpio_init_callback(&left_cb, button_left_pressed, BIT(button_left.pin));
	gpio_add_callback(button_left.port, &left_cb);

	gpio_init_callback(&right_cb, button_right_pressed, BIT(button_right.pin));
	gpio_add_callback(button_right.port, &right_cb);

	return 0;
}

static int init_audio(void)
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

static int load_songs_from_root(struct app_state *state)
{
	int ret;
	char list_buf[SONG_LIST_BUF_SIZE];
	size_t buf_size = sizeof(list_buf);
	char *entry;

	memset(list_buf, 0, sizeof(list_buf));
	ret = sd_card_list_files(NULL, list_buf, &buf_size, false);
	if (ret)
	{
		LOG_ERR("Failed to list root files: %d", ret);
		return ret;
	}

	state->song_count = 0;
	entry = strtok(list_buf, "\r\n");
	while (entry != NULL && state->song_count < MAX_SONGS)
	{
		if (ends_with_wav(entry))
		{
			strncpy(state->songs[state->song_count], entry, MAX_SONG_NAME_LEN - 1);
			state->songs[state->song_count][MAX_SONG_NAME_LEN - 1] = '\0';
			state->song_count++;
		}
		entry = strtok(NULL, "\r\n");
	}

	if (state->song_count == 0)
	{
		state->selected_index = 0;
	}
	else if (state->selected_index >= state->song_count)
	{
		state->selected_index = 0;
	}

	state->list_dirty = true;
	state->ui_dirty = true;
	LOG_INF("Loaded %d WAV song(s) from SD root", state->song_count);
	return 0;
}

static int ui_init(void)
{
	if (!device_is_ready(display))
	{
		LOG_ERR("Display not ready");
		return -ENODEV;
	}

	if (display_blanking_off(display))
	{
		LOG_ERR("Failed to enable display");
		return -EIO;
	}

	lvgl_lock();

	lv_obj_t *scr = lv_scr_act();
	lv_obj_set_style_bg_color(scr, lv_color_white(), LV_PART_MAIN);
	lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, LV_PART_MAIN);

	top_label = lv_label_create(scr);
	lv_obj_set_style_text_color(top_label, lv_color_black(), LV_STATE_DEFAULT);
	lv_obj_set_style_text_font(top_label, &lv_font_montserrat_12, LV_STATE_DEFAULT);
	lv_obj_align(top_label, LV_ALIGN_TOP_LEFT, 3, 2);

	progress_label = lv_label_create(scr);
	lv_obj_set_style_text_color(progress_label, lv_color_black(), LV_STATE_DEFAULT);
	lv_obj_set_style_text_font(progress_label, &lv_font_montserrat_12, LV_STATE_DEFAULT);
	lv_obj_align(progress_label, LV_ALIGN_TOP_LEFT, 3, 16);

	list_label = lv_label_create(scr);
	lv_obj_set_style_text_color(list_label, lv_color_black(), LV_STATE_DEFAULT);
	lv_obj_set_style_text_font(list_label, &lv_font_montserrat_12, LV_STATE_DEFAULT);
	lv_obj_set_size(list_label, 196, 178);
	lv_obj_align(list_label, LV_ALIGN_TOP_LEFT, 3, 32);
	lv_label_set_long_mode(list_label, LV_LABEL_LONG_CLIP);

	lvgl_unlock();
	return 0;
}

static void ui_refresh(struct app_state *state)
{
	char top_text[128];
	char progress_text[128];
	char list_text[SONG_LIST_TEXT_BUF_SIZE];
	size_t off = 0;
	uint32_t percent = 0;

	if (state->data_total_bytes > 0)
	{
		percent = (state->data_streamed_bytes * 100U) / state->data_total_bytes;
		if (percent > 100U)
		{
			percent = 100U;
		}
	}

	if (state->playing_index >= 0 && state->playing_index < state->song_count)
	{
		const char *prefix = state->playback_state == PLAYBACK_PAUSED ? "Paused: " : "Now: ";
		snprintk(top_text, sizeof(top_text), "%s%s", prefix, state->songs[state->playing_index]);
	}
	else
	{
		snprintk(top_text, sizeof(top_text), "Now: (stopped)");
	}

	snprintk(progress_text, sizeof(progress_text), "Progress: %u%%", percent);

	if (state->list_dirty)
	{
		list_text[0] = '\0';
		if (state->song_count == 0)
		{
			snprintk(list_text, sizeof(list_text), "No .wav files in SD root.");
		}
		else
		{
			for (int i = 0; i < state->song_count && off < sizeof(list_text); i++)
			{
				const char selected = (i == state->selected_index) ? '>' : ' ';
				const char playing = (i == state->playing_index) ? '*' : ' ';
				int written = snprintk(&list_text[off], sizeof(list_text) - off, "%c%c %s\n", selected, playing, state->songs[i]);
				if (written < 0 || (size_t)written >= (sizeof(list_text) - off))
				{
					break;
				}
				off += (size_t)written;
			}
		}
	}

	lvgl_lock();
	if (strcmp(cached_top_text, top_text) != 0)
	{
		strncpy(cached_top_text, top_text, sizeof(cached_top_text) - 1);
		cached_top_text[sizeof(cached_top_text) - 1] = '\0';
		lv_label_set_text(top_label, cached_top_text);
	}

	if (strcmp(cached_progress_text, progress_text) != 0)
	{
		strncpy(cached_progress_text, progress_text, sizeof(cached_progress_text) - 1);
		cached_progress_text[sizeof(cached_progress_text) - 1] = '\0';
		lv_label_set_text(progress_label, cached_progress_text);
	}

	if (state->list_dirty && strcmp(cached_list_text, list_text) != 0)
	{
		strncpy(cached_list_text, list_text, sizeof(cached_list_text) - 1);
		cached_list_text[sizeof(cached_list_text) - 1] = '\0';
		lv_label_set_text(list_label, cached_list_text);
	}
	lvgl_unlock();

	state->list_dirty = false;
}

static void stop_playback(struct app_state *state, bool close_file, bool drop_i2s)
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

static int queue_one_block(struct app_state *state, bool *eof)
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

static int prefill_and_start(struct app_state *state)
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

static int start_selected_song(struct app_state *state)
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

static int pause_song(struct app_state *state)
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

static int resume_song(struct app_state *state)
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

static void process_playback(struct app_state *state)
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

static int take_events(atomic_t *counter)
{
	int value = atomic_get(counter);

	if (value > 0)
	{
		atomic_set(counter, 0);
	}

	return value;
}

static void move_selection(struct app_state *state, int step)
{
	if (state->song_count == 0)
	{
		return;
	}

	state->selected_index = (state->selected_index + step + state->song_count) % state->song_count;
	state->ui_dirty = true;
	state->list_dirty = true;
}

static void handle_buttons(struct app_state *state)
{
	int events;

	events = take_events(&left_events);
	for (int i = 0; i < events; i++)
	{
		move_selection(state, -1);
	}

	events = take_events(&right_events);
	for (int i = 0; i < events; i++)
	{
		move_selection(state, 1);
	}

	events = take_events(&power_events);
	for (int i = 0; i < events; i++)
	{
		int ret = 0;
		if (state->playback_state == PLAYBACK_PLAYING)
		{
			ret = pause_song(state);
		}
		else if (state->playback_state == PLAYBACK_PAUSED && state->selected_index == state->playing_index)
		{
			ret = resume_song(state);
		}
		else
		{
			ret = start_selected_song(state);
		}

		if (ret)
		{
			LOG_ERR("Power action failed: %d", ret);
			stop_playback(state, true, true);
		}
	}
}

int main(void)
{
	int ret;
	int64_t last_lvgl_handler_ms = 0;

	LOG_INF("eno-os: LVGL WAV player starting");

	ret = sd_card_init();
	if (ret)
	{
		LOG_ERR("SD card init failed: %d", ret);
		return ret;
	}

	ret = init_audio();
	if (ret)
	{
		LOG_ERR("Audio init failed: %d", ret);
		return ret;
	}

	ret = ui_init();
	if (ret)
	{
		LOG_ERR("UI init failed: %d", ret);
		return ret;
	}

	ret = load_songs_from_root(&app);
	if (ret)
	{
		LOG_ERR("Failed to load songs: %d", ret);
	}

	ret = init_buttons();
	if (ret)
	{
		LOG_ERR("Button init failed: %d", ret);
		return ret;
	}

	ui_refresh(&app);

	while (1)
	{
		bool did_ui_refresh = false;
		int64_t now_ms;

		handle_buttons(&app);
		process_playback(&app);

		if (app.ui_dirty)
		{
			ui_refresh(&app);
			app.ui_dirty = false;
			did_ui_refresh = true;
		}

		now_ms = k_uptime_get();
		if ((app.playback_state != PLAYBACK_PLAYING || did_ui_refresh) &&
			(now_ms - last_lvgl_handler_ms) >= 50)
		{
			lvgl_lock();
			lv_timer_handler();
			lvgl_unlock();
			last_lvgl_handler_ms = now_ms;
		}

		k_msleep(5);
	}
}

