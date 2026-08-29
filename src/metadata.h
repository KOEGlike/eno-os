#pragma once

#include <stddef.h>
#include <stdbool.h>
#include <zephyr/fs/fs.h>

/* Read title/artist from ID3 tags (ID3v2.2/2.3/2.4, ID3v1 fallback) */
int metadata_read_info(const char *path, char *title, size_t title_len,
	char *artist, size_t artist_len);

/* Locate embedded album art in an MP3. Opens *file on success; the
 * caller must sd_card_close it after reading [offset, offset+len).
 * Only JPEG art is reported (tjpgd is the only available decoder).
 */
int metadata_open_art(const char *path, struct fs_file_t *file,
	size_t *offset, size_t *len);
