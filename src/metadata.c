#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/fs/fs.h>
#include <string.h>
#include <strings.h>

#include "metadata.h"
#include "sd_card.h"

LOG_MODULE_REGISTER(METADATA, LOG_LEVEL_INF);

#define ID3_HEADER_SIZE 10
#define ID3_V2_FRAME_HEADER_SIZE 10
#define ID3_V2_2_FRAME_HEADER_SIZE 6
#define ID3V1_SIZE 128
#define MAX_TEXT_FRAME_SIZE 512
/* APIC head probe: encoding + mime + picture type + description; long
 * descriptions need room, but this stays far below the image data
 */
#define APIC_HEAD_PROBE 256
/* Cap art frame size: typical embedded covers are well below this */
#define MAX_ART_FRAME_SIZE (2 * 1024 * 1024)

static bool read_exact(struct fs_file_t *file, uint8_t *buf, size_t len)
{
	ssize_t got = fs_read(file, buf, len);

	return got == (ssize_t)len;
}

static bool skip_to(struct fs_file_t *file, off_t pos)
{
	return fs_seek(file, pos, FS_SEEK_SET) == 0;
}

static uint32_t be32(const uint8_t *p)
{
	return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
		((uint32_t)p[2] << 8) | p[3];
}

static uint32_t be24(const uint8_t *p)
{
	return ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | p[2];
}

static uint32_t syncsafe32(const uint8_t *p)
{
	return ((uint32_t)(p[0] & 0x7f) << 21) | ((uint32_t)(p[1] & 0x7f) << 14) |
		((uint32_t)(p[2] & 0x7f) << 7) | (uint32_t)(p[3] & 0x7f);
}

/* Parse the ID3v2 tag header and skip any extended header.
 * Returns the file position where frame parsing starts, or -1 when
 * the file has no parseable tag (missing ID3 or global
 * unsynchronisation, whose data would need de-unsyncing).
 */
static off_t id3v2_tag_start(struct fs_file_t *file, uint8_t *ver_major, off_t *tag_end)
{
	uint8_t header[ID3_HEADER_SIZE];
	off_t pos;

	if (!read_exact(file, header, sizeof(header)) ||
		memcmp(header, "ID3", 3) != 0 || (header[5] & 0x80))
	{
		return -1;
	}

	*ver_major = header[3];
	*tag_end = ID3_HEADER_SIZE + syncsafe32(header + 6);
	pos = ID3_HEADER_SIZE;

	/* Extended header (v2.3/v2.4). The v2.4 size field covers the
	 * whole extended header including itself; v2.3 excludes it
	 */
	if (*ver_major >= 3 && (header[5] & 0x40) && *tag_end - pos >= 4)
	{
		uint8_t ext[4];

		if (read_exact(file, ext, sizeof(ext)))
		{
			uint32_t ext_size = (*ver_major == 4) ? syncsafe32(ext) : be32(ext);

			if (*ver_major == 4)
			{
				ext_size = (ext_size >= 4) ? ext_size - 4 : 0;
			}
			pos += 4 + ext_size;
		}
	}

	return pos;
}

/* Read the next frame header. Returns the content length and leaves
 * the file positioned at the frame content (*content_pos).
 * Returns 0 at end of tag / malformed data, or -1 when the frame was
 * skipped (file repositioned past it): frame-level unsynchronisation
 * (v2.4) is not de-unsynced by this parser.
 */
