#pragma once

/* Minimal LVGL image decoder for variable (in-flash) I1 images.
 * LVGL's bin decoder treats I1 as an indexed format and expands it
 * through an ARGB8888 intermediate (far too large for this board).
 * This decoder hands the 1bpp data straight to the software renderer,
 * whose I1->I1 blend copies bits exactly: bit 1 = paper, bit 0 = ink.
 */

void i1_decoder_init(void);
