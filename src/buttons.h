#pragma once

#include <zephyr/sys/atomic.h>

int init_buttons(void);
int buttons_take_power_events(void);
int buttons_take_left_events(void);
int buttons_take_right_events(void);
int buttons_take_volume_up_events(void);
int buttons_take_volume_down_events(void);