static off_t id3v2_next_frame(struct fs_file_t *file, uint8_t ver_major,
	off_t tag_end, char id[5], off_t *content_pos)
{
	uint8_t fh[ID3_V2_FRAME_HEADER_SIZE];
	uint8_t hsize = (ver_major == 2) ? ID3_V2_2_FRAME_HEADER_SIZE
					 : ID3_V2_FRAME_HEADER_SIZE;
	uint32_t size;
	off_t pos = fs_tell(file);

	if (pos + (off_t)hsize > tag_end || !read_exact(file, fh, hsize))
	{
		return 0;
	}

	if (fh[0] < 'A' || fh[0] > 'Z')
	{
		return 0; /* padding or garbage: stop scanning */
	}

	if (ver_major == 2)
	{
		id[0] = fh[0];
		id[1] = fh[1];
		id[2] = fh[2];
		id[3] = '\0';
		size = be24(fh + 3);
	}
	else
	{
		memcpy(id, fh, 4);
		id[4] = '\0';
		size = (ver_major == 4) ? syncsafe32(fh + 4) : be32(fh + 4);
	}

	if (size > (uint32_t)(tag_end - pos - hsize))
	{
		return 0;
	}

	*content_pos = pos + hsize;

	/* Frame flags (byte fh[9], v2.3/v2.4 only): frames flagged as
	 * compressed or encrypted cannot be parsed as-is; grouped
	 * frames carry one extra leading byte that must be skipped
	 */
	if (ver_major == 4)
	{
		if (fh[9] & 0x02)
		{
			/* frame-level unsynchronisation: data would need
			 * de-unsyncing, which we do not implement
			 */
			(void)skip_to(file, pos + hsize + size);
			return -1;
		}
		if (fh[9] & 0x0C)
		{
			(void)skip_to(file, pos + hsize + size);
			return -1;
		}
		if (fh[9] & 0x40)
		{
			if (size < 1)
			{
				return 0;
			}
			*content_pos += 1;
			size -= 1;
		}
		if (fh[9] & 0x01)
		{
			/* data length indicator prepended to the content */
			if (size < 4)
			{
				return 0;
			}
			*content_pos += 4;
			size -= 4;
		}
	}
	else if (ver_major == 3)
	{
		if (fh[9] & 0xC0)
		{
			(void)skip_to(file, pos + hsize + size);
			return -1;
		}
		if (fh[9] & 0x20)
		{
			if (size < 1)
			{
				return 0;
			}
			*content_pos += 1;
			size -= 1;
		}
	}

	return (off_t)size;
}

/* Copy latin1/utf8 text, dropping trailing padding */
static void copy_latin(const uint8_t *src, size_t len, char *dst, size_t dst_len)
{
	size_t i;

	dst[0] = '\0';
	if (dst_len == 0)
	{
		return;
	}

	for (i = 0; i < len && i + 1 < dst_len; i++)
	{
		if (src[i] == '\0')
		{
			break;
		}
		/* montserrat has no glyphs above ASCII: map to '_' */
		dst[i] = (src[i] < 0x80) ? (char)src[i] : '_';
	}
	dst[i] = '\0';
}

/* Convert UTF-16 (BOM aware, defaults to big endian without BOM) to
 * ASCII-ish by dropping the high byte; non-ASCII collapses to '_'.
 * Good enough for a 200x200 e-ink screen.
 */
static void copy_utf16(const uint8_t *src, size_t len, char *dst, size_t dst_len)
{
	bool big_endian = true;
	size_t i = 0;
	size_t o = 0;

	dst[0] = '\0';
	if (dst_len == 0)
	{
		return;
	}

	if (len >= 2)
	{
		if (src[0] == 0xff && src[1] == 0xfe)
		{
			big_endian = false;
			i = 2;
		}
		else if (src[0] == 0xfe && src[1] == 0xff)
		{
			big_endian = true;
			i = 2;
		}
	}

	while (i + 1 < len && o + 1 < dst_len)
	{
		uint16_t ch = big_endian ? ((uint16_t)src[i] << 8) | src[i + 1]
					 : ((uint16_t)src[i + 1] << 8) | src[i];

		i += 2;
		if (ch == 0)
		{
			break;
		}
		dst[o++] = (ch < 0x80) ? (char)ch : '_';
	}
	dst[o] = '\0';
}

