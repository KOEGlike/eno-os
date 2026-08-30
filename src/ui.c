#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/logging/log.h>
#include <zephyr/drivers/display.h>
#include <stdio.h>
#include <string.h>

#include <lvgl.h>
#include <lvgl_zephyr.h>
#include "misc/cache/instance/lv_image_cache.h"

#include "ui.h"
#include "icons.h"
#include "art.h"
#include "i1_decoder.h"

LOG_MODULE_REGISTER(UI, LOG_LEVEL_DBG);

#define DISPLAY_NODE DT_CHOSEN(zephyr_display)
static const struct device *display = DEVICE_DT_GET(DISPLAY_NODE);

/* Full refreshes clear ghosting accumulated by partial refreshes */
#define FULL_REFRESH_INTERVAL 10

/* Browser layout geometry (200x200 panel) */
#define PROGRESS_H 3
#define HEADER_Y 5
#define HEADER_H 12
#define HEADER_SEP_Y 19
#define LIST_Y 21
#define ROW_STRIDE 18

/* Player layout geometry */
#define ART_SIZE ART_W
#define TITLE_Y 126
#define ARTIST_Y 140
#define ICONS_Y 156
#define TIMES_Y 178
#define PLAYER_BAR_Y 191
#define PLAYER_BAR_H 7

static lv_obj_t *browser_view;
static lv_obj_t *player_view;

/* Browser: top progress bar + now playing header */
static lv_obj_t *browser_progress;
static lv_obj_t *now_label;
static lv_obj_t *time_label;
static lv_obj_t *header_sep;
static lv_obj_t *row_labels[BROWSER_VISIBLE_ROWS];
static lv_obj_t *row_seps[BROWSER_VISIBLE_ROWS];

/* Player */
static lv_obj_t *art_img;
static lv_obj_t *shuffle_img;
static lv_obj_t *playpause_img;
static lv_obj_t *loop_img;
static lv_obj_t *title_label;
static lv_obj_t *artist_label;
static lv_obj_t *elapsed_label;
static lv_obj_t *total_label;
static lv_obj_t *player_progress;

/* Album art canvas rendered by art_render(); handed to LVGL as an
 * I1 image descriptor (bit 1 = paper, bit 0 = ink; no palette needed
 * for the I1->I1 software blend on this mono display)
 */
static uint8_t art_canvas[ART_STRIDE * ART_H];
static lv_image_dsc_t art_dsc;

/* Cached text to avoid pointless LVGL invalidations on e-ink */
static char cached_now_text[128];
static char cached_time_text[32];
static char cached_row_text[BROWSER_VISIBLE_ROWS][160];
static int8_t cached_row_state[BROWSER_VISIBLE_ROWS]; /* -1 hidden */
static char cached_title_text[128];
static char cached_artist_text[MAX_ARTIST_LEN];
static char cached_elapsed_text[16];
static char cached_total_text[16];
static int cached_browser_progress_w = -1;
static int cached_player_progress_w = -1;
static int cached_playpause_icon = -1;
static int cached_shuffle_icon = -1;
static int cached_loop_icon = -1;

static uint32_t partial_refreshes;

static void style_bar(lv_obj_t *obj)
{
	lv_obj_remove_style_all(obj);
	lv_obj_set_style_bg_color(obj, lv_color_black(), LV_PART_MAIN);
	lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, LV_PART_MAIN);
	lv_obj_set_size(obj, 0, PROGRESS_H);
}

static void style_sep(lv_obj_t *obj)
{
	lv_obj_remove_style_all(obj);
	lv_obj_set_style_bg_color(obj, lv_color_black(), LV_PART_MAIN);
	lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, LV_PART_MAIN);
	lv_obj_set_size(obj, 200, 1);
}

/* I1 images blend bit-exact into the mono framebuffer: no recolor
 * styling needed, the bitmaps already use the paper/ink convention
 */
static void set_bar_width(lv_obj_t *bar, int w, int *cached)
{
	if (*cached == w)
	{
		return;
	}
	*cached = w;
	lv_obj_set_width(bar, w);
}

