/* Host-side end-to-end test: runs the firmware's decoder_open /
 * decoder_fill pipeline (sniffing, ID3-prefixed FLAC, WAV, MP3) and
 * writes raw interleaved s16le PCM. Compare against a reference, e.g.:
 *   ffmpeg -i in.flac -f s16le -acodec pcm_s16le -ac 2 ref.pcm
 *   cmp out.pcm ref.pcm
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <zephyr/fs/fs.h>
#include "decoder.h"

int main(int argc, char **argv)
{
	if (argc != 3) {
		fprintf(stderr, "usage: %s in.flac out.pcm\n", argv[0]);
		return 2;
	}

	FILE *in = fopen(argv[1], "rb");
	if (!in) { perror("open"); return 2; }
	FILE *out = fopen(argv[2], "wb");
	if (!out) { perror("create"); return 2; }

	struct audio_decoder dec;
	struct fs_file_t f = { .f = in };

	fseek(in, 0, SEEK_END);
	uint32_t size = (uint32_t)ftell(in);
	fseek(in, 0, SEEK_SET);

	int ret = decoder_open(&dec, &f, size);
	if (ret) {
		fprintf(stderr, "decoder_open failed: %d\n", ret);
		return 1;
	}
	fprintf(stderr, "format=%d sr=%u ch=%u total_ms=%u\n", dec.format,
		dec.sample_rate, dec.src_channels, dec.total_ms);

	int16_t buf[1152 * 2];
	uint64_t total_frames = 0;
	while (1) {
		size_t n = decoder_fill(&dec, buf, 1152);
		if (n == 0) break;
		fwrite(buf, sizeof(int16_t), n * 2, out);
		total_frames += n;
	}
	fprintf(stderr, "frames=%llu elapsed_ms=%u\n",
		(unsigned long long)total_frames, dec.elapsed_ms);
	decoder_close(&dec);
	fclose(out);
	fclose(in);
	return 0;
}
