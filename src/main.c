#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/fatal.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/reboot.h>
#include <zephyr/sys/mem_stats.h>
#include <string.h>

#include <lvgl.h>
#include <lvgl_zephyr.h>

#include "main.h"
#include "sd_card.h"
#include "buttons.h"
#include "knob.h"
#include "audio.h"
#include "ui.h"
#include "browser.h"

LOG_MODULE_REGISTER(MAIN, LOG_LEVEL_DBG);

/* The board exhibits rare transient flash-read corruption under load
 * (fault dumps show garbage literal loads inside _isr_wrapper and
 * undefined instructions at valid code; confirmed with two different
 * SD cards). A warm reboot fully restores function and the boot flow
 * never triggers the fault, so auto-recover instead of freezing.
 */
void k_sys_fatal_error_handler(unsigned int reason, const struct arch_esf *esf)
{
	LOG_ERR("Fatal %u pc 0x%08x lr 0x%08x -- rebooting",
		reason,
		(esf != NULL) ? esf->basic.pc : 0,
		(esf != NULL) ? esf->basic.lr : 0);
	sys_reboot(SYS_REBOOT_WARM);

	CODE_UNREACHABLE;
}

/* LVGL heap diagnostics: the LVGL pool sits directly before the idle
 * thread TCB; periodic headroom logs watch for allocation pressure
 * near the pool end while the corruption source is investigated.
 */
extern void lvgl_heap_stats(struct sys_memory_stats *stats);

static struct app_state app = {
	.browser = {
		.cwd = "/SD:",
		.count = 0,
		.selected = 0,
		.scroll_top = 0,
	},
	.ui_mode = UI_MODE_BROWSER,
	.playing_path = {0},
	.playing_dir = {0},
	.playback_state = PLAYBACK_STOPPED,
	.file_open = false,
	.i2s_started = false,
	.elapsed_s = 0,
	.total_s = 0,
	.shuffle = false,
	.loop = false,
	.ui_dirty = true,
	.art_dirty = true,
};

/* Scratch listing for shuffle/next/prev on the playing folder */
static struct browser_entry playlist_entries[MAX_BROWSER_ENTRIES];

/* ------------------------------------------------------------------ */
/* Playback control                                                    */
/* ------------------------------------------------------------------ */

static const char *path_basename(const char *path)
{
	const char *slash = strrchr(path, '/');

	return (slash != NULL) ? slash + 1 : path;
}

static int play_path(struct app_state *state, const char *path, const char *dir)
{
	/* a song explicitly started by the user cancels a pending
	 * auto-advance from a song that just ended
	 */
	atomic_set(&state->advance_request, 0);

	int ret = start_song(state, path);

	if (ret)
	{
		LOG_ERR("Start %s failed: %d", path, ret);
		return ret;
	}

	strncpy(state->playing_path, path, sizeof(state->playing_path) - 1);
	state->playing_path[sizeof(state->playing_path) - 1] = '\0';
	strncpy(state->playing_dir, dir, sizeof(state->playing_dir) - 1);
	state->playing_dir[sizeof(state->playing_dir) - 1] = '\0';
	state->art_dirty = true;

	return 0;
}

static int restart_song(struct app_state *state)
{
	if (state->playing_path[0] == '\0')
	{
		return -ENOENT;
	}

	/* restart reloads the file from byte 0 and plays */
	return play_path(state, state->playing_path, state->playing_dir);
}

/* Find the currently playing entry in a fresh listing of its folder */
static int find_playing_entry(const char *playing_name,
	const struct browser_entry *entries, int count)
{
	for (int i = 0; i < count; i++)
	{
		if (entries[i].is_audio && strcmp(entries[i].name, playing_name) == 0)
		{
			return i;
		}
	}
	return -1;
}

static int next_audio_entry(const struct browser_entry *entries, int count, int from,
	bool forward, bool random)
{
	int audio[MAX_BROWSER_ENTRIES];
	int audio_count = 0;

	for (int i = 0; i < count; i++)
	{
		if (entries[i].is_audio)
		{
			audio[audio_count++] = i;
		}
	}

	if (audio_count == 0)
	{
		return -1;
	}

	if (random)
	{
		if (audio_count == 1)
		{
			return audio[0];
		}

		/* cheap LCG seeded once from the cycle counter: fine for
		 * shuffle play (no crypto needed)
		 */
		static uint32_t rnd;
		static bool seeded;

		if (!seeded)
		{
			rnd = 0x1234abcd ^ k_cycle_get_32();
			seeded = true;
		}
		rnd = rnd * 1664525u + 1013904223u;

		uint32_t pick = rnd % (uint32_t)(audio_count - 1);
		int pos = 0;

		for (int i = 0; i < audio_count; i++)
		{
			if (audio[i] == from)
			{
				pos = i;
				break;
			}
		}
		return audio[(pos + 1 + (int)pick) % audio_count];
	}

	int pos = -1;

	for (int i = 0; i < audio_count; i++)
	{
		if (audio[i] == from)
		{
			pos = i;
			break;
		}
	}
	if (pos < 0)
	{
		pos = 0;
	}

	return forward ? audio[(pos + 1) % audio_count]
		       : audio[(pos + audio_count - 1) % audio_count];
}