static void decode_text_frame(const uint8_t *data, size_t len, char *dst, size_t dst_len)
{
	if (len == 0)
	{
		dst[0] = '\0';
		return;
	}

	switch (data[0])
	{
	case 1: /* UTF-16 with BOM */
	case 2: /* UTF-16BE, no BOM */
		copy_utf16(data + 1, len - 1, dst, dst_len);
		break;
	case 3: /* UTF-8 */
	default: /* ISO-8859-1 */
		copy_latin(data + 1, len - 1, dst, dst_len);
		break;
	}
}

/* Scan ID3v2 text frames. Returns true if a tag was present. */
static bool id3v2_scan_text(const char *path, char *title, size_t title_len,
	char *artist, size_t artist_len)
{
	struct fs_file_t file;
	uint8_t ver_major;
	off_t tag_end;
	off_t pos;
	bool present = false;

	fs_file_t_init(&file);
	if (sd_card_open(path, &file))
	{
		return false;
	}

	pos = id3v2_tag_start(&file, &ver_major, &tag_end);
	if (pos < 0 || !skip_to(&file, pos))
	{
		sd_card_close(&file);
		return false;
	}

	while (true)
	{
		char id[5];
		off_t content_pos;
		off_t frame_len;
		uint8_t buf[MAX_TEXT_FRAME_SIZE];

		frame_len = id3v2_next_frame(&file, ver_major, tag_end, id, &content_pos);
		if (frame_len == 0)
		{
			break; /* end of tag or malformed */
		}
		if (frame_len < 0)
		{
			continue; /* frame skipped, file repositioned */
		}

		if ((strcmp(id, "TIT2") == 0 || strcmp(id, "TT2") == 0 ||
			strcmp(id, "TPE1") == 0 || strcmp(id, "TP1") == 0) &&
			(size_t)frame_len <= sizeof(buf))
		{
			if (read_exact(&file, buf, (size_t)frame_len))
			{
				if (strcmp(id, "TIT2") == 0 || strcmp(id, "TT2") == 0)
				{
					decode_text_frame(buf, (size_t)frame_len, title, title_len);
				}
				else
				{
					decode_text_frame(buf, (size_t)frame_len, artist, artist_len);
				}
				present = true;
			}
		}

		(void)skip_to(&file, content_pos + frame_len);
	}

	sd_card_close(&file);
	return present;
}

static bool id3v1_scan(const char *path, char *title, size_t title_len,
	char *artist, size_t artist_len)
{
	struct fs_file_t file;
	uint8_t tail[ID3V1_SIZE];
	bool found = false;

	fs_file_t_init(&file);
	if (sd_card_open(path, &file))
	{
		return false;
	}

	if (fs_seek(&file, -ID3V1_SIZE, FS_SEEK_END) == 0 &&
		read_exact(&file, tail, sizeof(tail)) &&
		memcmp(tail, "TAG", 3) == 0)
	{
		if (title != NULL)
		{
			copy_latin(tail + 3, 30, title, title_len);
		}
		if (artist != NULL)
		{
			copy_latin(tail + 33, 30, artist, artist_len);
		}
		found = true;
	}

	sd_card_close(&file);
	return found;
}

int metadata_read_info(const char *path, char *title, size_t title_len,
	char *artist, size_t artist_len)
{
	if (title != NULL)
	{
		title[0] = '\0';
	}
	if (artist != NULL)
	{
		artist[0] = '\0';
	}

	if (!id3v2_scan_text(path, title, title_len, artist, artist_len))
	{
		id3v1_scan(path, title, title_len, artist, artist_len);
	}

	return 0;
}

/* Case-insensitive substring search (portable memmem replacement) */
static const uint8_t *find_ci(const uint8_t *hay, size_t hay_len,
	const char *needle)
{
	size_t nlen = strlen(needle);
	size_t i;

	if (nlen == 0 || hay_len < nlen)
	{
		return NULL;
	}

	for (i = 0; i + nlen <= hay_len; i++)
	{
		if (strncasecmp((const char *)hay + i, needle, nlen) == 0)
		{
			return hay + i;
		}
	}
	return NULL;
}

