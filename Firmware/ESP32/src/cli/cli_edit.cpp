// =============================================================================
// cli_edit.cpp - Byte-level line editor state machine.
// =============================================================================

#include "cli_edit.h"
#include "cli_term.h"
#include "cli_history.h"
#include "cli_complete.h"
#include "cli_cmdtab.h"

#include <stdio.h>
#include <string.h>

// ---------------------------------------------------------------------------
// Parse state for escape-sequence handling.
// ---------------------------------------------------------------------------
typedef enum {
    ST_NORMAL = 0,
    ST_ESC,
    ST_CSI,
    ST_SS3,
} EditState;

// ---------------------------------------------------------------------------
// Module state
// ---------------------------------------------------------------------------
static char   s_buf[CLI_EDIT_BUF_SIZE];
static int    s_len          = 0;
static int    s_cursor       = 0;
static bool   s_line_ready   = false;
static bool   s_need_reprompt = false;
static int    s_prompt_cols  = 13;   // visible width of "[BugBuster]> "

static EditState s_state = ST_NORMAL;
static int       s_csi_params[4];
static int       s_csi_pn       = 0;
static bool      s_csi_has_digit = false;

static int       s_hist_offset = 0;  // 0 = editing, 1 = most recent, ...

// ---------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------

static void bell(void) { term_emit("\a", 1); }

// Echo a freshly-inserted character and redraw any tail after the cursor.
static void echo_insert(char c)
{
    if (s_cursor == s_len) {
        // Inserted at end. Cursor is already at s_len-1 before increment; we
        // call this after the insert so s_cursor == s_len == old_len+1. Just
        // emit the char.
        term_emit(&c, 1);
        return;
    }
    // Inserted in the middle. The character lives at s_buf[s_cursor-1].
    // Emit from there to end, then move cursor back to its logical position.
    term_emit(&s_buf[s_cursor - 1], s_len - (s_cursor - 1));
    int back = s_len - s_cursor;
    if (back > 0) term_cursor_back(back);
}

// Replace the entire edit line with `s` (used by history recall).
static void replace_line(const char* s)
{
    if (!s) s = "";
    // Move cursor to start of edit area
    if (s_cursor > 0) term_cursor_back(s_cursor);
    term_erase_to_eol();

    int n = (int)strlen(s);
    if (n >= CLI_EDIT_BUF_SIZE) n = CLI_EDIT_BUF_SIZE - 1;
    memcpy(s_buf, s, (size_t)n);
    s_buf[n] = '\0';
    s_len    = n;
    s_cursor = n;
    if (n > 0) term_emit(s_buf, (size_t)n);
}

// ---------------------------------------------------------------------------
// Edit ops
// ---------------------------------------------------------------------------

static void op_insert(char c)
{
    if (s_len >= CLI_EDIT_BUF_SIZE - 1) { bell(); return; }
    if (s_cursor < s_len) {
        memmove(&s_buf[s_cursor + 1], &s_buf[s_cursor], (size_t)(s_len - s_cursor));
    }
    s_buf[s_cursor] = c;
    s_cursor++;
    s_len++;
    s_buf[s_len] = '\0';
    echo_insert(c);
    s_hist_offset = 0;  // any real edit drops us out of history navigation
}

static void op_backspace(void)
{
    if (s_cursor == 0) { bell(); return; }
    if (s_cursor < s_len) {
        memmove(&s_buf[s_cursor - 1], &s_buf[s_cursor], (size_t)(s_len - s_cursor));
    }
    s_cursor--;
    s_len--;
    s_buf[s_len] = '\0';
    if (s_cursor == s_len) {
        term_emit("\b \b", 3);
    } else {
        term_emit("\b", 1);
        term_emit(&s_buf[s_cursor], (size_t)(s_len - s_cursor));
        term_emit(" ", 1);
        term_cursor_back((s_len - s_cursor) + 1);
    }
    s_hist_offset = 0;
}

static void op_delete_forward(void)
{
    if (s_cursor == s_len) { bell(); return; }
    memmove(&s_buf[s_cursor], &s_buf[s_cursor + 1], (size_t)(s_len - s_cursor - 1));
    s_len--;
    s_buf[s_len] = '\0';
    term_emit(&s_buf[s_cursor], (size_t)(s_len - s_cursor));
    term_emit(" ", 1);
    term_cursor_back((s_len - s_cursor) + 1);
    s_hist_offset = 0;
}