static int skip_song(struct app_state *state, bool forward)
{
	char path[MAX_PATH_LEN];
	int idx;

	if (state->playing_dir[0] == '\0')
	{
		return -ENOENT;
	}

	int count = browser_scan_dir(state->playing_dir,
		playlist_entries, MAX_BROWSER_ENTRIES, false);
	if (count <= 0)
	{
		return -ENOENT;
	}

	int from = find_playing_entry(path_basename(state->playing_path),
		playlist_entries, count);

	idx = next_audio_entry(playlist_entries, count, from, forward, state->shuffle);
	if (idx < 0)
	{
		return -ENOENT;
	}

	path_join(state->playing_dir, playlist_entries[idx].name, path, sizeof(path));

	return play_path(state, path, state->playing_dir);
}

/* Called when the pump reports end-of-song: loop repeats the current
 * song, shuffle picks a random other one from the same folder,
 * otherwise play the next one
 */
static void advance_auto(struct app_state *state)
{
	if (state->loop)
	{
		if (restart_song(state))
		{
			stop_playback(state, true, true);
		}
		return;
	}

	if (skip_song(state, true))
	{
		stop_playback(state, true, true);
	}
}

/* ------------------------------------------------------------------ */
/* UI mode + button handling                                           */
/* ------------------------------------------------------------------ */

static void toggle_mode(struct app_state *state)
{
	state->ui_mode = (state->ui_mode == UI_MODE_BROWSER) ? UI_MODE_PLAYER
							     : UI_MODE_BROWSER;
	if (state->ui_mode == UI_MODE_PLAYER)
	{
		state->art_dirty = true;
	}
	ui_switch_mode(state->ui_mode);
	state->ui_dirty = true;
}

static void browser_single_left(struct app_state *state)
{
	/* go up a directory; at the root this is a no-op */
	if (browser_up(&state->browser))
	{
		state->ui_dirty = true;
	}
}

static void browser_single_right(struct app_state *state)
{
	struct browser_ctx *br = &state->browser;

	if (br->count == 0)
	{
		return;
	}

	struct browser_entry *e = &br->entries[br->selected];

	if (e->is_dir)
	{
		if (browser_enter_selected(br))
		{
			state->ui_dirty = true;
		}
		return;
	}

	if (e->is_audio)
	{
		char path[MAX_PATH_LEN];

		browser_selected_path(br, path, sizeof(path));
		if (play_path(state, path, br->cwd))
		{
			stop_playback(state, true, true);
		}
	}
}

/* Single press dispatch and multi-press (double press etc.) logic.
 * Side button presses are counted inside a window; when the window
 * expires the accumulated count picks the action:
 *   left:  1 = up dir / toggle shuffle, 2 = restart song, 3+ = prev
 *   right: 1 = enter dir / play song / toggle loop, 2+ = next song
 * Presses on both side buttons inside COMBO_WINDOW_MS switch the UI
 * mode instead.
 */
#define MULTI_PRESS_WINDOW_MS 300
/* "both side buttons at once" - humans pressing together land within
 * ~50 ms; keep the window tight so sequential single presses of the
 * two buttons are not misread as a combo
 */
#define COMBO_WINDOW_MS 80
/* A double press that follows a restart this closely is treated as
 * the spec's "double press again" -> previous song
 */
#define RESTART_CHAIN_MS 800

struct side_button_ctx {
	int pending;
	int64_t deadline;
	int64_t last_press;
};

static struct side_button_ctx left_ctx;
static struct side_button_ctx right_ctx;

static void side_press(struct app_state *state, struct side_button_ctx *btn,
	struct side_button_ctx *other)
{
	int64_t now = k_uptime_get();

	if (now - other->last_press <= COMBO_WINDOW_MS)
	{
		/* both side buttons: mode switch wins, cancel everything
		 * pending on either button and drop both timestamps so a
		 * third quick press cannot re-trigger the combo
		 */
		btn->pending = 0;
		other->pending = 0;
		other->last_press = 0;
		btn->last_press = 0;
		toggle_mode(state);
		return;
	}

	/* re-arm on every press so a "double press again" (for prev
	 * song) is recognized even when it comes after the previous
	 * window already dispatched
	 */
	btn->deadline = now + MULTI_PRESS_WINDOW_MS;
	btn->pending++;
	btn->last_press = now;
}

