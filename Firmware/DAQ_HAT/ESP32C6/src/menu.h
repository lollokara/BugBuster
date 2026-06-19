#pragma once

// Pebble-style carousel menu system for the DAQ HAT display.
//
// A small state machine drives a stack of menus/submenus. The selection box
// snaps between rows with a spring "gravity" overshoot. Items can be
// submenus, toggles, option cyclers, bargraph editors, or read-only info
// rows (diagnostics). A 30 s inactivity timer closes the menu back to the
// main readout. To stay light on the FPU-less core, the menu only redraws
// while animating or on a low-rate refresh tick.

#include <stdint.h>
#include <stdbool.h>

typedef enum {
    MENU_RUNNING,
    MENU_CLOSED,    // timed out or backed out of the root
} menu_status_t;

void menu_init(void);

// Open the menu at the root (called when a button is pressed from the main
// readout). The button that opened the menu is consumed by the caller.
void menu_open(uint32_t now_ms);

bool menu_active(void);

// Feed button events (BTN_EV_* bitmask) and advance animation/timeout.
// Sets *need_render true when the framebuffer must be repainted this tick.
menu_status_t menu_update(uint32_t events, uint32_t now_ms, bool *need_render);

// Paint the current menu/editor into the framebuffer (does NOT flush).
void menu_render(uint32_t now_ms);