/* Note: all LVGL access happens on the main thread only
 * (CONFIG_LV_Z_FLUSH_THREAD=n), so no extra locking is needed here;
 * ui_init/ui_full_refresh_check lock because they run while other
 * calls may be interleaved with lv_timer_handler.
 */
static void set_label_text(lv_obj_t *label, const char *text, char *cached, size_t cached_len)
{
	if (strcmp(cached, text) == 0)
	{
		return;
	}
	strncpy(cached, text, cached_len - 1);
	cached[cached_len - 1] = '\0';
	lv_label_set_text(label, cached);
}

static void set_row(lv_obj_t *label, lv_obj_t *sep, int index, const char *text,
	bool selected, bool visible)
{
	char *cached = cached_row_text[index];

	if (!visible)
	{
		if (cached_row_state[index] != -1)
		{
			cached_row_state[index] = -1;
			lv_obj_add_flag(label, LV_OBJ_FLAG_HIDDEN);
			lv_obj_add_flag(sep, LV_OBJ_FLAG_HIDDEN);
		}
		return;
	}

	set_label_text(label, text, cached, sizeof(cached_row_text[index]));

	if (cached_row_state[index] != (selected ? 1 : 0))
	{
		cached_row_state[index] = selected ? 1 : 0;
		if (selected)
		{
			lv_obj_set_style_bg_color(label, lv_color_black(), LV_PART_MAIN);
			lv_obj_set_style_bg_opa(label, LV_OPA_COVER, LV_PART_MAIN);
			lv_obj_set_style_text_color(label, lv_color_white(), LV_PART_MAIN);
		}
		else
		{
			lv_obj_set_style_bg_opa(label, LV_OPA_TRANSP, LV_PART_MAIN);
			lv_obj_set_style_text_color(label, lv_color_black(), LV_PART_MAIN);
		}
	}

	lv_obj_remove_flag(label, LV_OBJ_FLAG_HIDDEN);
	lv_obj_remove_flag(sep, LV_OBJ_FLAG_HIDDEN);
}

static void format_time(char *buf, size_t len, uint32_t s)
{
	snprintk(buf, len, "%02u:%02u", s / 60, s % 60);
}

/* Display name/artist of the playing song: ID3 title when the playing
 * folder is the one being browsed (its entries carry the tags), file
 * name otherwise. artist is empty when no tag is available.
 */
static void playing_title_artist(const struct app_state *state,
	char *title, size_t title_len, char *artist, size_t artist_len)
{
	const struct browser_ctx *br = &state->browser;
	const char *name = strrchr(state->playing_path, '/');
	const char *tag_title = NULL;
	const char *tag_artist = NULL;

	name = (name != NULL) ? name + 1 : state->playing_path;

	if (strcmp(br->cwd, state->playing_dir) == 0)
	{
		for (int i = 0; i < br->count; i++)
		{
			if (!br->entries[i].is_dir &&
				strcmp(br->entries[i].name, name) == 0)
			{
				tag_title = br->entries[i].title;
				tag_artist = br->entries[i].artist;
				break;
			}
		}
	}

	if (tag_title != NULL && tag_title[0] != '\0')
	{
		strncpy(title, tag_title, title_len - 1);
		title[title_len - 1] = '\0';
	}
	else
	{
		strncpy(title, name, title_len - 1);
		title[title_len - 1] = '\0';
	}

	if (artist != NULL && artist_len > 0)
	{
		if (tag_artist != NULL)
		{
			strncpy(artist, tag_artist, artist_len - 1);
			artist[artist_len - 1] = '\0';
		}
		else
		{
			artist[0] = '\0';
		}
	}
}

