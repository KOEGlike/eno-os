#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/logging/log.h>
#include <zephyr/drivers/display.h>
#include <stdio.h>
#include <string.h>

#include <lvgl.h>
#include <lvgl_zephyr.h>

#include "ui.h"

LOG_MODULE_REGISTER(UI, LOG_LEVEL_DBG);

#define DISPLAY_NODE DT_CHOSEN(zephyr_display)
static const struct device *display = DEVICE_DT_GET(DISPLAY_NODE);

static lv_obj_t *top_label;
static lv_obj_t *progress_label;
static lv_obj_t *list_label;
static char cached_top_text[128];
static char cached_progress_text[128];
static char cached_list_text[SONG_LIST_TEXT_BUF_SIZE];

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

void ui_refresh(struct app_state *state)
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
