/*
 * Minimal FLAC decoder (subset of the format needed for playback):
 * - STREAMINFO parsing, frame sample rate/depth handling
 * - constant / verbatim / fixed-LPC / generic-LPC subframes
 * - partitioned rice residuals (4-bit and 5-bit parameters)
 * - independent, left/side, right/side and mid/side channel modes
 * - output: interleaved stereo 16-bit PCM, sample counts from the
 *   frame headers feed the elapsed-time tracking
 *
 * CRC-8/CRC-16 frame checksums are consumed but not verified.
 * Not supported (rejected cleanly): >2 channels, bit depths > 24,
 * block sizes > 8192. Corrupt frames trigger a bounded sync resync;
 * the stream ends when the resync budget (failure cap or EOF) is
 * exhausted.
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/fs/fs.h>
#include <string.h>
#include <stdint.h>

#include "flac.h"

LOG_MODULE_REGISTER(flacdec, LOG_LEVEL_INF);

/* Covers ffmpeg (4608) and flac CLI (4096) defaults plus 96 kHz
 * encodes (8192). Files with larger blocks are rejected and skipped.
 */
#define FLAC_MAX_BLOCK_SIZE 8192
#define FLAC_IO_BUF_SIZE 4096

/* static state: the app runs a single decoder instance */
static struct flac_stream {
	struct audio_decoder *dec;

	/* buffered byte input */
	uint8_t io_buf[FLAC_IO_BUF_SIZE];
	uint32_t io_len;
	uint32_t io_pos;
	bool io_eof;

	/* stream info */
	uint32_t sample_rate;
	uint8_t channels;
	uint8_t bps;

	/* decoded block */
	int32_t ch0[FLAC_MAX_BLOCK_SIZE];
	int32_t ch1[FLAC_MAX_BLOCK_SIZE];
	uint32_t block_samples;
	uint32_t out_pos;
	uint8_t resync_fails;
	uint8_t chan_mode;   /* decoded frame channel assignment */
	uint8_t frame_bps;   /* sample depth of the current frame */
} flac;

enum chan_mode {
	CHAN_INDEPENDENT = 0,
	CHAN_LEFT_SIDE,
	CHAN_RIGHT_SIDE,
	CHAN_MID_SIDE,
};

/* ------------------------------------------------------------------ */
/* Bit reader (FLAC is MSB-first)                                      */
/* ------------------------------------------------------------------ */

struct br {
	uint64_t acc;
	int bits;
	bool error;
};

static void io_fill(struct flac_stream *s)
{
	ssize_t n;

	if (s->io_pos < s->io_len || s->io_eof)
	{
		return;
	}

	n = fs_read(s->dec->file, s->io_buf, sizeof(s->io_buf));
	if (n <= 0)
	{
		s->io_eof = true;
		return;
	}
	s->io_len = (uint32_t)n;
	s->io_pos = 0;
}

static uint8_t next_byte(struct flac_stream *s, bool *err)
{
	io_fill(s);
	if (s->io_pos >= s->io_len)
	{
		*err = true;
		return 0;
	}
	return s->io_buf[s->io_pos++];
}

static uint32_t read_bits(struct br *b, int n)
{
	uint32_t v;

	if (n == 0)
	{
		return 0;
	}
	if (n > 32 || b->error)
	{
		b->error = true;
		return 0;
	}

	while (b->bits < n)
	{
		uint8_t byte = next_byte(&flac, &b->error);

		if (b->error)
		{
			return 0;
		}
		b->acc = (b->acc << 8) | byte;
		b->bits += 8;
	}

	v = (uint32_t)(b->acc >> (b->bits - n)) & ((n == 32) ? 0xffffffffu : ((1u << n) - 1u));
	b->bits -= n;
	return v;
}

/* n-bit signed value (two's complement in n bits) */
static int32_t read_bits_signed(struct br *b, int n)
{
	uint32_t v = read_bits(b, n);

	if (n > 0 && n < 32 && (v & (1u << (n - 1))))
	{
		v |= ~((1u << n) - 1u);
	}
	return (int32_t)v;
}

static uint32_t read_bit(struct br *b)
{
	return read_bits(b, 1);
}

