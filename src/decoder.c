#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>
#include <zephyr/fs/fs.h>
#include <string.h>
#include <stdlib.h>

#include "mp3dec.h"
#include "decoder.h"

LOG_MODULE_REGISTER(decoder, LOG_LEVEL_INF);

#define MP3_RING_SIZE		8192
#define WAV_RAW_BUF_SIZE	6400
#define MP3_GRANULE_MAX		1152

static uint8_t mp3_ring[MP3_RING_SIZE];
static int16_t mp3_frame_pcm[MP3_GRANULE_MAX * 2];
static uint8_t wav_raw[WAV_RAW_BUF_SIZE];

/* WAV fmt chunk payload fields (little endian) */
#define WAV_FMT_PCM		1
#define WAV_FMT_EXTENSIBLE	0xfffe

struct wav_fmt {
	uint16_t format;
	uint16_t channels;
	uint32_t rate;
	uint32_t byte_rate;
	uint16_t block_align;
	uint16_t bits;
};

static size_t wav_fill(struct audio_decoder *dec, int16_t *dst, size_t max_frames);
static size_t mp3_fill(struct audio_decoder *dec, int16_t *dst, size_t max_frames);

static int file_read_exact(struct fs_file_t *file, void *buf, size_t len, size_t *out)
{
	ssize_t n = fs_read(file, buf, len);

	if (n < 0) {
		return (int)n;
	}
	*out = (size_t)n;
	return 0;
}

static int file_read_u32_at(struct fs_file_t *file, uint32_t offset, uint8_t *buf, size_t len)
{
	ssize_t n;
	int ret;

	ret = fs_seek(file, offset, FS_SEEK_SET);
	if (ret < 0) {
		return ret;
	}

	n = fs_read(file, buf, len);
	if (n < 0) {
		return (int)n;
	}
	return (n == (ssize_t)len) ? 0 : -EIO;
}

