#pragma once

#include <stdbool.h>

#include "main.h"

/* List `path` into ctx (dirs first, then audio files, both sorted),
 * scanning ID3 metadata for the audio entries
 */
int browser_open_dir(struct browser_ctx *ctx, const char *path);

/* Move the selection by `step` entries (wraps around); returns true
 * if the selection moved
 */
bool browser_move_selection(struct browser_ctx *ctx, int step);

/* Returns the full path of the selected entry (cwd + name) */
void browser_selected_path(const struct browser_ctx *ctx, char *out, size_t out_len);

/* Enter the selected directory (no-op for files); returns true if
 * navigation happened
 */
bool browser_enter_selected(struct browser_ctx *ctx);

/* Go up one directory; returns true if navigation happened (root is
 * the top)
 */
bool browser_up(struct browser_ctx *ctx);

/* Scan a directory listing without touching the browser state (used
 * for shuffle/next/prev on the playing folder). with_metadata=false
 * skips the (slow) ID3 tag pass when only names are needed.
 */
int browser_scan_dir(const char *path, struct browser_entry *entries, int max_entries,
	bool with_metadata);

/* dir + entry name -> "dir/name" (dir stored without trailing slash) */
void path_join(const char *dir, const char *name, char *out, size_t out_len);
