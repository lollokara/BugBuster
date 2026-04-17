// =============================================================================
// cli.cpp - Serial CLI shell: byte loop, BBP handshake interlock,
// table-driven command dispatch, and TUI dashboard routing.
//
// Handler bodies live in cli_cmds_*.cpp; the command table is in
// cli_cmdtab.cpp; the line-editor state machine lives in cli_edit.cpp;
// the full-screen dashboard lives in cli_tui.cpp. This file is the entry
// point called from mainLoopTask — it owns the prompt, feeds inbound bytes
// to either the editor or the TUI, and dispatches committed lines.
//
// Phase 4 of the CLI rebuild: adds `tui` routing on top of the Phase 3 line
// editor. The BBP handshake interlock unwinds the alt-screen preemptively
// on every 0xBB byte so a BBP handshake always leaves the terminal clean.
// =============================================================================

#include "cli.h"
#include "cli_shared.h"
#include "cli_cmdtab.h"
#include "cli_term.h"
#include "cli_edit.h"
#include "cli_tui.h"
#include "cli_history.h"
#include "serial_io.h"
#include "bbp.h"

#include <stdio.h>
#include <string.h>

// ---------------------------------------------------------------------------
// Shared state (definition of the extern declared in cli_shared.h)
// ---------------------------------------------------------------------------
AD74416H* g_cli_dev = nullptr;

// ---------------------------------------------------------------------------
// Shell state
// ---------------------------------------------------------------------------
static bool s_showPrompt = true;
static bool s_size_probed_once = false;

// Visible width (columns) of the prompt text "[BugBuster]> ".
static const int PROMPT_VIS_COLS = 13;

// ---------------------------------------------------------------------------
// Prompt rendering
// ---------------------------------------------------------------------------

static void emit_prompt_inline(void)
{
    term_bold_cprint(TERM_FG_B_CYAN, "[BugBuster]");
    term_print("> ");
}

static void emit_prompt_fresh(void)
{
    term_print("\r\n");
    emit_prompt_inline();
}

// ---------------------------------------------------------------------------
// "Did you mean?" — edit-distance-1 nearest command.
// Returns NULL if no near match.
// ---------------------------------------------------------------------------
static int abs_i(int x) { return x < 0 ? -x : x; }

static bool edit_distance_le_1(const char* a, const char* b)
{
    int la = (int)strlen(a), lb = (int)strlen(b);
    if (abs_i(la - lb) > 1) return false;
    if (la == lb) {
        int diffs = 0;
        for (int i = 0; i < la; i++) if (a[i] != b[i]) if (++diffs > 1) return false;
        return true;
    }
    // Single insert or delete.
    const char* s;
    const char* t;
    if (la < lb) { s = a; t = b; } else { s = b; t = a; }
    int sl = (int)strlen(s), tl = (int)strlen(t);
    int i = 0, j = 0, diffs = 0;
    while (i < sl && j < tl) {
        if (s[i] == t[j]) { i++; j++; }
        else { j++; if (++diffs > 1) return false; }
    }
    return true;
}

static const char* did_you_mean(const char* cmd)
{
    if (!cmd || !*cmd) return NULL;
    for (size_t i = 0; i < g_cliCommandCount; i++) {
        const CliCommand* c = &g_cliCommands[i];
        if (c->category == CAT_HIDDEN) continue;
        if (edit_distance_le_1(cmd, c->name)) return c->name;
    }
    return NULL;
}

// ---------------------------------------------------------------------------
// Dispatch
// ---------------------------------------------------------------------------

static void handleCommand(const char* line)
{
    while (*line == ' ') line++;
    if (*line == '\0') return;

    char cmd[24] = {};
    int i = 0;
    while (*line && *line != ' ' && i < (int)sizeof(cmd) - 1) {
        cmd[i++] = *line++;
    }
    cmd[i] = '\0';
    while (*line == ' ') line++;
    const char* args = line;

    const CliCommand* entry = cli_cmdtab_find(cmd);
    if (entry && entry->handler) {
        entry->handler(args);
    } else {
        term_cprintf(TERM_FG_B_RED, "Unknown command: '%s'.", cmd);
        const char* hint = did_you_mean(cmd);
        if (hint) term_cprintf(TERM_FG_B_YELLOW, " Did you mean '%s'?", hint);
        term_println(" Type 'help' for available commands.");
    }
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void cliInit(AD74416H& device)
{
    g_cli_dev          = &device;
    s_showPrompt       = true;
    s_size_probed_once = false;
    term_init();
    cli_history_init();
    cli_edit_init();
    cli_tui_init();
}

void cliProcess()
{
    // If BBP binary mode is active, process binary protocol instead of CLI.
    if (bbpIsActive()) {
        bbpProcess();
        return;
    }

    // --------------------------------------------------------------
    // TUI dashboard mode
    // --------------------------------------------------------------
    if (cli_tui_active()) {
        cli_tui_tick();

        while (serial_available()) {
            uint8_t b = (uint8_t)serial_read();

            // 0xBB is never a legal TUI key. Preemptively unwind the
            // alt-screen so that if the next 3 bytes complete the BBP
            // handshake, the host terminal is already back to normal.
            if (b == 0xBB) cli_tui_preempt();

            if (bbpDetectHandshake(b)) {
                cli_tui_leave();
                cli_edit_reset();
                s_showPrompt = true;
                return;
            }
            if (b == 0xBB) continue;

            cli_tui_feed(b);

            if (cli_tui_want_exit()) {
                cli_tui_clear_want_exit();
                s_showPrompt = true;
                return;
            }
        }
        return;
    }

    // --------------------------------------------------------------
    // Normal line-editor mode
    // --------------------------------------------------------------
    if (s_showPrompt && !bbpCdcClaimed()) {
        emit_prompt_fresh();
        cli_edit_on_prompt_shown(PROMPT_VIS_COLS);

        if (!s_size_probed_once) {
            term_probe_size();
            s_size_probed_once = true;
        }
        s_showPrompt = false;
    }

    while (serial_available()) {
        uint8_t b = (uint8_t)serial_read();

        if (bbpDetectHandshake(b)) {
            cli_edit_reset();
            s_showPrompt = true;
            return;
        }
        if (b == 0xBB) continue;

        cli_edit_feed(b);

        if (cli_edit_need_reprompt() && !bbpCdcClaimed()) {
            emit_prompt_inline();
            cli_edit_redraw_line();
            cli_edit_clear_reprompt();
        }

        if (cli_edit_line_ready()) {
            const char* line = cli_edit_take_line();
            if (line && *line) cli_history_push(line);
            if (line) handleCommand(line);
            s_showPrompt = true;
            return;
        }

        // If a handler launched the TUI, bail out immediately so the next
        // cliProcess tick drives the dashboard instead of re-emitting a prompt.
        if (cli_tui_active()) {
            s_showPrompt = false;
            return;
        }
    }
}
