#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Initialize the interactive menu module.
void cli_menu_init(void);

// Enter the interactive menu (takes over the screen).
void cli_menu_enter(void);

// Leave the interactive menu.
void cli_menu_leave(void);

// Handle a preemptive unwind of the alt-screen (for BBP).
void cli_menu_preempt(void);

// Returns true if the menu is currently active.
bool cli_menu_active(void);

// Returns true if the menu has requested to exit (e.g. user pressed 'q').
bool cli_menu_want_exit(void);

// Clear the exit request flag.
void cli_menu_clear_want_exit(void);

// Feed a byte of input into the menu's state machine.
void cli_menu_feed(uint8_t b);

// Periodic tick for redrawing the UI.
void cli_menu_tick(void);

#ifdef __cplusplus
}
#endif
