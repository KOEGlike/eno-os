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
/* I2S block: sized for ~3200 frames at any rate (72.6 ms at 44.1 kHz,
 * 200 ms at 16 kHz) so one SD read per block stays well under the
 * audio time it carries even at 4 MHz SPI. Not derived from
 * SAMPLE_FREQUENCY: high-rate streams need the bigger blocks.
 */
#define MAX_BLOCK_FRAMES 3200
#define BLOCK_SIZE (MAX_BLOCK_FRAMES * NUMBER_OF_CHANNELS * BYTES_PER_SAMPLE)
/* The pump paces itself at QUEUE_SOFT_LIMIT blocks (see audio.c), so
 * the slab only needs to cover the driver msgq (16) plus a couple of
 * blocks in flight.
 */
#define BLOCK_COUNT 10
#define INITIAL_BLOCKS 8
#define TIMEOUT 2000
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