static void refresh_browser(struct app_state *state)
{
	char time_text[32];
	char now_text[128];
	int bar_w;
	struct browser_ctx *br = &state->browser;

	/* top progress bar tracks the playing song */
	bar_w = (state->total_s > 0)
		? MIN((int)((200u * (uint64_t)state->elapsed_s) / state->total_s), 200)
		: 0;
	set_bar_width(browser_progress, bar_w, &cached_browser_progress_w);

	if (state->playback_state != PLAYBACK_STOPPED && state->playing_path[0] != '\0')
	{
		playing_title_artist(state, now_text, sizeof(now_text), NULL, 0);
	}
	else
	{
		snprintk(now_text, sizeof(now_text), "Nothing playing");
	}
	set_label_text(now_label, now_text, cached_now_text, sizeof(cached_now_text));

	format_time(time_text, sizeof(time_text), state->elapsed_s);
	strcat(time_text, "/");
	format_time(time_text + strlen(time_text), sizeof(time_text) - strlen(time_text),
		state->total_s);
	set_label_text(time_label, time_text, cached_time_text, sizeof(cached_time_text));

	/* file browser rows */
	for (int i = 0; i < BROWSER_VISIBLE_ROWS; i++)
	{
		int entry = br->scroll_top + i;
		char text[160];

		if (entry >= br->count)
		{
			set_row(row_labels[i], row_seps[i], i, "", false, false);
			continue;
		}

		struct browser_entry *e = &br->entries[entry];

		if (e->is_dir)
		{
			snprintk(text, sizeof(text), "%s %s", LV_SYMBOL_DIRECTORY, e->name);
		}
		else if (e->artist[0] != '\0')
		{
			snprintk(text, sizeof(text), "%s - %s", e->title, e->artist);
		}
		else
		{
			snprintk(text, sizeof(text), "%s", e->title);
		}

		set_row(row_labels[i], row_seps[i], i, text, entry == br->selected, true);
	}
}

static void refresh_player(struct app_state *state)
{
	char time_text[16];
	int bar_w;

	if (state->art_dirty)
	{
		if (state->playing_path[0] != '\0')
		{
			(void)art_render(state->playing_path, art_canvas);
		}
		else
		{
			art_render_fallback(art_canvas);
		}

		art_dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
		art_dsc.header.w = ART_W;
		art_dsc.header.h = ART_H;
		art_dsc.header.cf = LV_COLOR_FORMAT_I1;
		art_dsc.header.stride = ART_STRIDE;
		art_dsc.data_size = sizeof(art_canvas);
		art_dsc.data = art_canvas;
		lv_image_cache_drop((const void *)&art_dsc);
		lv_image_set_src(art_img, &art_dsc);
		state->art_dirty = false;
	}

	/* play/pause icon */
	int pp = (state->playback_state == PLAYBACK_PLAYING) ? 1 : 0;

	if (cached_playpause_icon != pp)
	{
		cached_playpause_icon = pp;
		lv_image_set_src(playpause_img, pp ? &icon_pause : &icon_play);
	}

	/* song name + artist below the art, left aligned */
	if (state->playback_state != PLAYBACK_STOPPED && state->playing_path[0] != '\0')
	{
		char title[128];
		char artist[MAX_ARTIST_LEN];

		playing_title_artist(state, title, sizeof(title), artist, sizeof(artist));
		set_label_text(title_label, title, cached_title_text, sizeof(cached_title_text));
		set_label_text(artist_label, artist, cached_artist_text, sizeof(cached_artist_text));
	}
	else
	{
		set_label_text(title_label, "Nothing playing",
			cached_title_text, sizeof(cached_title_text));
		set_label_text(artist_label, "", cached_artist_text, sizeof(cached_artist_text));
	}

	int sh = state->shuffle ? 1 : 0;

	if (cached_shuffle_icon != sh)
	{
		cached_shuffle_icon = sh;
		lv_image_set_src(shuffle_img, sh ? &icon_shuffle_on : &icon_shuffle_off);
	}

	int lp = state->loop ? 1 : 0;

	if (cached_loop_icon != lp)
	{
		cached_loop_icon = lp;
		lv_image_set_src(loop_img, lp ? &icon_loop_on : &icon_loop_off);
	}

	/* progress bar with times above */
	bar_w = (state->total_s > 0)
		? MIN((int)((200u * (uint64_t)state->elapsed_s) / state->total_s), 200)
		: 0;
	set_bar_width(player_progress, bar_w, &cached_player_progress_w);

	format_time(time_text, sizeof(time_text), state->elapsed_s);
	set_label_text(elapsed_label, time_text, cached_elapsed_text,
		sizeof(cached_elapsed_text));

	format_time(time_text, sizeof(time_text), state->total_s);
	set_label_text(total_label, time_text, cached_total_text,
		sizeof(cached_total_text));
}

