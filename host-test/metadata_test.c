/* Host-side test for the metadata module: FLAC VORBIS_COMMENT tags and
 * PICTURE (JPEG) art location.
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <zephyr/fs/fs.h>
#include "sd_card.h"
#include "metadata.h"

int main(void)
{
	char title[48] = {0}, artist[48] = {0};
	struct fs_file_t f;
	size_t off = 0, len = 0;

	metadata_read_info("t_tags.flac", title, sizeof(title), artist, sizeof(artist));
	printf("title='%s' artist='%s'\n", title, artist);

	int ret = metadata_open_art("t_tags.flac", &f, &off, &len);
	printf("art: ret=%d off=%zu len=%zu\n", ret, off, len);
	if (ret == 0) {
		uint8_t hdr[2];

		fs_read(&f, hdr, 2);
		printf("jpeg magic: %02x %02x (want ff d8)\n", hdr[0], hdr[1]);
		sd_card_close(&f);
	}
	return 0;
}
