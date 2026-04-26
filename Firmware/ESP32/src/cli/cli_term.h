#pragma once

// =============================================================================
// cli_term.h - ANSI terminal output layer for the CLI.
//
// Every byte written through the term_* functions goes through a single
// choke point (term_emit) that is gated on bbpCdcClaimed(): once the BBP
// binary protocol has ever taken over CDC #0, the CLI must emit zero bytes.
//
// When color is disabled (either by the user via `color off` or because we
// suspect a dumb terminal), SGR escape sequences are skipped; the surrounding
// text still renders correctly.
//
// Phase 2 of the CLI rebuild: color + gating. Cursor / alt-screen / size
// probe APIs are reserved for Phases 3-4 and intentionally not here yet.
// =============================================================================

#include <stddef.h>
#include <stdint.h>

typedef enum {
    TERM_FG_DEFAULT    = 39,
    TERM_FG_BLACK      = 30,
    TERM_FG_RED        = 31,
    TERM_FG_GREEN      = 32,
    TERM_FG_YELLOW     = 33,
    TERM_FG_BLUE       = 34,
    TERM_FG_MAGENTA    = 35,
    TERM_FG_CYAN       = 36,
    TERM_FG_WHITE      = 37,
    TERM_FG_B_BLACK    = 90,
    TERM_FG_B_RED      = 91,
    TERM_FG_B_GREEN    = 92,
    TERM_FG_B_YELLOW   = 93,
    TERM_FG_B_BLUE     = 94,
    TERM_FG_B_MAGENTA  = 95,
    TERM_FG_B_CYAN     = 96,
    TERM_FG_B_WHITE    = 97,
} TermColor;

typedef enum {
    TERM_ATTR_RESET     = 0,
    TERM_ATTR_BOLD      = 1,
    TERM_ATTR_DIM       = 2,
    TERM_ATTR_ITALIC    = 3,
    TERM_ATTR_UNDERLINE = 4,
    TERM_ATTR_REVERSE   = 7,
} TermAttr;

#ifdef __cplusplus
extern "C" {
#endif

void term_init(void);

// Enable / disable color SGR output. Default: enabled.
bool term_color_enabled(void);
void term_set_color(bool on);

// Single choke point: gated on !bbpCdcClaimed(). All term_* writes funnel here.
void term_emit(const char* buf, size_t len);

// Gated mirrors of serial_print*. Safe to call at any time; silently drop
// bytes when CDC #0 has been claimed by BBP.
void term_print(const char* s);
void term_println(const char* s);
void term_printf(const char* fmt, ...);

// Color / attribute SGR emitters. All no-ops if color is disabled.
void term_fg(TermColor c);
void term_attr(TermAttr a);
void term_reset_sgr(void);

// Convenience: set fg, print formatted, reset. Color-gated.
void term_cprintf(TermColor c, const char* fmt, ...);

// Convenience: print `s` with fg `c`, auto-reset, no newline.
void term_cprint(TermColor c, const char* s);

// Bold wrapper: attr BOLD + fg c + text + reset.
void term_bold_cprint(TermColor c, const char* s);

// ---- Cursor / line manipulation ----
void term_cursor_back(int n);
void term_cursor_forward(int n);
void term_cursor_home_col(void);    // emit '\r'
void term_erase_to_eol(void);       // ESC[K

// ---- Terminal size (async CPR probe) ----
// Default (24, 80) until an CPR reply arrives. Host-requested probe is
// non-blocking: the reply is parsed by the CLI input state machine and
// piped back here via cli_term_on_size().
int  term_rows(void);
int  term_cols(void);
void term_probe_size(void);
void cli_term_on_size(int rows, int cols);

// ---- CLI command handler registered in the command table ----
void cli_cmd_color(const char* args);

#ifdef __cplusplus
}
#endif
