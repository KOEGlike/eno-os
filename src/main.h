#pragma once

#include <zephyr/fs/fs.h>

#define MAX_SONGS 32
#define MAX_SONG_NAME_LEN 96
#define SONG_LIST_TEXT_BUF_SIZE 4096
#define WAV_HEADER_SIZE 44
#define SAMPLE_FREQUENCY 16000
#define SAMPLE_BIT_WIDTH 16
#define BYTES_PER_SAMPLE 2
#define NUMBER_OF_CHANNELS 2
#define SAMPLES_PER_BLOCK ((SAMPLE_FREQUENCY / 10) * NUMBER_OF_CHANNELS)
#define INITIAL_BLOCKS 8
#define TIMEOUT 2000
#define BLOCK_SIZE (BYTES_PER_SAMPLE * SAMPLES_PER_BLOCK)
#define BLOCK_COUNT (INITIAL_BLOCKS + 6)
#define PROGRESS_UI_UPDATE_MS 10000
#define PROGRESS_UI_STEP_PCT 10

enum playback_state {
	PLAYBACK_STOPPED = 0,
	PLAYBACK_PLAYING,
	PLAYBACK_PAUSED,
};

struct app_state {
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