static void op_left(void)
{
    if (s_cursor == 0) return;
    s_cursor--;
    term_cursor_back(1);
}

static void op_right(void)
{
    if (s_cursor == s_len) return;
    s_cursor++;
    term_cursor_forward(1);
}

static void op_home(void)
{
    if (s_cursor == 0) return;
    term_cursor_back(s_cursor);
    s_cursor = 0;
}

static void op_end(void)
{
    if (s_cursor == s_len) return;
    term_cursor_forward(s_len - s_cursor);
    s_cursor = s_len;
}

static void op_kill_to_eol(void)
{
    if (s_cursor == s_len) return;
    s_len = s_cursor;
    s_buf[s_len] = '\0';
    term_erase_to_eol();
    s_hist_offset = 0;
}

static void op_kill_line(void)
{
    if (s_len == 0 && s_cursor == 0) return;
    if (s_cursor > 0) term_cursor_back(s_cursor);
    term_erase_to_eol();
    s_buf[0] = '\0';
    s_len    = 0;
    s_cursor = 0;
    s_hist_offset = 0;
}

// Ctrl-W — kill backward to start of previous word.
static void op_kill_back_word(void)
{
    if (s_cursor == 0) { bell(); return; }
    int c = s_cursor;
    while (c > 0 && s_buf[c - 1] == ' ') c--;
    while (c > 0 && s_buf[c - 1] != ' ') c--;
    int del = s_cursor - c;
    if (del == 0) { bell(); return; }
    memmove(&s_buf[c], &s_buf[s_cursor], (size_t)(s_len - s_cursor));
    s_cursor = c;
    s_len   -= del;
    s_buf[s_len] = '\0';
    // Redraw: back, tail, pad with spaces, back to cursor
    term_cursor_back(del);
    term_emit(&s_buf[s_cursor], (size_t)(s_len - s_cursor));
    for (int i = 0; i < del; i++) term_emit(" ", 1);
    term_cursor_back((s_len - s_cursor) + del);
    s_hist_offset = 0;
}

// Alt-B — move cursor back one word.
static void op_back_word(void)
{
    if (s_cursor == 0) return;
    int c = s_cursor;
    while (c > 0 && s_buf[c - 1] == ' ') c--;
    while (c > 0 && s_buf[c - 1] != ' ') c--;
    int delta = s_cursor - c;
    if (delta == 0) return;
    s_cursor = c;
    term_cursor_back(delta);
}

// Alt-F — move cursor forward one word.
static void op_forward_word(void)
{
    if (s_cursor == s_len) return;
    int c = s_cursor;
    while (c < s_len && s_buf[c] == ' ')  c++;
    while (c < s_len && s_buf[c] != ' ')  c++;
    int delta = c - s_cursor;
    if (delta == 0) return;
    s_cursor = c;
    term_cursor_forward(delta);
}

// Ctrl-L — clear screen, re-probe terminal size, redraw prompt.
static void op_ctrl_l(void)
{
    term_emit("\x1b[2J\x1b[H", 7);
    term_probe_size();
    s_need_reprompt = true;
}

static void op_history_up(void)
{
    int new_off = s_hist_offset + 1;
    const char* entry = cli_history_peek(new_off);
    if (!entry) { bell(); return; }
    if (s_hist_offset == 0) cli_history_save_scratch(s_buf);
    s_hist_offset = new_off;
    replace_line(entry);
}

static void op_history_down(void)
{
    if (s_hist_offset == 0) { bell(); return; }
    int new_off = s_hist_offset - 1;
    s_hist_offset = new_off;
    const char* entry = (new_off == 0) ? cli_history_scratch() : cli_history_peek(new_off);
    replace_line(entry ? entry : "");
}

static void op_ctrl_c(void)
{
    // Emit "^C", kill line, request a fresh prompt.
    term_println("^C");
    s_buf[0] = '\0';
    s_len    = 0;
    s_cursor = 0;
    s_hist_offset = 0;
    s_need_reprompt = true;
}

static void op_commit_line(void)
{
    // The caller will drain s_buf via cli_edit_take_line().
    term_emit("\r\n", 2);
    s_line_ready  = true;
    s_hist_offset = 0;
}

