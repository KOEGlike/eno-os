#pragma once

#include "main.h"

int ui_init(void);
void ui_refresh(struct app_state *state);
/* Show/hide the mode views and reset text caches */
void ui_switch_mode(enum ui_mode mode);
void ui_full_refresh_check(struct app_state *state);
