// =============================================================================
// cli_history.cpp - Command history ring for the CLI.
// =============================================================================

#include "cli_history.h"
#include "cli_term.h"

#include <string.h>

// ---------------------------------------------------------------------------
// Storage
// ---------------------------------------------------------------------------
static char s_ring[CLI_HISTORY_DEPTH][CLI_HISTORY_LINE_BUF];
static int  s_count    = 0;   // number of valid entries (0..CLI_HISTORY_DEPTH)
static int  s_head     = 0;   // next write slot (ring index)
static char s_scratch[CLI_HISTORY_LINE_BUF];

// Map a 1-based "offset from newest" to the physical ring index.
static int offset_to_index(int offset)
{
    // Most-recently-pushed entry is at (head - 1) mod DEPTH.
    // offset 1 -> (head - 1), offset 2 -> (head - 2), ...
    int idx = s_head - offset;
    while (idx < 0) idx += CLI_HISTORY_DEPTH;
    idx %= CLI_HISTORY_DEPTH;
    return idx;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------
void cli_history_init(void)
{
    s_count   = 0;
    s_head    = 0;
    s_scratch[0] = '\0';
    for (int i = 0; i < CLI_HISTORY_DEPTH; i++) s_ring[i][0] = '\0';
}

void cli_history_push(const char* line)
{
    if (!line || !*line) return;

    // Dedupe consecutive duplicates.
    if (s_count > 0) {
        int last = offset_to_index(1);
        if (strncmp(s_ring[last], line, CLI_HISTORY_LINE_BUF) == 0) return;
    }

    strncpy(s_ring[s_head], line, CLI_HISTORY_LINE_BUF - 1);
    s_ring[s_head][CLI_HISTORY_LINE_BUF - 1] = '\0';
    s_head = (s_head + 1) % CLI_HISTORY_DEPTH;
    if (s_count < CLI_HISTORY_DEPTH) s_count++;
}

int cli_history_count(void)
{
    return s_count;
}

const char* cli_history_peek(int offset)
{
    if (offset <= 0 || offset > s_count) return NULL;
    return s_ring[offset_to_index(offset)];
}

void cli_history_save_scratch(const char* line)
{
    if (!line) { s_scratch[0] = '\0'; return; }
    strncpy(s_scratch, line, CLI_HISTORY_LINE_BUF - 1);
    s_scratch[CLI_HISTORY_LINE_BUF - 1] = '\0';
}

const char* cli_history_scratch(void)
{
    return s_scratch;
}

void cli_history_print(void)
{
    term_println("");
    if (s_count == 0) {
        term_println("(history empty)");
        return;
    }
    // Print oldest to newest so the newest is at the bottom.
    for (int off = s_count; off >= 1; off--) {
        const char* line = cli_history_peek(off);
        if (!line) continue;
        term_printf("  %3d  %s\r\n", s_count - off + 1, line);
    }
}

extern "C" void cli_cmd_history(const char* args)
{
    (void)args;
    cli_history_print();
}
