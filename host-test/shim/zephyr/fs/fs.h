#pragma once
#include <stdio.h>
#include <sys/types.h>

#define FS_SEEK_SET SEEK_SET
#define FS_SEEK_CUR SEEK_CUR
#define FS_SEEK_END SEEK_END

struct fs_file_t {
	FILE *f;
};

static inline void fs_file_t_init(struct fs_file_t *f) { f->f = NULL; }
static inline ssize_t fs_read(struct fs_file_t *f, void *buf, size_t len)
{
	return (ssize_t)fread(buf, 1, len, f->f);
}
static inline int fs_seek(struct fs_file_t *f, off_t off, int whence)
{
	return fseek(f->f, off, whence);
}
static inline off_t fs_tell(struct fs_file_t *f)
{
	return ftell(f->f);
}