// Find the start of the token currently under the cursor.
static int current_token_start(void)
{
    int i = s_cursor;
    while (i > 0 && s_buf[i - 1] != ' ') i--;
    return i;
}

// Count tokens before the cursor; 0 = still editing the command name,
// 1 = first arg, 2 = second arg, ...
static int current_token_index(void)
{
    int idx = 0;
    bool in_word = false;
    for (int i = 0; i < s_cursor; i++) {
        if (s_buf[i] != ' ') {
            in_word = true;
        } else {
            if (in_word) { in_word = false; idx++; }
        }
    }
    // If we're past a space into a new (possibly empty) arg, idx already
    // reflects it. If we're still inside the current word, idx is the index
    // of that word.
    return idx;
}

static void replace_token_with(int tok_start, const char* replacement, bool add_space)
{
    int tail_len = s_len - s_cursor;
    char newline[CLI_EDIT_BUF_SIZE];
    int written = snprintf(newline, sizeof(newline), "%.*s%s%s",
                           tok_start, s_buf, replacement, add_space ? " " : "");
    if (written < 0) return;
    if ((size_t)(written + tail_len) >= sizeof(newline)) {
        tail_len = (int)sizeof(newline) - written - 1;
    }
    memcpy(&newline[written], &s_buf[s_cursor], (size_t)tail_len);
    newline[written + tail_len] = '\0';

    replace_line(newline);
    // Position cursor just after the replaced token + space (if added).
    int desired = written;
    if (s_cursor > desired) term_cursor_back(s_cursor - desired);
    else if (s_cursor < desired) term_cursor_forward(desired - s_cursor);
    s_cursor = desired;
}

// Complete a non-first-word argument using the command's complete() callback.
static void complete_argument(void)
{
    // First token gives us the command name.
    int cmd_end = 0;
    while (cmd_end < s_len && s_buf[cmd_end] != ' ') cmd_end++;
    if (cmd_end == 0) { bell(); return; }

    char cmdname[24];
    int nlen = cmd_end < 23 ? cmd_end : 23;
    memcpy(cmdname, s_buf, (size_t)nlen);
    cmdname[nlen] = '\0';

    const CliCommand* c = cli_cmdtab_find(cmdname);
    if (!c || !c->complete) { bell(); return; }

    int tok_start = current_token_start();
    int arg_idx   = current_token_index() - 1;  // 0 = first arg after cmd
    if (arg_idx < 0) arg_idx = 0;

    char prefix[48];
    int plen = s_cursor - tok_start;
    if (plen < 0) plen = 0;
    if (plen >= (int)sizeof(prefix)) plen = (int)sizeof(prefix) - 1;
    memcpy(prefix, &s_buf[tok_start], (size_t)plen);
    prefix[plen] = '\0';

    const char* matches[CLI_COMPLETE_MAX_MATCHES];
    int n = c->complete(prefix, arg_idx, matches, CLI_COMPLETE_MAX_MATCHES);
    if (n == 0) { bell(); return; }

    if (n == 1) {
        replace_token_with(tok_start, matches[0], /*add_space=*/true);
        return;
    }

    char common[48];
    int clen = cli_complete_common_prefix(matches, n, common, sizeof(common));
    if (clen > plen) {
        replace_token_with(tok_start, common, /*add_space=*/false);
        return;
    }

    // Multiple matches with no new common prefix — print list and redraw.
    term_emit("\r\n", 2);
    for (int i = 0; i < n; i++) {
        term_printf("  %s", matches[i]);
        if (((i + 1) % 6) == 0) term_emit("\r\n", 2);
    }
    if ((n % 6) != 0) term_emit("\r\n", 2);
    s_need_reprompt = true;
}