/* unary-coded value: number of zeros before the next 1 bit */
static uint32_t read_unary(struct br *b)
{
	uint32_t count = 0;

	while (count < (1u << 20))
	{
		if (b->error || read_bit(b))
		{
			return count;
		}
		count++;
	}
	b->error = true;
	return 0;
}

static void br_align(struct br *b)
{
	int rem = b->bits & 7;

	b->bits -= rem;
}

/* read n bytes through the buffered io layer (keeps file position
 * accounting consistent with skip_bytes)
 */
static bool read_bytes_io(struct flac_stream *s, uint8_t *buf, uint32_t n)
{
	for (uint32_t i = 0; i < n; i++)
	{
		bool err = false;

		buf[i] = next_byte(s, &err);
		if (err)
		{
			return false;
		}
	}
	return true;
}

/* skip n bytes, draining the buffer first then seeking the file */
static void skip_bytes(struct flac_stream *s, uint32_t n)
{
	uint32_t buffered = s->io_len - s->io_pos;

	if (buffered >= n)
	{
		s->io_pos += n;
		return;
	}

	n -= buffered;
	s->io_pos = s->io_len;
	if (n > 0)
	{
		(void)fs_seek(s->dec->file, (off_t)n, FS_SEEK_CUR);
	}
}

/* ------------------------------------------------------------------ */
/* Subframe decoding                                                   */
/* ------------------------------------------------------------------ */

/* fixed predictor coefficients, order 1..4 */
static const int32_t fixed_coefs[4][4] = {
	{ 1, 0, 0, 0 },
	{ 2, -1, 0, 0 },
	{ 3, -3, 1, 0 },
	{ 4, -6, 4, -1 },
};

static int32_t rice_value(struct br *b, uint8_t param)
{
	uint32_t q = read_unary(b);
	uint32_t r = (param > 0) ? read_bits(b, param) : 0;
	uint32_t v;

	if (b->error || q > (uint32_t)(INT32_MAX >> param))
	{
		b->error = true;
		return 0;
	}

	v = (q << param) | r;
	return (v & 1) ? -(int32_t)((v >> 1) + 1) : (int32_t)(v >> 1);
}

static bool decode_residuals(struct br *b, int32_t *dest, uint32_t n, uint32_t warmup)
{
	uint32_t method = read_bits(b, 2);
	int param_bits;
	uint32_t escape;
	uint32_t porder;
	uint32_t partitions;
	uint32_t psize;
	uint32_t idx;

	if (method >= 2)
	{
		return false;
	}
	param_bits = (method == 0) ? 4 : 5;
	escape = (1u << param_bits) - 1u;

	porder = read_bits(b, 4);
	partitions = 1u << porder;
	if (n % partitions != 0 || partitions > n)
	{
		return false;
	}
	psize = n / partitions;
	/* spec: (block size >> partition order) must be larger than the
	 * predictor order
	 */
	if (psize <= warmup)
	{
		return false;
	}

	idx = warmup;
	for (uint32_t p = 0; p < partitions; p++)
	{
		uint32_t count = psize - (p == 0 ? warmup : 0);
		uint32_t param = read_bits(b, param_bits);

		if (param == escape)
		{
			uint32_t bits = read_bits(b, 5);

			for (uint32_t i = 0; i < count; i++)
			{
				dest[idx++] = (bits == 0) ? 0 : read_bits_signed(b, (int)bits);
			}
		}
		else
		{
			for (uint32_t i = 0; i < count; i++)
			{
				dest[idx++] = rice_value(b, (uint8_t)param);
			}
		}

		if (b->error)
		{
			return false;
		}
	}

	return true;
}

