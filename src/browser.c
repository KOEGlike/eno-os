#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/fs/fs.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <stdlib.h>

#include "browser.h"
#include "metadata.h"
#include "sd_card.h"

LOG_MODULE_REGISTER(BROWSER, LOG_LEVEL_INF);

#define BROWSER_ROOT_PATH "/SD:"
/* FAT metadata folder that appears on many cards */
#define SYSTEM_VOLUME_INFO "System Volume Information"

static bool ends_with_ci(const char *name, const char *ext)
{
	size_t len = strlen(name);
	size_t ext_len = strlen(ext);

	if (len < ext_len)
	{
		return false;
	}

	return strcasecmp(name + len - ext_len, ext) == 0;
}

static bool is_audio_name(const char *name)
{
	return ends_with_ci(name, ".wav") || ends_with_ci(name, ".mp3") ||
		ends_with_ci(name, ".flac");
}

void path_join(const char *dir, const char *name, char *out, size_t out_len)
{
	/* dir is stored without a trailing slash */
	snprintk(out, out_len, "%s/%s", dir, name);
}

/* Copy a name without its audio extension, for fallback titles */
static void strip_audio_ext(const char *name, char *out, size_t out_len)
{
	const char *dot = strrchr(name, '.');

	if (dot != NULL && is_audio_name(name))
	{
		snprintk(out, out_len, "%.*s", (int)(dot - name), name);
	}
	else
	{
		strncpy(out, name, out_len - 1);
		out[out_len - 1] = '\0';
	}
}

static int entry_cmp(const void *a, const void *b)
{
	const struct browser_entry *ea = a;
	const struct browser_entry *eb = b;

	/* dirs first, then case-insensitive names */
	if (ea->is_dir != eb->is_dir)
	{
		return ea->is_dir ? -1 : 1;
	}

	for (const char *ca = ea->name, *cb = eb->name; *ca && *cb; ca++, cb++)
	{
		int d = tolower((unsigned char)*ca) - tolower((unsigned char)*cb);

		if (d != 0)
		{
			return d;
		}
	}

	return (int)strlen(ea->name) - (int)strlen(eb->name);
}

static void scan_metadata(struct browser_entry *entries, int count, const char *dir)
{
	char path[MAX_PATH_LEN];

	for (int i = 0; i < count; i++)
	{
		struct browser_entry *e = &entries[i];

		if (e->is_dir || !e->is_audio || e->scanned)
		{
			continue;
		}

		path_join(dir, e->name, path, sizeof(path));
		metadata_read_info(path, e->title, sizeof(e->title),
			e->artist, sizeof(e->artist));

		/* file name fallback when there is no title tag */
		if (e->title[0] == '\0')
		{
			strip_audio_ext(e->name, e->title, sizeof(e->title));
		}

		e->scanned = true;
	}
}

int browser_scan_dir(const char *path, struct browser_entry *entries, int max_entries,
	bool with_metadata)
{
	struct fs_dir_t dir;
	struct fs_dirent ent;
	int count = 0;

	fs_dir_t_init(&dir);
	if (fs_opendir(&dir, path))
	{
		LOG_ERR("opendir %s failed", path);
		return -ENOENT;
	}

	while (fs_readdir(&dir, &ent) == 0 && ent.name[0] != '\0')
	{
		if (ent.name[0] == '.' ||
			strcasecmp(ent.name, SYSTEM_VOLUME_INFO) == 0)
		{
			continue; /* ".", "..", hidden files, FAT metadata dir */
		}

		if (count >= max_entries)
		{
			LOG_WRN("Directory listing truncated at %d entries", max_entries);
			break;
		}

		/* names that do not fit would be truncated into entries
		 * that can never be opened again: skip them
		 */
		if (strlen(ent.name) >= sizeof(entries[0].name) ||
			strlen(path) + 1 + strlen(ent.name) >= MAX_PATH_LEN)
		{
			LOG_WRN("Skipping %s: name or path too long", ent.name);
			continue;
		}

		struct browser_entry *e = &entries[count];

		memset(e, 0, sizeof(*e));
		strncpy(e->name, ent.name, sizeof(e->name) - 1);
		e->is_dir = (ent.type == FS_DIR_ENTRY_DIR);
		e->is_audio = !e->is_dir && is_audio_name(ent.name);

		if (e->is_dir || e->is_audio)
		{
			count++;
		}
	}

	(void)fs_closedir(&dir);

	qsort(entries, count, sizeof(struct browser_entry), entry_cmp);
	if (with_metadata)
	{
		scan_metadata(entries, count, path);
	}

	return count;
}

int browser_open_dir(struct browser_ctx *ctx, const char *path)
{
	if (strlen(path) + 1 > sizeof(ctx->cwd))
	{
		LOG_ERR("Path too long: %s", path);
		return -ENAMETOOLONG;
	}

	int count = browser_scan_dir(path, ctx->entries, MAX_BROWSER_ENTRIES, true);

	if (count < 0)
	{
		return count;
	}

	strncpy(ctx->cwd, path, sizeof(ctx->cwd) - 1);
	ctx->cwd[sizeof(ctx->cwd) - 1] = '\0';
	ctx->count = count;
	ctx->selected = 0;
	ctx->scroll_top = 0;

	return 0;
}

bool browser_move_selection(struct browser_ctx *ctx, int step)
{
	if (ctx->count == 0 || step == 0)
	{
		return false;
	}

	ctx->selected = ((ctx->selected + step) % ctx->count + ctx->count) % ctx->count;

	/* keep the selection inside the visible window */
	if (ctx->selected < ctx->scroll_top)
	{
		ctx->scroll_top = ctx->selected;
	}
	else if (ctx->selected >= ctx->scroll_top + BROWSER_VISIBLE_ROWS)
	{
		ctx->scroll_top = ctx->selected - BROWSER_VISIBLE_ROWS + 1;
	}

	return true;
}

void browser_selected_path(const struct browser_ctx *ctx, char *out, size_t out_len)
{
	if (ctx->count == 0 || ctx->selected >= ctx->count)
	{
		out[0] = '\0';
		return;
	}

	path_join(ctx->cwd, ctx->entries[ctx->selected].name, out, out_len);
}

bool browser_enter_selected(struct browser_ctx *ctx)
{
	const struct browser_entry *e;

	if (ctx->count == 0 || ctx->selected >= ctx->count)
	{
		return false;
	}

	e = &ctx->entries[ctx->selected];
	if (!e->is_dir)
	{
		return false;
	}

	char path[MAX_PATH_LEN];

	path_join(ctx->cwd, e->name, path, sizeof(path));
	return browser_open_dir(ctx, path) == 0;
}

bool browser_up(struct browser_ctx *ctx)
{
	if (strcmp(ctx->cwd, BROWSER_ROOT_PATH) == 0)
	{
		return false;
	}

	char parent[MAX_PATH_LEN];

	strncpy(parent, ctx->cwd, sizeof(parent) - 1);
	parent[sizeof(parent) - 1] = '\0';

	char *slash = strrchr(parent, '/');

	if (slash == NULL)
	{
		return false;
	}

	if (slash == parent)
	{
		/* "/SD:/foo" -> "/SD:" (the mount point has no slash) */
		slash[1] = '\0';
	}
	else
	{
		slash[0] = '\0';
	}

	return browser_open_dir(ctx, parent) == 0;
}
