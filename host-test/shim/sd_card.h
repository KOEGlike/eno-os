#pragma once
#include <zephyr/fs/fs.h>

/* Host stubs matching the firmware's sd_card.h API (see sd_card_stub.c) */
int sd_card_open(char const *const filename, struct fs_file_t *f);
int sd_card_close(struct fs_file_t *f);
