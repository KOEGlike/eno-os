#pragma once

#include "decoder.h"

/* FLAC support for the unified decoder. flac_parse() reads the
 * metadata blocks (STREAMINFO) and fills sample_rate/total_ms;
 * flac_fill() decodes frames on demand into interleaved stereo
 * 16-bit PCM. State lives in flac.c statics (single decoder).
 */
int flac_parse(struct audio_decoder *dec);
size_t flac_fill(struct audio_decoder *dec, int16_t *dst, size_t max_frames);
