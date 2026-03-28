#pragma once

#include <zephyr/drivers/i2c.h>
#include <zephyr/drivers/i2s.h>

#include "sd_card.h"
#include "main.h"

int init_audio(void);
void stop_playback(struct app_state *state, bool close_file, bool drop_i2s);
int queue_one_block(struct app_state *state, bool *eof);
int prefill_and_start(struct app_state *state);
int start_selected_song(struct app_state *state);
int pause_song(struct app_state *state);
int resume_song(struct app_state *state);
void process_playback(struct app_state *state);


