#pragma once

#include <stddef.h>
#include <stdint.h>

#include <zephyr/fs/fs.h>

enum audio_file_format {
	AUDIO_FILE_WAV,
	AUDIO_FILE_MP3,
};

/*
 * Unified streaming decoder: consumes a WAV or MP3 file opened by the
 * caller and produces interleaved stereo 16-bit PCM at the stream's
 * native sample rate. Mono sources are upmixed; 8/24/32-bit sources
 * are converted to 16-bit.
 */
struct audio_decoder {
	enum audio_file_format format;
	uint32_t sample_rate;
	/* playback position / duration in milliseconds */
	uint32_t elapsed_ms;
	uint32_t total_ms;

	/* internal */
	struct fs_file_t *file;
	uint32_t file_size;
	bool opened;
	bool file_exhausted;
	uint32_t file_read_total;

	/* wav */
	uint32_t data_size;
	uint32_t data_read;
	uint32_t byte_rate;
	uint8_t src_bits;
	uint8_t src_channels;

	/* mp3 */
	void *mp3;
	uint8_t *ring;
	uint32_t ring_len;
	uint32_t ring_pos;
	bool synced;
	uint32_t elapsed_frames;
	int16_t *frame_pcm;
	uint32_t pending_frames;
	uint32_t pending_pos;

	/* conversion scratch (wav) */
	uint8_t *raw_buf;
};

int decoder_open(struct audio_decoder *dec, struct fs_file_t *file, uint32_t file_size);
void decoder_close(struct audio_decoder *dec);

/*
 * Top up the decoder's internal input buffer from the file. Call
 * regularly (e.g. before each block fill): the SD read stall then
 * lands while the audio queue is still full instead of when the
 * decoder is about to run dry.
 */
void decoder_prefetch(struct audio_decoder *dec);

/* Returns the number of stereo frames written to dst (0 = end of stream). */
size_t decoder_fill(struct audio_decoder *dec, int16_t *dst, size_t max_frames);