static void side_button_dispatch(struct app_state *state, struct side_button_ctx *btn,
	bool is_left, int64_t now)
{
	int n;
	static int64_t last_restart_ms;

	if (btn->pending == 0 || now < btn->deadline)
	{
		return;
	}

	n = btn->pending;
	btn->pending = 0;

	if (is_left)
	{
		if (n == 1)
		{
			last_restart_ms = 0;
			if (state->ui_mode == UI_MODE_BROWSER)
			{
				browser_single_left(state);
			}
			else
			{
				state->shuffle = !state->shuffle;
				state->ui_dirty = true;
			}
		}
		else if (n == 2)
		{
			/* "double press again" = previous song: a restart
			 * that follows a recent restart is actually the
			 * user going back
			 */
			if (last_restart_ms != 0 && now - last_restart_ms <= RESTART_CHAIN_MS)
			{
				last_restart_ms = 0;
				if (skip_song(state, false))
				{
					stop_playback(state, true, true);
				}
			}
			else
			{
				last_restart_ms = now;
				if (restart_song(state))
				{
					stop_playback(state, true, true);
				}
			}
		}
		else
		{
			last_restart_ms = 0;
			if (skip_song(state, false))
			{
				stop_playback(state, true, true);
			}
		}
	}
	else
	{
		if (n == 1)
		{
			if (state->ui_mode == UI_MODE_BROWSER)
			{
				browser_single_right(state);
			}
			else
			{
				state->loop = !state->loop;
				state->ui_dirty = true;
			}
		}
		else
		{
			if (skip_song(state, true))
			{
				stop_playback(state, true, true);
			}
		}
	}
}

static void handle_buttons(struct app_state *state)
{
	int events;
	int64_t now = k_uptime_get();
	static int64_t last_power_action;

	/* Collapse power-button bursts (bounces, impatient double
	 * presses) into one action. Play/pause only: songs are picked
	 * in the browser.
	 */
	events = buttons_take_power_events();
	if (events > 0 && (now - last_power_action) >= 500)
	{
		last_power_action = now;

		int ret = 0;

		if (state->playback_state == PLAYBACK_PLAYING)
		{
			ret = pause_song(state);
		}
		else if (state->playback_state == PLAYBACK_PAUSED)
		{
			ret = resume_song(state);
		}

		if (ret)
		{
			LOG_ERR("Power action failed: %d", ret);
			stop_playback(state, true, true);
		}
	}

	events = buttons_take_left_events();
	for (int i = 0; i < events; i++)
	{
		side_press(state, &left_ctx, &right_ctx);
	}

	events = buttons_take_right_events();
	for (int i = 0; i < events; i++)
	{
		side_press(state, &right_ctx, &left_ctx);
	}

	(void)side_button_dispatch(state, &left_ctx, true, now);
	(void)side_button_dispatch(state, &right_ctx, false, now);

	events = buttons_take_volume_up_events();
	for (int i = 0; i < events; i++)
	{
		audio_volume_step(AUDIO_VOLUME_STEP_DB);
	}

	events = buttons_take_volume_down_events();
	for (int i = 0; i < events; i++)
	{
		audio_volume_step(-AUDIO_VOLUME_STEP_DB);
	}
}

/* ------------------------------------------------------------------ */

int main(void)
{
	int ret;
	int64_t last_lvgl_handler_ms = 0;

	LOG_INF("eno-os: LVGL player starting");

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

	ret = browser_open_dir(&app.browser, "/SD:");
	if (ret)
	{
		LOG_ERR("Failed to list SD root: %d", ret);
	}

	ret = init_buttons();
	if (ret)
	{
		LOG_ERR("Button init failed: %d", ret);
		return ret;
	}

	ret = init_knob();
	if (ret)
	{
		LOG_ERR("Knob init failed: %d", ret);
		return ret;
	}

	ui_refresh(&app);

	while (1)
	{
		bool did_ui_refresh = false;
		int knob_steps;
		int64_t now_ms;

		handle_buttons(&app);

		/* song finished: advance (shuffle/loop aware) */
		if (atomic_get(&app.advance_request))
		{
			atomic_set(&app.advance_request, 0);
			advance_auto(&app);
		}

		if (app.ui_mode == UI_MODE_BROWSER)
		{
			knob_steps = knob_poll();
			if (knob_steps != 0 &&
				browser_move_selection(&app.browser, knob_steps))
			{
				knob_haptic_pulse();
				app.ui_dirty = true;
			}
		}
		else
		{
			/* keep the hall sensor baseline fresh and discard
			 * any accumulated travel (unconditionally, so
			 * returning to the browser never produces phantom
			 * scrolls)
			 */
			knob_poll();
			knob_discard();
		}

		if (app.ui_dirty)
		{
			ui_refresh(&app);
			app.ui_dirty = false;
			did_ui_refresh = true;
			ui_full_refresh_check(&app);
		}

		now_ms = k_uptime_get();

		static int64_t last_heap_log_ms;

		if (now_ms - last_heap_log_ms >= 30000)
		{
			struct sys_memory_stats stats;

			lvgl_heap_stats(&stats);
			last_heap_log_ms = now_ms;
			LOG_INF("LVGL heap: free %zu allocated %zu peak %zu",
				stats.free_bytes, stats.allocated_bytes,
				stats.max_allocated_bytes);
		}

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