static bool decode_subframe(struct br *b, int32_t *out, uint32_t n, int bps)
{
	uint32_t type;
	uint32_t order;
	uint32_t wasted = 0;

	if (b->error || bps < 1)
	{
		return false;
	}

	/* subframe header: 1 zero pad bit, 6 type bits, wasted flag */
	if (read_bit(b) != 0)
	{
		return false;
	}
	type = read_bits(b, 6);

	if (b->error)
	{
		return false;
	}

	if (read_bit(b))
	{
		wasted = read_unary(b) + 1;
		if (wasted > 32)
		{
			return false;
		}
		bps -= (int)wasted;
		if (bps < 1)
		{
			return false; /* spec: resulting depth must stay > 0 */
		}
	}

	if (type == 0)
	{
		/* constant */
		int32_t v = read_bits_signed(b, bps);

		for (uint32_t i = 0; i < n; i++)
		{
			out[i] = v;
		}
	}
	else if (type == 1)
	{
		/* verbatim */
		for (uint32_t i = 0; i < n; i++)
		{
			out[i] = read_bits_signed(b, bps);
		}
	}
	else if ((type & 0x38) == 0x08 && (type & 0x07) <= 4)
	{
		/* fixed LPC, order 0..4 */
		order = type & 0x07;

		for (uint32_t i = 0; i < order; i++)
		{
			out[i] = read_bits_signed(b, bps);
		}
		if (!decode_residuals(b, out, n, order))
		{
			return false;
		}
		for (uint32_t i = order; i < n; i++)
		{
			int64_t sum = 0;

			for (uint32_t j = 0; j < order; j++)
			{
				sum += (int64_t)fixed_coefs[order - 1][j] * out[i - 1 - j];
			}
			out[i] += (int32_t)sum;
		}
	}
	else if ((type & 0x20) != 0)
	{
		/* generic LPC: warm-up samples first, then precision, shift
		 * and coefficients (RFC 9639 section 9.2.6)
		 */
		order = (type & 0x1f) + 1;
		int32_t coefs[32];

		for (uint32_t i = 0; i < order; i++)
		{
			out[i] = read_bits_signed(b, bps);
		}
		uint32_t prec = read_bits(b, 4) + 1;
		int shift = read_bits_signed(b, 5);

		if (b->error || prec == 16 || shift < 0)
		{
			return false; /* 0b1111 precision and negative shifts are forbidden */
		}

		for (uint32_t j = 0; j < order; j++)
		{
			coefs[j] = read_bits_signed(b, (int)prec);
		}
		if (!decode_residuals(b, out, n, order))
		{
			return false;
		}
		for (uint32_t i = order; i < n; i++)
		{
			int64_t sum = 0;

			for (uint32_t j = 0; j < order; j++)
			{
				sum += (int64_t)coefs[j] * out[i - 1 - j];
			}
			out[i] += (int32_t)(sum >> shift);
		}
	}
	else
	{
		return false;
	}

	if (b->error)
	{
		return false;
	}

	if (wasted > 0)
	{
		for (uint32_t i = 0; i < n; i++)
		{
			out[i] <<= wasted;
		}
	}

	return true;
}

/* ------------------------------------------------------------------ */
/* Frame decoding                                                      */
/* ------------------------------------------------------------------ */

/* UTF-8 coded frame/sample number (1..7 bytes); the value itself is
 * not needed for sequential playback, only its length matters
 */
static bool read_utf8_coded(struct br *b)
{
	uint8_t first = (uint8_t)read_bits(b, 8);
	int extra;

	if (b->error)
	{
		return false;
	}

	if (first < 0x80)
	{
		return true;
	}
	else if ((first & 0xe0) == 0xc0)
	{
		extra = 1;
	}
	else if ((first & 0xf0) == 0xe0)
	{
		extra = 2;
	}
	else if ((first & 0xf8) == 0xf0)
	{
		extra = 3;
	}
	else if ((first & 0xfc) == 0xf8)
	{
		extra = 4;
	}
	else if ((first & 0xfe) == 0xfc)
	{
		extra = 5;
	}
	else if (first == 0xfe)
	{
		extra = 6;
	}
	else
	{
		return false;
	}

	for (int i = 0; i < extra; i++)
	{
		(void)read_bits(b, 8);
		if (b->error)
		{
			return false;
		}
	}
	return true;
}

