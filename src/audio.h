#pragma once

#include <zephyr/drivers/i2c.h>
#include <zephyr/drivers/i2s.h>

#include "sd_card.h"
#include "main.h"

int init_audio(void);

/* Volume change per volume-button press, in dB */
#define AUDIO_VOLUME_STEP_DB 2

int audio_volume_step(int step_db);
void stop_playback(struct app_state *state, bool close_file, bool drop_i2s);
int queue_one_block(struct app_state *state, bool *eof);
int prefill_and_start(struct app_state *state);
int start_song(struct app_state *state, const char *path);
int pause_song(struct app_state *state);
int resume_song(struct app_state *state);


