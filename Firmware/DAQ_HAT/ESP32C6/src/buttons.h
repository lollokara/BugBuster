#pragma once

// Front-panel button driver: UP, DOWN, OK (active-low with pull-ups).
// Generates discrete navigation events with debounce, auto-repeat for UP/DOWN
// (so holding scrolls / adjusts a bargraph), and a long-press on OK that maps
// to BACK. Poll buttons_poll() once per main-loop iteration.

#include <stdint.h>

// Event bitmask returned by buttons_poll().
#define BTN_EV_UP    0x01u
#define BTN_EV_DOWN  0x02u
#define BTN_EV_OK    0x04u   // OK short press (on release)
#define BTN_EV_BACK  0x08u   // OK long press (hold)

void     buttons_init(void);

// Returns a bitmask of events that occurred since the last poll. `now_ms` is a
// monotonic millisecond timestamp.
uint32_t buttons_poll(uint32_t now_ms);