static bool flac_decode_frame(struct flac_stream *s)
{
	struct br b = { 0 };
	uint32_t blocksize = 0;
	uint8_t bs_code, sr_code, ch_code, bps_code;
	uint8_t mode = CHAN_INDEPENDENT;
	int frame_bps;

	if (read_bits(&b, 14) != 0x3ffe)
	{
		return false;
	}
	(void)read_bit(&b); /* reserved */
	(void)read_bit(&b); /* blocking strategy */

	bs_code = (uint8_t)read_bits(&b, 4);
	sr_code = (uint8_t)read_bits(&b, 4);
	ch_code = (uint8_t)read_bits(&b, 4);
	bps_code = (uint8_t)read_bits(&b, 3);
	(void)read_bit(&b); /* reserved */

	if (b.error)
	{
		return false;
	}

	switch (bs_code)
	{
	case 0:
		return false;
	case 1:
		blocksize = 192;
		break;
	case 2:
	case 3:
	case 4:
	case 5:
		blocksize = 576u << (bs_code - 2);
		break;
	case 6:
		/* 8 bit (blocksize - 1) from the end of the header */
		break;
	case 7:
		/* 16 bit (blocksize - 1) from the end of the header */
		break;
	default:
		blocksize = 256u << (bs_code - 8);
		break;
	}

	switch (sr_code)
	{
	case 15:
		return false; /* invalid */
	case 0:
		break; /* streaminfo */
	case 12:
	case 13:
	case 14:
		break; /* coded, parsed below */
	default:
		break; /* fixed-rate codes: keep the STREAMINFO rate */
	}

	switch (ch_code)
	{
	case 0:
	case 1:
		mode = CHAN_INDEPENDENT;
		s->channels = (uint8_t)(ch_code + 1);
		break;
	case 2:
		mode = CHAN_INDEPENDENT;
		s->channels = 3; /* >2 channel files: reject via channels check below */
		break;
	case 8:
		mode = CHAN_LEFT_SIDE;
		s->channels = 2;
		break;
	case 9:
		mode = CHAN_RIGHT_SIDE;
		s->channels = 2;
		break;
	case 10:
		mode = CHAN_MID_SIDE;
		s->channels = 2;
		break;
	default:
		return false;
	}

	switch (bps_code)
	{
	case 0:
		frame_bps = s->bps;
		break;
	case 1:
		frame_bps = 8;
		break;
	case 2:
		frame_bps = 12;
		break;
	case 4:
		frame_bps = 16;
		break;
	case 5:
		frame_bps = 20;
		break;
	case 6:
		frame_bps = 24;
		break;
	case 7:
		frame_bps = 32;
		break;
	default:
		return false;
	}

	if (frame_bps > 24 || s->channels > 2)
	{
		return false;
	}

	if (!read_utf8_coded(&b))
	{
		return false;
	}

	if (bs_code == 6)
	{
		blocksize = read_bits(&b, 8) + 1;
	}
	else if (bs_code == 7)
	{
		blocksize = read_bits(&b, 16) + 1;
	}

	if (sr_code == 12)
	{
		if (read_bits(&b, 8) == 0)
		{
			return false;
		}
	}
	else if (sr_code == 13 || sr_code == 14)
	{
		if (read_bits(&b, 16) == 0)
		{
			return false;
		}
	}
	/* a coded sample rate disagreeing with STREAMINFO is rare; the
	 * stream keeps playing at the STREAMINFO rate
	 */

	(void)read_bits(&b, 8); /* CRC-8, not verified */
	if (b.error || blocksize == 0 || blocksize > FLAC_MAX_BLOCK_SIZE)
	{
		return false;
	}

	/* subframes */
	if (s->channels == 1)
	{
		if (!decode_subframe(&b, s->ch0, blocksize, frame_bps))
		{
			return false;
		}
	}
	else if (mode == CHAN_LEFT_SIDE)
	{
		/* ch0 = left, ch1 = side (side carries one extra bit) */
		if (!decode_subframe(&b, s->ch0, blocksize, frame_bps) ||
			!decode_subframe(&b, s->ch1, blocksize, frame_bps + 1))
		{
			return false;
		}
	}
	else if (mode == CHAN_RIGHT_SIDE)
	{
		/* ch0 = side, ch1 = right */
		if (!decode_subframe(&b, s->ch0, blocksize, frame_bps + 1) ||
			!decode_subframe(&b, s->ch1, blocksize, frame_bps))
		{
			return false;
		}
	}
	else if (mode == CHAN_MID_SIDE)
	{
		/* mid has stream depth, side one extra bit */
		if (!decode_subframe(&b, s->ch0, blocksize, frame_bps) ||
			!decode_subframe(&b, s->ch1, blocksize, frame_bps + 1))
		{
			return false;
		}
	}
	else
	{
		if (!decode_subframe(&b, s->ch0, blocksize, frame_bps) ||
			!decode_subframe(&b, s->ch1, blocksize, frame_bps))
		{
			return false;
		}
	}

	(void)read_bits(&b, 16); /* CRC-16, not verified */
	if (b.error)
	{
		return false;
	}
	br_align(&b);

	s->block_samples = blocksize;
	s->out_pos = 0;
	s->chan_mode = mode;
	s->frame_bps = (uint8_t)frame_bps;

	/* elapsed time from decoded sample counts */
	flac.dec->elapsed_frames += blocksize;
	if (s->sample_rate > 0)
	{
		flac.dec->elapsed_ms =
			(uint32_t)((uint64_t)flac.dec->elapsed_frames * 1000u / s->sample_rate);
	}

	return true;
}