static uint32_t le32(const uint8_t *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
	       ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint16_t le16(const uint8_t *p)
{
	return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static int wav_parse(struct audio_decoder *dec)
{
	const uint8_t riff_id[4] = { 'R', 'I', 'F', 'F' };
	const uint8_t wave_id[4] = { 'W', 'A', 'V', 'E' };
	const uint8_t fmt_id[4] = { 'f', 'm', 't', ' ' };
	const uint8_t data_id[4] = { 'd', 'a', 't', 'a' };
	uint8_t hdr[8];
	struct wav_fmt fmt;
	bool fmt_seen = false;
	uint32_t off = 12;
	int ret;

	ret = file_read_u32_at(dec->file, 0, hdr, 12);
	if (ret < 0) {
		return ret;
	}
	if (memcmp(hdr, riff_id, 4) != 0 || memcmp(&hdr[8], wave_id, 4) != 0) {
		return -EINVAL;
	}

	while (true) {
		ret = file_read_u32_at(dec->file, off, hdr, 8);
		if (ret < 0) {
			return ret;
		}

		uint32_t size = le32(&hdr[4]);
		off += 8;

		if (memcmp(hdr, fmt_id, 4) == 0 && size >= 16) {
			uint8_t fmtbuf[40];
			size_t len = MIN(size, sizeof(fmtbuf));

			ret = file_read_u32_at(dec->file, off, fmtbuf, len);
			if (ret < 0) {
				return ret;
			}

			fmt.format = le16(&fmtbuf[0]);
			fmt.channels = le16(&fmtbuf[2]);
			fmt.rate = le32(&fmtbuf[4]);
			fmt.bits = le16(&fmtbuf[14]);

			if (fmt.format == WAV_FMT_EXTENSIBLE) {
				if (size < 40 || le16(&fmtbuf[16]) < 22) {
					return -ENOTSUP;
				}
				fmt.format = le16(&fmtbuf[24]);
			}

			fmt_seen = true;
		} else if (memcmp(hdr, data_id, 4) == 0) {
			dec->data_size = size;
			break;
		}

		if (size > dec->file_size - off) {
			LOG_ERR("Corrupt WAV chunk size %u", size);
			return -EINVAL;
		}

		off += size + (size & 1);
	}

	if (!fmt_seen || dec->data_size == 0) {
		return -EINVAL;
	}

	if (fmt.format != WAV_FMT_PCM) {
		LOG_ERR("Unsupported WAV format 0x%04x", fmt.format);
		return -ENOTSUP;
	}
	if (fmt.channels < 1 || fmt.channels > 2) {
		LOG_ERR("Unsupported WAV channel count %u", fmt.channels);
		return -ENOTSUP;
	}
	if (fmt.bits != 8 && fmt.bits != 16 && fmt.bits != 24 && fmt.bits != 32) {
		LOG_ERR("Unsupported WAV bit depth %u", fmt.bits);
		return -ENOTSUP;
	}
	if (fmt.rate < 8000 || fmt.rate > 192000) {
		LOG_ERR("Unsupported WAV sample rate %u", fmt.rate);
		return -ENOTSUP;
	}

	dec->sample_rate = fmt.rate;
	dec->src_channels = fmt.channels;
	dec->src_bits = fmt.bits;
	dec->progress_den = dec->data_size;

	/* Position the file at the start of the data chunk payload */
	return fs_seek(dec->file, off, FS_SEEK_SET);
}

static void convert_samples(const struct audio_decoder *dec, const uint8_t *src,
			    size_t src_frames, int16_t *dst)
{
	uint8_t bytes_per = dec->src_bits / 8;
	size_t i;

	for (i = 0; i < src_frames; i++) {
		int16_t l, r;

		switch (dec->src_bits) {
		case 8:
			l = (int16_t)(((int)src[0] - 128) << 8);
			r = (dec->src_channels == 2)
				? (int16_t)(((int)src[1] - 128) << 8) : l;
			break;
		case 16:
			l = (int16_t)le16(&src[0]);
			r = (dec->src_channels == 2) ? (int16_t)le16(&src[2]) : l;
			break;
		case 24: {
			int32_t v = ((int8_t)src[2] << 24) | ((int32_t)src[1] << 16) |
				    ((int32_t)src[0] << 8);
			l = (int16_t)(v >> 16);
			if (dec->src_channels == 2) {
				v = ((int8_t)src[5] << 24) | ((int32_t)src[4] << 16) |
				    ((int32_t)src[3] << 8);
				r = (int16_t)(v >> 16);
			} else {
				r = l;
			}
			break;
		}
		default: {
			int32_t v = (int32_t)le32(&src[0]);
			l = (int16_t)(v >> 16);
			if (dec->src_channels == 2) {
				r = (int16_t)(((int32_t)le32(&src[4])) >> 16);
			} else {
				r = l;
			}
			break;
		}
		}

		*dst++ = l;
		*dst++ = r;
		src += bytes_per * dec->src_channels;
	}
}

static size_t wav_fill(struct audio_decoder *dec, int16_t *dst, size_t max_frames)
{
	uint8_t bytes_per_frame = dec->src_channels * (dec->src_bits / 8);
	size_t frames = 0;

	while (frames < max_frames && dec->data_read < dec->data_size) {
		size_t raw_want = MIN((max_frames - frames) * bytes_per_frame,
				      MIN((size_t)WAV_RAW_BUF_SIZE,
					  dec->data_size - dec->data_read));

		/* whole source frames only, drop the rounded-up tail */
		raw_want -= raw_want % bytes_per_frame;
		if (raw_want == 0) {
			break;
		}

		size_t got = 0;
		int ret;

		ret = file_read_exact(dec->file, dec->raw_buf, raw_want, &got);
		if (ret < 0) {
			LOG_ERR("WAV read failed: %d", ret);
			break;
		}
		if (got == 0) {
			break;
		}

		dec->data_read += got;
		dec->progress_num = dec->data_read;

		size_t src_frames = got / bytes_per_frame;

		convert_samples(dec, dec->raw_buf, src_frames, dst + frames * 2);
		frames += src_frames;
	}

	return frames;
}

static int mp3_refill(struct audio_decoder *dec)
{
	ssize_t n;
	int ret;

	if (dec->file_exhausted) {
		return 0;
	}

	/* compact first so a partially consumed ring frees up space */
	if (dec->ring_pos > 0) {
		memmove(dec->ring, dec->ring + dec->ring_pos, dec->ring_len - dec->ring_pos);
		dec->ring_len -= dec->ring_pos;
		dec->ring_pos = 0;
	}

	if (dec->ring_len == MP3_RING_SIZE) {
		return 0;
	}

	n = fs_read(dec->file, dec->ring + dec->ring_len, MP3_RING_SIZE - dec->ring_len);
	if (n < 0) {
		ret = (int)n;
		LOG_ERR("MP3 read failed: %d", ret);
		return ret;
	}
	if (n == 0) {
		dec->file_exhausted = true;
		return 0;
	}

	dec->file_read_total += (uint32_t)n;
	dec->ring_len += (uint32_t)n;

	return (int)n;
}

/* Margin so helix can always read a full frame header + side info
 * without running off the end of the buffer (it advances past them
 * before checking the main-data size).
 */
#define MP3_MIN_DECODE_BYTES	64

static bool mp3_decode_next(struct audio_decoder *dec)
{
	MP3FrameInfo info;

	while (true) {
		int bytes_left = (int)(dec->ring_len - dec->ring_pos);
		unsigned char *inptr;
		int consumed;
		int err;

		if (bytes_left < MP3_MIN_DECODE_BYTES) {
			if (mp3_refill(dec) > 0) {
				continue;
			}
			/* truncated tail: drop it */
			return false;
		}

		{
			int sync = MP3FindSyncWord(dec->ring + dec->ring_pos, bytes_left);

			if (sync < 0) {
				/* drop everything but a possible split sync prefix */
				dec->ring_pos = (dec->ring_len > 3) ?
						dec->ring_len - 3 : 0;
				if (mp3_refill(dec) > 0) {
					continue;
				}
				return false;
			}
			dec->ring_pos += (uint32_t)sync;
			bytes_left = (int)(dec->ring_len - dec->ring_pos);
		}

		if (bytes_left >= 4 &&
		    (dec->ring[dec->ring_pos + 2] & 0xf0) == 0) {
			/* free-format frame: helix mis-handles these when
			 * the next frame is not buffered; skip the header
			 */
			dec->ring_pos += 4;
			continue;
		}

		inptr = dec->ring + dec->ring_pos;
		int avail_before = bytes_left;
		err = MP3Decode(dec->mp3, &inptr, &bytes_left, dec->frame_pcm, 0);
		consumed = (int)(inptr - (dec->ring + dec->ring_pos));

		if (err == ERR_MP3_NONE) {
			MP3GetLastFrameInfo(dec->mp3, &info);
			if (info.nChans <= 0 || info.outputSamps <= 0 ||
			    info.nChans > 2) {
				/* unusable frame header info: skip and resync */
				if (consumed == 0) {
					dec->ring_pos++;
				}
				continue;
			}
			dec->src_channels = (uint8_t)info.nChans;
			if (!dec->synced) {
				dec->sample_rate = (uint32_t)info.samprate;
				dec->synced = true;
				LOG_INF("MP3: %d Hz, %d ch, layer %d, %d kbps",
					info.samprate, info.nChans, info.layer,
					info.bitrate / 1000);
			}
			dec->pending_frames =
				(uint32_t)info.outputSamps / (uint32_t)info.nChans;
			dec->pending_pos = 0;
			return true;
		}

		if (err == ERR_MP3_INDATA_UNDERFLOW && !dec->file_exhausted) {
			/* helix consumed the header/side info before finding
			 * the main data short; rewind and retry with a full
			 * ring so the bit reservoir can be satisfied
			 */
			dec->ring_pos -= (uint32_t)MIN(consumed, avail_before);
			if (mp3_refill(dec) > 0) {
				continue;
			}
			continue;
		}

		if (err == ERR_MP3_INDATA_UNDERFLOW && dec->file_exhausted) {
			return false;
		}

		/* Bad frame: make sure we always make progress */
		consumed = MIN(consumed, avail_before);
		dec->ring_pos += (consumed > 0) ? (uint32_t)consumed : 1;
	}
}

static size_t mp3_fill(struct audio_decoder *dec, int16_t *dst, size_t max_frames)
{
	size_t frames = 0;

	dec->progress_num = dec->file_read_total - (dec->ring_len - dec->ring_pos);

	while (frames < max_frames) {
		if (dec->pending_pos < dec->pending_frames) {
			size_t count = MIN(dec->pending_frames - dec->pending_pos,
					   max_frames - frames);
			size_t i;

			if (dec->src_channels == 1) {
				int16_t s = dec->frame_pcm[dec->pending_pos];

				for (i = 0; i < count; i++) {
					*dst++ = s;
					*dst++ = s;
				}
			} else {
				const int16_t *src = dec->frame_pcm + dec->pending_pos * 2;

				for (i = 0; i < count; i++) {
					*dst++ = *src++;
					*dst++ = *src++;
				}
			}
			dec->pending_pos += count;
			frames += count;
			continue;
		}

		if (!mp3_decode_next(dec)) {
			break;
		}
	}

	return frames;
}

static int mp3_parse(struct audio_decoder *dec)
{
	uint8_t hdr[10];
	ssize_t n;
	int ret;

	dec->mp3 = MP3InitDecoder();
	if (dec->mp3 == NULL) {
		LOG_ERR("Failed to allocate MP3 decoder");
		return -ENOMEM;
	}

	n = fs_read(dec->file, hdr, sizeof(hdr));
	if (n < 0) {
		return (int)n;
	}

	if (n >= 10 && memcmp(hdr, "ID3", 3) == 0) {
		uint32_t tag_size = ((uint32_t)(hdr[6] & 0x7f) << 21) |
				    ((uint32_t)(hdr[7] & 0x7f) << 14) |
				    ((uint32_t)(hdr[8] & 0x7f) << 7) |
				    ((uint32_t)(hdr[9] & 0x7f));
		uint32_t skip = 10 + tag_size + ((hdr[5] & 0x10) ? 10 : 0);

		ret = fs_seek(dec->file, skip, FS_SEEK_SET);
		if (ret < 0) {
			return ret;
		}
	} else if (n > 0) {
		ret = fs_seek(dec->file, 0, FS_SEEK_SET);
		if (ret < 0) {
			return ret;
		}
	}

	dec->progress_den = dec->file_size;
	dec->sample_rate = 0;

	/* decode the first frame up front so the caller learns the
	 * sample rate before configuring the stream
	 */
	if (!mp3_decode_next(dec)) {
		LOG_ERR("No MP3 frames found");
		MP3FreeDecoder(dec->mp3);
		dec->mp3 = NULL;
		return -ENODATA;
	}

	return 0;
}

int decoder_open(struct audio_decoder *dec, struct fs_file_t *file, uint32_t file_size)
{
	uint8_t sniff[12];
	ssize_t n;
	int ret;

	memset(dec, 0, sizeof(*dec));
	dec->file = file;
	dec->file_size = file_size;

	n = fs_read(file, sniff, sizeof(sniff));
	if (n < 0) {
		return (int)n;
	}
	ret = fs_seek(file, 0, FS_SEEK_SET);
	if (ret < 0) {
		return ret;
	}

	if (n == 12 && memcmp(sniff, "RIFF", 4) == 0 && memcmp(&sniff[8], "WAVE", 4) == 0) {
		dec->format = AUDIO_FILE_WAV;
		dec->raw_buf = wav_raw;
		ret = wav_parse(dec);
	} else {
		dec->format = AUDIO_FILE_MP3;
		dec->ring = mp3_ring;
		dec->frame_pcm = mp3_frame_pcm;
		ret = mp3_parse(dec);
	}

	if (ret < 0) {
		if (dec->mp3) {
			MP3FreeDecoder(dec->mp3);
			dec->mp3 = NULL;
		}
		return ret;
	}

	dec->opened = true;
	return 0;
}

void decoder_close(struct audio_decoder *dec)
{
	if (!dec->opened) {
		return;
	}

	if (dec->mp3) {
		MP3FreeDecoder(dec->mp3);
	}
	memset(dec, 0, sizeof(*dec));
}

size_t decoder_fill(struct audio_decoder *dec, int16_t *dst, size_t max_frames)
{
	if (!dec->opened || max_frames == 0) {
		return 0;
	}

	if (dec->format == AUDIO_FILE_WAV) {
		return wav_fill(dec, dst, max_frames);
	}
	return mp3_fill(dec, dst, max_frames);
}