void ui_refresh(struct app_state *state)
{
	if (state->ui_mode == UI_MODE_BROWSER)
	{
		refresh_browser(state);
	}
	else
	{
		refresh_player(state);
	}
}

int ui_init(void)
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

	/* claim I1 variable images before the bin decoder expands them
	 * through an ARGB8888 intermediate (which does not fit in RAM)
	 */
	i1_decoder_init();

	lv_obj_t *scr = lv_scr_act();

	lv_obj_set_style_bg_color(scr, lv_color_white(), LV_PART_MAIN);
	lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, LV_PART_MAIN);

	/* ---------- browser view ---------- */
	browser_view = lv_obj_create(scr);
	lv_obj_remove_style_all(browser_view);
	lv_obj_set_size(browser_view, 200, 200);
	lv_obj_set_pos(browser_view, 0, 0);
	lv_obj_remove_flag(browser_view, LV_OBJ_FLAG_SCROLLABLE);

	browser_progress = lv_obj_create(browser_view);
	style_bar(browser_progress);
	lv_obj_set_pos(browser_progress, 0, 0);

	now_label = lv_label_create(browser_view);
	lv_obj_set_style_text_color(now_label, lv_color_black(), LV_STATE_DEFAULT);
	lv_obj_set_style_text_font(now_label, &lv_font_montserrat_12, LV_STATE_DEFAULT);
	lv_label_set_long_mode(now_label, LV_LABEL_LONG_DOT);
	lv_obj_set_width(now_label, 112);
	lv_obj_set_pos(now_label, 3, HEADER_Y);

	time_label = lv_label_create(browser_view);
	lv_obj_set_style_text_color(time_label, lv_color_black(), LV_STATE_DEFAULT);
	lv_obj_set_style_text_font(time_label, &lv_font_montserrat_12, LV_STATE_DEFAULT);
	lv_obj_set_style_text_align(time_label, LV_TEXT_ALIGN_RIGHT, LV_STATE_DEFAULT);
	lv_obj_set_width(time_label, 80);
	lv_obj_set_pos(time_label, 117, HEADER_Y);

	header_sep = lv_obj_create(browser_view);
	style_sep(header_sep);
	lv_obj_set_pos(header_sep, 0, HEADER_SEP_Y);

	for (int i = 0; i < BROWSER_VISIBLE_ROWS; i++)
	{
		row_labels[i] = lv_label_create(browser_view);
		lv_obj_set_style_text_color(row_labels[i], lv_color_black(), LV_STATE_DEFAULT);
		lv_obj_set_style_text_font(row_labels[i], &lv_font_montserrat_12, LV_STATE_DEFAULT);
		lv_label_set_long_mode(row_labels[i], LV_LABEL_LONG_DOT);
		lv_obj_set_width(row_labels[i], 194);
		lv_obj_set_pos(row_labels[i], 3, LIST_Y + i * ROW_STRIDE);

		row_seps[i] = lv_obj_create(browser_view);
		style_sep(row_seps[i]);
		lv_obj_set_pos(row_seps[i], 0, LIST_Y + i * ROW_STRIDE + HEADER_H + 4);
	}

	/* ---------- player view ---------- */
	player_view = lv_obj_create(scr);
	lv_obj_remove_style_all(player_view);
	lv_obj_set_size(player_view, 200, 200);
	lv_obj_set_pos(player_view, 0, 0);
	lv_obj_remove_flag(player_view, LV_OBJ_FLAG_SCROLLABLE);

	art_img = lv_image_create(player_view);
	lv_obj_set_pos(art_img, (200 - ART_SIZE) / 2, 4);

	shuffle_img = lv_image_create(player_view);
	lv_image_set_src(shuffle_img, &icon_shuffle_off);
	lv_obj_set_pos(shuffle_img, 30, ICONS_Y);

	playpause_img = lv_image_create(player_view);
	lv_image_set_src(playpause_img, &icon_play);
	lv_obj_set_pos(playpause_img, 90, ICONS_Y);

	loop_img = lv_image_create(player_view);
	lv_image_set_src(loop_img, &icon_loop_off);
	lv_obj_set_pos(loop_img, 150, ICONS_Y);

	title_label = lv_label_create(player_view);
	lv_obj_set_style_text_color(title_label, lv_color_black(), LV_STATE_DEFAULT);
	lv_obj_set_style_text_font(title_label, &lv_font_montserrat_12, LV_STATE_DEFAULT);
	lv_label_set_long_mode(title_label, LV_LABEL_LONG_DOT);
	lv_obj_set_width(title_label, 194);
	lv_obj_set_pos(title_label, 3, TITLE_Y);

	artist_label = lv_label_create(player_view);
	lv_obj_set_style_text_color(artist_label, lv_color_black(), LV_STATE_DEFAULT);
	lv_obj_set_style_text_font(artist_label, &lv_font_montserrat_12, LV_STATE_DEFAULT);
	lv_label_set_long_mode(artist_label, LV_LABEL_LONG_DOT);
	lv_obj_set_width(artist_label, 194);
	lv_obj_set_pos(artist_label, 3, ARTIST_Y);

	elapsed_label = lv_label_create(player_view);
	lv_obj_set_style_text_color(elapsed_label, lv_color_black(), LV_STATE_DEFAULT);
	lv_obj_set_style_text_font(elapsed_label, &lv_font_montserrat_12, LV_STATE_DEFAULT);
	lv_obj_set_pos(elapsed_label, 3, TIMES_Y);

	total_label = lv_label_create(player_view);
	lv_obj_set_style_text_color(total_label, lv_color_black(), LV_STATE_DEFAULT);
	lv_obj_set_style_text_font(total_label, &lv_font_montserrat_12, LV_STATE_DEFAULT);
	lv_obj_set_style_text_align(total_label, LV_TEXT_ALIGN_RIGHT, LV_STATE_DEFAULT);
	lv_obj_set_width(total_label, 50);
	lv_obj_set_pos(total_label, 147, TIMES_Y);

	player_progress = lv_obj_create(player_view);
	style_bar(player_progress);
	lv_obj_set_size(player_progress, 0, PLAYER_BAR_H);
	lv_obj_set_pos(player_progress, 0, PLAYER_BAR_Y);

	lv_obj_add_flag(player_view, LV_OBJ_FLAG_HIDDEN);

	lvgl_unlock();
	return 0;
}