static void op_tab(void)
{
    // If the cursor is past a space, we're completing an argument.
    bool has_space_before_cursor = false;
    for (int i = 0; i < s_cursor; i++) {
        if (s_buf[i] == ' ') { has_space_before_cursor = true; break; }
    }
    if (has_space_before_cursor) {
        complete_argument();
        return;
    }

    // First-word completion.
    char prefix[48];
    int plen = s_cursor;
    if (plen >= (int)sizeof(prefix)) plen = (int)sizeof(prefix) - 1;
    memcpy(prefix, s_buf, (size_t)plen);
    prefix[plen] = '\0';

    const char* matches[CLI_COMPLETE_MAX_MATCHES];
    int n = cli_complete_first_word(prefix, matches, CLI_COMPLETE_MAX_MATCHES);

    if (n == 0) { bell(); return; }

    if (n == 1) {
        const char* name = matches[0];
        int namelen = (int)strlen(name);
        // Build line = name + " " + old tail after cursor
        char newline[CLI_EDIT_BUF_SIZE];
        int tail_len = s_len - s_cursor;
        int written  = snprintf(newline, sizeof(newline), "%s ", name);
        if (written < 0) return;
        if ((size_t)(written + tail_len) >= sizeof(newline)) {
            tail_len = (int)sizeof(newline) - written - 1;
        }
        memcpy(&newline[written], &s_buf[s_cursor], (size_t)tail_len);
        newline[written + tail_len] = '\0';

        // Redraw
        replace_line(newline);
        // Place cursor just after "name ": column = namelen + 1
        int desired = namelen + 1;
        if (s_cursor > desired) {
            term_cursor_back(s_cursor - desired);
        } else if (s_cursor < desired) {
            term_cursor_forward(desired - s_cursor);
        }
        s_cursor = desired;
        return;
    }

    // Multiple matches: try to extend to the longest common prefix; else list.
    char common[48];
    int clen = cli_complete_common_prefix(matches, n, common, sizeof(common));
    if (clen > plen) {
        // Extend the typed prefix to the common prefix.
        char newline[CLI_EDIT_BUF_SIZE];
        int tail_len = s_len - s_cursor;
        int written  = snprintf(newline, sizeof(newline), "%s", common);
        if (written < 0) return;
        if ((size_t)(written + tail_len) >= sizeof(newline)) {
            tail_len = (int)sizeof(newline) - written - 1;
        }
        memcpy(&newline[written], &s_buf[s_cursor], (size_t)tail_len);
        newline[written + tail_len] = '\0';

        replace_line(newline);
        if (s_cursor > written) term_cursor_back(s_cursor - written);
        else if (s_cursor < written) term_cursor_forward(written - s_cursor);
        s_cursor = written;
        return;
    }

    // Print the list and request a prompt redraw.
    term_emit("\r\n", 2);
    for (int i = 0; i < n; i++) {
        term_printf("  %s", matches[i]);
        if (((i + 1) % 6) == 0) term_emit("\r\n", 2);
    }
    if ((n % 6) != 0) term_emit("\r\n", 2);

    s_need_reprompt = true;
}

// ---------------------------------------------------------------------------
// CSI terminator dispatch
// ---------------------------------------------------------------------------

static void dispatch_csi(char final)
{
    // Handle tilde-family first
    if (final == '~' && s_csi_pn >= 1) {
        int p = s_csi_params[0];
        switch (p) {
            case 1: case 7: op_home(); break;          // Home
            case 4: case 8: op_end();  break;          // End
            case 3:         op_delete_forward(); break; // Delete
            default: /* ignore */ break;
        }
        return;
    }

    // Cursor Position Report reply: ESC [ <rows> ; <cols> R
    if (final == 'R' && s_csi_pn >= 2) {
        cli_term_on_size(s_csi_params[0], s_csi_params[1]);
        return;
    }

    switch (final) {
        case 'A': op_history_up();    break;
        case 'B': op_history_down();  break;
        case 'C': op_right();         break;
        case 'D': op_left();          break;
        case 'H': op_home();          break;
        case 'F': op_end();           break;
        default: /* ignore */         break;
    }
}

// ---------------------------------------------------------------------------
// Byte dispatch
// ---------------------------------------------------------------------------

static void feed_normal(uint8_t b)
{
    switch (b) {
        case 0x1B: s_state = ST_ESC; return;
        case 0x0D:
        case 0x0A:
            op_commit_line();
            return;
        case 0x09: op_tab();          return;
        case 0x7F:
        case 0x08: op_backspace();    return;
        case 0x03: op_ctrl_c();       return;
        case 0x01: op_home();         return;
        case 0x05: op_end();          return;
        case 0x02: op_left();         return;  // Ctrl-B
        case 0x06: op_right();        return;  // Ctrl-F
        case 0x0B: op_kill_to_eol();  return;  // Ctrl-K
        case 0x15: op_kill_line();    return;  // Ctrl-U
        case 0x17: op_kill_back_word(); return;// Ctrl-W
        case 0x10: op_history_up();   return;  // Ctrl-P
        case 0x0E: op_history_down(); return;  // Ctrl-N
        case 0x0C: op_ctrl_l();       return;  // Ctrl-L
        default: break;
    }

    if (b >= 0x20 && b <= 0x7E) {
        op_insert((char)b);
    }
    // All other control chars: silently dropped.
}

