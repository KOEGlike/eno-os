#include <zephyr/logging/log.h>
#include <ctype.h>
#include <string.h>

#include "sd_card.h"
#include "songs.h"
#include "main.h"

LOG_MODULE_REGISTER(SONGS, LOG_LEVEL_INF);

#define SONG_LIST_BUF_SIZE 2048

static bool ends_with_wav(const char *name)
{
	size_t len = strlen(name);

	if (len < 4)
	{
		return false;
	}

	return tolower((unsigned char)name[len - 4]) == '.' &&
		   tolower((unsigned char)name[len - 3]) == 'w' &&
		   tolower((unsigned char)name[len - 2]) == 'a' &&
		   tolower((unsigned char)name[len - 1]) == 'v';
}

int load_songs_from_root(struct app_state *state)
{
	int ret;
	char list_buf[SONG_LIST_BUF_SIZE];
	size_t buf_size = sizeof(list_buf);
	char *entry;

	memset(list_buf, 0, sizeof(list_buf));
	ret = sd_card_list_files(NULL, list_buf, &buf_size, false);
	if (ret)
	{
		LOG_ERR("Failed to list root files: %d", ret);
		return ret;
	}

	state->song_count = 0;
	entry = strtok(list_buf, "\r\n");
	while (entry != NULL && state->song_count < MAX_SONGS)
	{
		if (ends_with_wav(entry))
		{
			strncpy(state->songs[state->song_count], entry, MAX_SONG_NAME_LEN - 1);
			state->songs[state->song_count][MAX_SONG_NAME_LEN - 1] = '\0';
			state->song_count++;
		}
		entry = strtok(NULL, "\r\n");
	}

	if (state->song_count == 0)
	{
		state->selected_index = 0;
	}
	else if (state->selected_index >= state->song_count)
	{
		state->selected_index = 0;
	}

	state->list_dirty = true;
	state->ui_dirty = true;
	LOG_INF("Loaded %d WAV song(s) from SD root", state->song_count);
	return 0;
}