void ui_switch_mode(enum ui_mode mode)
{
	if (mode == UI_MODE_BROWSER)
	{
		lv_obj_remove_flag(browser_view, LV_OBJ_FLAG_HIDDEN);
		lv_obj_add_flag(player_view, LV_OBJ_FLAG_HIDDEN);
	}
	else
	{
		lv_obj_add_flag(browser_view, LV_OBJ_FLAG_HIDDEN);
		lv_obj_remove_flag(player_view, LV_OBJ_FLAG_HIDDEN);
	}

	/* mode switches are full-screen changes: reset the caches so
	 * every element is revalidated on the next refresh
	 */
	cached_browser_progress_w = -1;
	cached_player_progress_w = -1;
	cached_playpause_icon = -1;
	cached_shuffle_icon = -1;
	cached_loop_icon = -1;
	memset(cached_row_text, 0, sizeof(cached_row_text));
	for (int i = 0; i < BROWSER_VISIBLE_ROWS; i++)
	{
		cached_row_state[i] = -1;
	}
	cached_now_text[0] = '\0';
	cached_time_text[0] = '\0';
	cached_title_text[0] = '\0';
	cached_artist_text[0] = '\0';
	cached_elapsed_text[0] = '\0';
	cached_total_text[0] = '\0';
}

void ui_full_refresh_check(struct app_state *state)
{
	partial_refreshes++;

	if (partial_refreshes < FULL_REFRESH_INTERVAL)
	{
		return;
	}

	partial_refreshes = 0;

	/*
	 * Toggling blanking makes the display driver switch to the
	 * full-refresh profile and trigger a full update, which
	 * clears accumulated ghosting. Skip while playing since the
	 * full refresh blocks for a couple of seconds.
	 */
	if (state->playback_state == PLAYBACK_PLAYING)
	{
		return;
	}

	lvgl_lock();
	display_blanking_on(display);
	display_blanking_off(display);
	lvgl_unlock();
}