/* ------------------------------------------------------------------ */
/* Output conversion                                                   */
/* ------------------------------------------------------------------ */

static inline int16_t to16(int32_t v, int bps)
{
	int32_t out;

	if (bps > 16)
	{
		out = v >> (bps - 16);
	}
	else
	{
		out = v << (16 - bps);
	}

	if (out > INT16_MAX)
	{
		return INT16_MAX;
	}
	if (out < INT16_MIN)
	{
		return INT16_MIN;
	}
	return (int16_t)out;
}

/* Scan forward for the next frame sync (0xFF F8/F9, bounded) after a
 * corrupt frame so one bad spot does not truncate the whole track
 */
#define FLAC_RESYNC_LIMIT (1024u * 1024u)
#define FLAC_RESYNC_MAX_FAILS 32

static bool flac_resync(struct flac_stream *s)
{
	uint32_t scanned = 0;
	uint8_t prev = 0;

	while (scanned < FLAC_RESYNC_LIMIT)
	{
		bool err = false;
		uint8_t byte = next_byte(s, &err);

		if (err)
		{
			return false;
		}
		scanned++;
		if (prev == 0xff && (byte & 0xfe) == 0xf8)
		{
			/* candidate sync: rewind so the full header is
			 * re-parsed by the caller. prev and byte may sit
			 * in different buffer views: the FS position sits
			 * at refill_end regardless of io_pos, and prev is
			 * 2 bytes before the candidate byte (which is at
			 * refill_start + io_pos - 1), so the distance is
			 * io_len - io_pos + 2.
			 */
			if (s->io_pos >= 2)
			{
				s->io_pos -= 2;
			}
			else
			{
				(void)fs_seek(s->dec->file,
					-(off_t)(s->io_len - s->io_pos + 2), FS_SEEK_CUR);
				s->io_pos = 0;
				s->io_len = 0;
			}
			return true;
		}
		prev = byte;
	}

	return false;
}

size_t flac_fill(struct audio_decoder *dec, int16_t *dst, size_t max_frames)
{
	struct flac_stream *s = &flac;
	size_t frames = 0;

	while (frames < max_frames)
	{
		uint32_t count;

		if (s->out_pos >= s->block_samples)
		{
			if (!flac_decode_frame(s))
			{
				/* corrupt spot: resync and retry a bounded
				 * number of times, else end the stream
				 */
				s->block_samples = 0;
				s->out_pos = 0;
				if (++s->resync_fails > FLAC_RESYNC_MAX_FAILS || !flac_resync(s))
				{
					s->resync_fails = 0;
					break;
				}
				continue;
			}
			s->resync_fails = 0;
		}

		count = MIN(s->block_samples - s->out_pos, (uint32_t)(max_frames - frames));

		for (uint32_t i = 0; i < count; i++)
		{
			uint32_t j = s->out_pos + i;
			int32_t l;
			int32_t r;

			if (s->chan_mode == CHAN_LEFT_SIDE)
			{
				l = s->ch0[j];
				r = s->ch0[j] - s->ch1[j];
			}
			else if (s->chan_mode == CHAN_RIGHT_SIDE)
			{
				r = s->ch1[j];
				l = s->ch0[j] + s->ch1[j];
			}
			else if (s->chan_mode == CHAN_MID_SIDE)
			{
				int32_t mid2 = (s->ch0[j] << 1) | (s->ch1[j] & 1);

				l = (mid2 + s->ch1[j]) >> 1;
				r = (mid2 - s->ch1[j]) >> 1;
			}
			else if (s->channels == 1)
			{
				l = r = s->ch0[j];
			}
			else
			{
				l = s->ch0[j];
				r = s->ch1[j];
			}

			*dst++ = to16(l, s->frame_bps);
			*dst++ = to16(r, s->frame_bps);
		}

		s->out_pos += count;
		frames += count;
	}

	return frames;
}

