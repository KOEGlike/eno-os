#pragma once

/* AS5600 hall rotation knob: knob_poll() reports the selection steps
 * accumulated since the last call (one step per DEGREES_PER_STEP of
 * rotation); knob_haptic_pulse() fires the vibration motor briefly.
 */
int init_knob(void);
int knob_poll(void);
void knob_haptic_pulse(void);
