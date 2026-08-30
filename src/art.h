#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Album art canvas: 1bpp (LV_COLOR_FORMAT_I1), MSB-first rows.
 * Bit 1 = paper (white), bit 0 = ink (black), matching the mono
 * framebuffer convention of the display.
 */
#define ART_W 120
#define ART_H 120
#define ART_STRIDE ((ART_W + 7) / 8)

/* Decode the embedded JPEG album art of an MP3 into a dithered 1bpp
 * canvas (ART_STRIDE x ART_H bytes). Returns 0 when real album art
 * was rendered; on any failure the canvas is filled with the music
 * note fallback and -ENOENT is returned.
 */
int art_render(const char *path, uint8_t *canvas);

/* Fill the canvas with the music note glyph (for WAV / no art) */
void art_render_fallback(uint8_t *canvas);