/* ------------------------------------------------------------------ */
/* Metadata parsing                                                    */
/* ------------------------------------------------------------------ */

/* STREAMINFO block, 34 bytes:
 * 16 min block | 16 max block | 24 min frame | 24 max frame |
 * 20 rate | 3 channels-1 | 5 bps-1 | 36 total samples
 */
static bool streaminfo_from_bytes(struct audio_decoder *dec, const uint8_t *p)
{
	uint32_t min_block = ((uint32_t)p[0] << 8) | p[1];
	uint32_t max_block = ((uint32_t)p[2] << 8) | p[3];
	uint32_t sample_rate = ((uint32_t)p[10] << 12) | ((uint32_t)p[11] << 4) |
		((uint32_t)p[12] >> 4);
	uint32_t channels = (((uint32_t)p[12] >> 1) & 7u) + 1u;
	uint32_t bps = ((((uint32_t)p[12] & 1u) << 4) | ((uint32_t)p[13] >> 4)) + 1u;
	uint64_t total = ((uint64_t)(p[13] & 0x0f) << 32) |
		((uint64_t)p[14] << 24) | ((uint64_t)p[15] << 16) |
		((uint64_t)p[16] << 8) | p[17];

	(void)min_block;
	if (min_block == 0 || max_block == 0 || max_block > FLAC_MAX_BLOCK_SIZE ||
		channels > 2 || bps > 24 || sample_rate == 0 || sample_rate > 192000)
	{
		/* sample rates beyond what the codec/I2S link can carry
		 * are treated as unsupported
		 */
		return false;
	}

	flac.sample_rate = sample_rate;
	flac.channels = (uint8_t)channels;
	flac.bps = (uint8_t)bps;

	dec->sample_rate = sample_rate;
	dec->src_channels = (uint8_t)channels;
	if (total > 0)
	{
		dec->total_ms = (uint32_t)((total * 1000u) / sample_rate);
	}

	return true;
}

int flac_parse(struct audio_decoder *dec)
{
	uint8_t magic[4];
	bool have_streaminfo = false;

	memset(&flac, 0, sizeof(flac));
	flac.dec = dec;

	if (!read_bytes_io(&flac, magic, sizeof(magic)) ||
		memcmp(magic, "fLaC", 4) != 0)
	{
		return -EINVAL;
	}

	/* metadata blocks; all reads go through the io layer so the
	 * leftover buffered bytes flow into frame decoding
	 */
	while (true)
	{
		uint8_t hdr[4];
		bool last;
		uint8_t type;
		uint32_t len;

		if (!read_bytes_io(&flac, hdr, sizeof(hdr)))
		{
			return -EIO;
		}

		last = (hdr[0] & 0x80) != 0;
		type = hdr[0] & 0x7f;
		len = ((uint32_t)(hdr[1]) << 16) | ((uint32_t)(hdr[2]) << 8) | hdr[3];

		if (type == 0)
		{
			/* STREAMINFO */
			uint8_t si[34];

			if (len < sizeof(si) || !read_bytes_io(&flac, si, sizeof(si)))
			{
				return -EINVAL;
			}
			if (!streaminfo_from_bytes(dec, si))
			{
				return -EINVAL;
			}
			have_streaminfo = true;
			skip_bytes(&flac, len - sizeof(si));
		}
		else
		{
			skip_bytes(&flac, len);
		}

		if (last)
		{
			break;
		}
	}

	if (!have_streaminfo)
	{
		return -EINVAL;
	}

	return 0;
}
