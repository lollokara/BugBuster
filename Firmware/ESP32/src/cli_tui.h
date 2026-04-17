#pragma once

// =============================================================================
// cli_tui.h - Read-only full-screen dashboard for the CLI.
//
// Uses the alt-screen buffer so exiting restores the user's scrollback.
// Refreshes at 2 Hz from a snapshot of g_deviceState (taken under
// g_stateMutex with a short timeout — frames are skipped on contention).
//
// Exit paths:
//   - user presses 'q' or Ctrl-C
//   - BBP handshake byte (0xBB) arrives: alt-screen is unwound preemptively
//     so the host terminal is never left dangling mid-binary-session.
//
// Phase 4 of the CLI rebuild.
// =============================================================================

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void cli_tui_init(void);
bool cli_tui_active(void);

// Enter / leave alt-screen dashboard.
void cli_tui_enter(void);
void cli_tui_leave(void);

// Called when a 0xBB byte is seen: preemptively leave alt-screen (but keep
// TUI state so we can re-enter on next tick if it wasn't a handshake).
void cli_tui_preempt(void);

// Consume one inbound byte when the TUI is active. Sets "want_exit" on q/Ctrl-C.
void cli_tui_feed(uint8_t byte);

// Returns true if the TUI has requested its parent loop go back to CLI mode.
bool cli_tui_want_exit(void);
void cli_tui_clear_want_exit(void);

// Called periodically from cliProcess. Handles re-enter after preempt and
// schedules the 2 Hz redraw.
void cli_tui_tick(void);

// CLI command handler registered in the command table.
void cli_cmd_tui(const char* args);

#ifdef __cplusplus
}
#endif