/* Skip mime/type/description fields of an APIC frame head; returns the
 * offset of the image data within the frame, or 0 on parse failure.
 * frame_truncated is true when head_len < frame size: then the data
 * may legitimately start exactly at the probe boundary.
 */
static size_t apic_data_offset(const uint8_t *head, size_t head_len,
	size_t frame_size, bool v22, bool *is_jpeg)
{
	size_t pos = 1; /* text encoding */
	size_t term;
	uint8_t enc;

	*is_jpeg = false;

	if (v22)
	{
		if (pos + 3 > head_len)
		{
			return 0;
		}
		*is_jpeg = memcmp(head + pos, "JPG", 3) == 0;
		pos += 3;
	}
	else
	{
		size_t mime_start = pos;

		while (pos < head_len && head[pos] != '\0')
		{
			pos++;
		}
		if (pos >= head_len)
		{
			return 0;
		}
		*is_jpeg = find_ci(head + mime_start, pos - mime_start, "jpeg") != NULL ||
			find_ci(head + mime_start, pos - mime_start, "jpg") != NULL;
		pos++; /* mime terminator */
	}

	enc = head[0];
	term = (enc == 1 || enc == 2) ? 2 : 1;

	pos += 1; /* picture type */
	while (pos + term <= head_len && memcmp(head + pos, "\0\0", term) != 0)
	{
		pos += term;
	}
	if (pos + term > head_len)
	{
		return 0;
	}
	pos += term;

	if (pos < head_len)
	{
		return pos;
	}
	/* data starts exactly at the probe boundary: valid only when
	 * there is more frame content beyond the probe
	 */
	return (head_len < frame_size) ? pos : 0;
}

int metadata_open_art(const char *path, struct fs_file_t *file,
	size_t *offset, size_t *len)
{
	struct fs_file_t scan;
	uint8_t ver_major;
	off_t tag_end;
	off_t pos;
	int ret = -ENOENT;

	fs_file_t_init(file);
	fs_file_t_init(&scan);

	if (sd_card_open(path, &scan))
	{
		return -ENOENT;
	}

	pos = id3v2_tag_start(&scan, &ver_major, &tag_end);
	if (pos < 0 || !skip_to(&scan, pos))
	{
		goto out;
	}

	while (true)
	{
		char id[5];
		off_t content_pos;
		off_t frame_len;
		uint8_t head[APIC_HEAD_PROBE];
		size_t probe;
		size_t data_off;
		bool is_jpeg = false;

		frame_len = id3v2_next_frame(&scan, ver_major, tag_end, id, &content_pos);
		if (frame_len == 0)
		{
			break; /* end of tag or malformed */
		}
		if (frame_len < 0)
		{
			continue; /* frame skipped, file repositioned */
		}

		if (strcmp(id, "APIC") != 0 && strcmp(id, "PIC") != 0)
		{
			(void)skip_to(&scan, content_pos + frame_len);
			continue;
		}

		if ((size_t)frame_len > MAX_ART_FRAME_SIZE)
		{
			/* implausible for a cover: skip, keep looking */
			(void)skip_to(&scan, content_pos + frame_len);
			continue;
		}

		probe = MIN((size_t)frame_len, sizeof(head));
		if (!read_exact(&scan, head, probe))
		{
			break;
		}

		data_off = apic_data_offset(head, probe, (size_t)frame_len,
			ver_major == 2, &is_jpeg);
		if (is_jpeg && data_off > 0)
		{
			/* hand a fresh handle positioned at the image data to
			 * the caller (the scan handle is closed below)
			 */
			if (sd_card_open(path, file))
			{
				goto out;
			}
			if (fs_seek(file, content_pos + (off_t)data_off, FS_SEEK_SET))
			{
				sd_card_close(file);
				goto out;
			}
			*offset = (size_t)(content_pos + data_off);
			*len = (size_t)frame_len - data_off;
			ret = 0;
			goto out;
		}

		(void)skip_to(&scan, content_pos + frame_len);
	}

out:
	sd_card_close(&scan);
	return ret;
}
