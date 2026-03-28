#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <lvgl.h>
#include <lvgl_zephyr.h>

#include "main.h"
#include "sd_card.h"
#include "buttons.h"
#include "audio.h"
#include "ui.h"
#include "songs.h"

LOG_MODULE_REGISTER(MAIN, LOG_LEVEL_DBG);

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

	events = buttons_take_left_events();
	for (int i = 0; i < events; i++)
	{
		move_selection(state, -1);
	}

	events = buttons_take_right_events();
	for (int i = 0; i < events; i++)
	{
		move_selection(state, 1);
	}

	events = buttons_take_power_events();
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
