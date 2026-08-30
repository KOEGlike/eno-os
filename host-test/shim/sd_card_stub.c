#include <stdio.h>
#include <string.h>
#include <zephyr/fs/fs.h>

int sd_card_open(char const *const filename, struct fs_file_t *f)
{
	f->f = fopen(filename, "rb");
	return f->f ? 0 : -1;
}

int sd_card_close(struct fs_file_t *f)
{
	if (f->f) {
		fclose(f->f);
	}
	f->f = NULL;
	return 0;
}
