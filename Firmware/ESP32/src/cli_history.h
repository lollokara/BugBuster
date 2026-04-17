#pragma once

// =============================================================================
// cli_history.h - Command history ring for the CLI.
//
// 32 entries x 256 bytes = 8 KB in .bss. Consecutive duplicates and empty
// lines are silently dropped. A "scratch slot" preserves the line the user
// was typing when they first press Up, so that pressing Down past the newest
// entry restores their in-progress edit.
// =============================================================================

#include <stddef.h>

#define CLI_HISTORY_DEPTH     32
#define CLI_HISTORY_LINE_BUF  256

#ifdef __cplusplus
extern "C" {
#endif

void        cli_history_init(void);

// Push a completed line into the ring (no-op for empty or dup-of-last).
void        cli_history_push(const char* line);

// How many entries are currently stored (0..CLI_HISTORY_DEPTH).
int         cli_history_count(void);

// Return the entry `offset` positions back:
//   offset = 0  -> NULL (reserved for "current editing line", handled by caller)
//   offset = 1  -> most-recent entry
//   offset = N  -> N-th most-recent (where N <= count())
// Returns NULL if offset is out of range.
const char* cli_history_peek(int offset);

// Save/restore the in-progress line the user was typing when they first
// pressed Up. The editor calls save_scratch before its first history recall.
void        cli_history_save_scratch(const char* line);
const char* cli_history_scratch(void);

// Dump the history ring to the terminal (handler for `history` command).
void        cli_history_print(void);

// CLI command handler registered in the command table.
void        cli_cmd_history(const char* args);

#ifdef __cplusplus
}
#endif