static void feed_esc(uint8_t b)
{
    if (b == '[') {
        s_state = ST_CSI;
        s_csi_pn = 0;
        s_csi_params[0] = s_csi_params[1] = s_csi_params[2] = s_csi_params[3] = 0;
        s_csi_has_digit = false;
        return;
    }
    if (b == 'O') {
        s_state = ST_SS3;
        return;
    }
    // Meta-key shortcuts: Alt-B / Alt-F word motion.
    if (b == 'b' || b == 'B') { op_back_word();    s_state = ST_NORMAL; return; }
    if (b == 'f' || b == 'F') { op_forward_word(); s_state = ST_NORMAL; return; }
    // Bare ESC or unknown meta prefix: abandon and retry the byte as NORMAL.
    s_state = ST_NORMAL;
    feed_normal(b);
}

static void feed_csi(uint8_t b)
{
    // Accumulate parameter digits and semicolons.
    if (b >= '0' && b <= '9') {
        if (s_csi_pn < 4) {
            s_csi_params[s_csi_pn] = s_csi_params[s_csi_pn] * 10 + (b - '0');
            s_csi_has_digit = true;
        }
        return;
    }
    if (b == ';') {
        if (s_csi_has_digit && s_csi_pn < 3) s_csi_pn++;
        s_csi_has_digit = false;
        return;
    }
    // Intermediate bytes 0x20..0x2F: ignore (rare)
    if (b >= 0x20 && b <= 0x2F) return;

    // Terminator: 0x40..0x7E
    if (b >= 0x40 && b <= 0x7E) {
        if (s_csi_has_digit) s_csi_pn++;  // finalize last param
        dispatch_csi((char)b);
        s_state = ST_NORMAL;
        return;
    }
    // Anything else: reset and drop.
    s_state = ST_NORMAL;
}

static void feed_ss3(uint8_t b)
{
    // SS3 P..S = F1..F4 — reserved for future use; no-op in Phase 3.
    (void)b;
    s_state = ST_NORMAL;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

extern "C" void cli_edit_init(void)
{
    s_buf[0]        = '\0';
    s_len           = 0;
    s_cursor        = 0;
    s_line_ready    = false;
    s_need_reprompt = false;
    s_state         = ST_NORMAL;
    s_csi_pn        = 0;
    s_csi_has_digit = false;
    s_hist_offset   = 0;
}

extern "C" void cli_edit_reset(void)
{
    s_buf[0]        = '\0';
    s_len           = 0;
    s_cursor        = 0;
    s_line_ready    = false;
    s_hist_offset   = 0;
    s_state         = ST_NORMAL;
    s_csi_pn        = 0;
    s_csi_has_digit = false;
}

extern "C" void cli_edit_on_prompt_shown(int visible_cols)
{
    s_prompt_cols = visible_cols;
    // Prompt just printed — start fresh edit.
    cli_edit_reset();
}

extern "C" void cli_edit_feed(uint8_t b)
{
    switch (s_state) {
        case ST_NORMAL: feed_normal(b); break;
        case ST_ESC:    feed_esc(b);    break;
        case ST_CSI:    feed_csi(b);    break;
        case ST_SS3:    feed_ss3(b);    break;
    }
}

extern "C" bool cli_edit_line_ready(void)
{
    return s_line_ready;
}

extern "C" const char* cli_edit_take_line(void)
{
    s_line_ready = false;
    return s_buf;
}

extern "C" bool cli_edit_need_reprompt(void)
{
    return s_need_reprompt;
}

extern "C" void cli_edit_clear_reprompt(void)
{
    s_need_reprompt = false;
}

extern "C" void cli_edit_redraw_line(void)
{
    if (s_len > 0) term_emit(s_buf, (size_t)s_len);
    int back = s_len - s_cursor;
    if (back > 0) term_cursor_back(back);
}
