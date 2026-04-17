// =============================================================================
// cli_term.cpp - ANSI terminal output layer for the CLI.
//
// Every byte routed through the term_* API passes through term_emit(), which
// short-circuits when bbpCdcClaimed() is true so the CLI can never corrupt an
// active BBP binary session.
// =============================================================================

#include "cli_term.h"
#include "bbp.h"
#include "usb_cdc.h"
#include "serial_io.h"   // for serial_println fallback in the color cmd

#include <stdio.h>
#include <string.h>
#include <stdarg.h>

// ---------------------------------------------------------------------------
// Module state
// ---------------------------------------------------------------------------
static bool s_color_enabled = true;
static int  s_rows = 24;
static int  s_cols = 80;

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

extern "C" void term_init(void)
{
    s_color_enabled = true;
}

extern "C" bool term_color_enabled(void)
{
    return s_color_enabled;
}

extern "C" void term_set_color(bool on)
{
    s_color_enabled = on;
}

// The single choke point. Any direct writes to CDC #0 outside this function
// bypass the BBP safety gate and will eventually land as a regression.
extern "C" void term_emit(const char* buf, size_t len)
{
    if (!buf || len == 0) return;
    if (bbpCdcClaimed()) return;
    usb_cdc_cli_write((const uint8_t*)buf, len);
}

extern "C" void term_print(const char* s)
{
    if (!s) return;
    term_emit(s, strlen(s));
}

extern "C" void term_println(const char* s)
{
    if (s) term_emit(s, strlen(s));
    term_emit("\r\n", 2);
}

extern "C" void term_printf(const char* fmt, ...)
{
    char buf[512];
    va_list args;
    va_start(args, fmt);
    int len = vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    if (len > 0) {
        if ((size_t)len > sizeof(buf)) len = (int)sizeof(buf);
        term_emit(buf, (size_t)len);
    }
}

// ---------------------------------------------------------------------------
// SGR helpers — no-op when color disabled.
// ---------------------------------------------------------------------------

extern "C" void term_fg(TermColor c)
{
    if (!s_color_enabled) return;
    char seq[12];
    int n = snprintf(seq, sizeof(seq), "\x1b[%dm", (int)c);
    if (n > 0) term_emit(seq, (size_t)n);
}

extern "C" void term_attr(TermAttr a)
{
    if (!s_color_enabled) return;
    char seq[8];
    int n = snprintf(seq, sizeof(seq), "\x1b[%dm", (int)a);
    if (n > 0) term_emit(seq, (size_t)n);
}

extern "C" void term_reset_sgr(void)
{
    if (!s_color_enabled) return;
    term_emit("\x1b[0m", 4);
}

extern "C" void term_cprintf(TermColor c, const char* fmt, ...)
{
    if (s_color_enabled) term_fg(c);

    char buf[512];
    va_list args;
    va_start(args, fmt);
    int len = vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    if (len > 0) {
        if ((size_t)len > sizeof(buf)) len = (int)sizeof(buf);
        term_emit(buf, (size_t)len);
    }

    if (s_color_enabled) term_reset_sgr();
}

extern "C" void term_cprint(TermColor c, const char* s)
{
    if (!s) return;
    if (s_color_enabled) term_fg(c);
    term_emit(s, strlen(s));
    if (s_color_enabled) term_reset_sgr();
}

extern "C" void term_bold_cprint(TermColor c, const char* s)
{
    if (!s) return;
    if (s_color_enabled) {
        term_attr(TERM_ATTR_BOLD);
        term_fg(c);
    }
    term_emit(s, strlen(s));
    if (s_color_enabled) term_reset_sgr();
}

// ---------------------------------------------------------------------------
// Cursor / line manipulation. These always emit (not gated on color) because
// they are structural, not cosmetic. Still gated via term_emit on BBP.
// ---------------------------------------------------------------------------

extern "C" void term_cursor_back(int n)
{
    if (n <= 0) return;
    char seq[12];
    int k = snprintf(seq, sizeof(seq), "\x1b[%dD", n);
    if (k > 0) term_emit(seq, (size_t)k);
}

extern "C" void term_cursor_forward(int n)
{
    if (n <= 0) return;
    char seq[12];
    int k = snprintf(seq, sizeof(seq), "\x1b[%dC", n);
    if (k > 0) term_emit(seq, (size_t)k);
}

extern "C" void term_cursor_home_col(void)
{
    term_emit("\r", 1);
}

extern "C" void term_erase_to_eol(void)
{
    term_emit("\x1b[K", 3);
}

// ---------------------------------------------------------------------------
// Terminal size (async probe via Cursor Position Report)
// ---------------------------------------------------------------------------

extern "C" int term_rows(void) { return s_rows; }
extern "C" int term_cols(void) { return s_cols; }

extern "C" void term_probe_size(void)
{
    // Save cursor, jump far off-screen, ask for CPR, restore cursor.
    // Reply arrives as an inbound CSI sequence and is parsed by cli_edit.
    term_emit("\x1b[s\x1b[999;999H\x1b[6n\x1b[u", 17);
}

extern "C" void cli_term_on_size(int rows, int cols)
{
    if (rows >= 4 && rows <= 300) s_rows = rows;
    if (cols >= 20 && cols <= 500) s_cols = cols;
}

// ---------------------------------------------------------------------------
// `color` CLI command.
//   color           Show current setting.
//   color on        Enable ANSI color output.
//   color off       Disable (plain text only).
// ---------------------------------------------------------------------------
extern "C" void cli_cmd_color(const char* args)
{
    while (args && *args == ' ') args++;

    if (!args || !*args) {
        term_printf("ANSI color is currently: %s\r\n",
                    s_color_enabled ? "ON" : "OFF");
        term_println("Usage: color on | off");
        return;
    }

    if (strcmp(args, "on") == 0) {
        term_set_color(true);
        term_cprintf(TERM_FG_B_GREEN, "ANSI color enabled.\r\n");
        return;
    }
    if (strcmp(args, "off") == 0) {
        term_set_color(false);
        term_println("ANSI color disabled.");
        return;
    }

    term_println("Usage: color on | off");
}
