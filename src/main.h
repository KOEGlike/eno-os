#pragma once

#include <zephyr/fs/fs.h>
#include <zephyr/sys/atomic.h>

#define MAX_PATH_LEN 128
#define MAX_NAME_LEN 96
#define MAX_TITLE_LEN 48
#define MAX_ARTIST_LEN 48
#define MAX_BROWSER_ENTRIES 24
/* Rows of the file list visible at once */
#define BROWSER_VISIBLE_ROWS 9
#define SAMPLE_FREQUENCY 16000
#define SAMPLE_BIT_WIDTH 16
#define BYTES_PER_SAMPLE 2
#define NUMBER_OF_CHANNELS 2
#define SAMPLES_PER_BLOCK ((SAMPLE_FREQUENCY / 10) * NUMBER_OF_CHANNELS)
#define INITIAL_BLOCKS 16
#define TIMEOUT 2000
#define BLOCK_SIZE (BYTES_PER_SAMPLE * SAMPLES_PER_BLOCK)
/* The pump paces itself at QUEUE_SOFT_LIMIT blocks (see audio.c), so
 * the slab only needs to cover the driver msgq (16) plus a couple of
 * blocks in flight. FLAC decoding needs the BSS this frees.
 */
#define BLOCK_COUNT 18
#define PROGRESS_UI_UPDATE_MS 400

enum playback_state {
	PLAYBACK_STOPPED = 0,
	PLAYBACK_PLAYING,
	PLAYBACK_PAUSED,
};

enum ui_mode {
	UI_MODE_BROWSER = 0,
	UI_MODE_PLAYER,
};

struct browser_entry {
	char name[MAX_NAME_LEN];
	bool is_dir;
	bool is_audio;
	bool scanned;
	bool has_art;
	char title[MAX_TITLE_LEN];
	char artist[MAX_ARTIST_LEN];
};

struct browser_ctx {
	char cwd[MAX_PATH_LEN];
	struct browser_entry entries[MAX_BROWSER_ENTRIES];
	int count;
	int selected;
	int scroll_top;
};

struct app_state {
	struct browser_ctx browser;
	enum ui_mode ui_mode;

	/* The song that is (or was last) playing, and the folder it
	 * belongs to: shuffle/next/prev operate on that folder's
	 * listing, independent of where the browser currently is
	 */
	char playing_path[MAX_PATH_LEN];
	char playing_dir[MAX_PATH_LEN];

	enum playback_state playback_state;
	struct fs_file_t file;
	bool file_open;
	bool i2s_started;
	uint32_t elapsed_s;
	uint32_t total_s;
	int64_t last_progress_ui_ms;
	bool shuffle;
	bool loop;
	/* Set by the audio pump when a song ends; the main loop
	 * advances to the next song (shuffle/loop aware)
	 */
	atomic_t advance_request;
	bool ui_dirty;
	bool art_dirty;
};
