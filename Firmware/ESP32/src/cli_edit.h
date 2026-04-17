#pragma once

// =============================================================================
// cli_edit.h - Byte-level line editor state machine for the CLI.
//
// cliProcess() feeds bytes via cli_edit_feed(). When Enter is pressed, the
// editor sets a line-ready flag which the caller drains with
// cli_edit_take_line(), dispatches, then lets the editor reset.
//
// The editor handles:
//   - printable ASCII insert at cursor
//   - Backspace (0x08, 0x7F) delete-before-cursor
//   - ESC [ A / B / C / D  (history up/down, cursor left/right)
//   - ESC [ H / F          (Home / End)
//   - ESC [ 1~ / 4~ / 7~ / 8~  (Home / End variants)
//   - ESC [ 3~             (Delete forward)
//   - ESC [ <r>;<c> R      (Cursor Position Report — reports size to cli_term)
//   - ESC O P..S           (SS3 F1..F4 — reserved, no-op in Phase 3)
//   - Ctrl-A (Home) Ctrl-E (End) Ctrl-K (kill-to-eol) Ctrl-U (kill-line)
//   - Ctrl-C (abandon line)
//   - Tab (first-word completion)
//
// Phase 3 of the CLI rebuild. Word motion (Alt-B/F, Ctrl-W) lands in Phase 5.
// =============================================================================

#include <stdbool.h>
#include <stdint.h>

#define CLI_EDIT_BUF_SIZE  256

#ifdef __cplusplus
extern "C" {
#endif

void        cli_edit_init(void);
void        cli_edit_reset(void);

// Called by cli.cpp after the prompt has been printed. `visible_cols` is the
// on-screen width of the prompt text (excluding SGR escapes).
void        cli_edit_on_prompt_shown(int visible_cols);

// Feed one inbound byte through the state machine. Returns nothing; query
// cli_edit_line_ready() afterward.
void        cli_edit_feed(uint8_t byte);

// Non-zero if Enter was pressed since the last take / reset.
bool        cli_edit_line_ready(void);

// Consume the committed line (null-terminated). The editor clears its buffer
// and line-ready flag. Caller should then print a new prompt.
const char* cli_edit_take_line(void);

// After a tab-completion listing, the caller needs to re-emit the prompt.
bool        cli_edit_need_reprompt(void);
void        cli_edit_clear_reprompt(void);

// Re-emit the current buffer and reposition the cursor. Called after the
// prompt is re-printed during a tab-completion list redraw.
void        cli_edit_redraw_line(void);

#ifdef __cplusplus
}
#endif
