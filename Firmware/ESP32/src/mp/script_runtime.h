#pragma once
// =============================================================================
// script_runtime.h — pure-C pieces of the script runtime (spec 2026-10-03 §2).
//
// No FreeRTOS, no MicroPython: the run-state enums and their wire names, the
// single file-script slot admission rule, the structured log-line assembler
// (`<ts_ms> <L> <src> <text>\n`) and the query-string helpers shared by HTTP
// and the BLE tunnel (which carries `?name=` inside the path). Host-tested by
// tests/firmware_host/test_script_runtime.py.
// =============================================================================

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    SCRIPT_SRC_MANUAL  = 0,   // HTTP/BLE/BBP/C6 run-file or eval
    SCRIPT_SRC_AUTORUN = 1,   // autorun.py at boot or autorun run-now
    SCRIPT_SRC_REPL    = 2,   // browser REPL line
} ScriptSource;

typedef enum {
    SCRIPT_STATE_IDLE = 0,    // nothing has run since boot
    SCRIPT_STATE_RUNNING,
    SCRIPT_STATE_STOPPING,    // stop requested, script not yet returned
    SCRIPT_STATE_ERROR,       // last script raised
    SCRIPT_STATE_DONE,        // last script returned or was stopped
} ScriptState;

typedef enum {
    SCRIPT_EXIT_NONE = 0,
    SCRIPT_EXIT_OK,
    SCRIPT_EXIT_ERROR,
    SCRIPT_EXIT_STOPPED,
} ScriptExit;

const char *sr_source_name(ScriptSource s);   // "manual" | "autorun" | "repl"
const char *sr_state_name(ScriptState s);     // "idle" | "running" | "stopping" | "error" | "done"
const char *sr_exit_name(ScriptExit e);       // "none" | "ok" | "error" | "stopped"

// ---------------------------------------------------------------------------
// Single file-script slot. Evals and REPL lines never take the slot and are
// refused while a file script holds it; a file request may replace it.
// ---------------------------------------------------------------------------
typedef enum { SR_ADMIT_START = 0, SR_ADMIT_BUSY = 1, SR_ADMIT_REPLACE = 2 } sr_admit_t;

sr_admit_t sr_admit(bool file_slot_busy, bool request_is_file, bool replace);

// ---------------------------------------------------------------------------
// Structured log lines. Raw script output arrives in fragments; the assembler
// cuts it at '\n' (dropping '\r'), or every SR_LINE_MAX bytes, and emits one
// `<ts_ms> <L> <src> <text>\n` line per cut. The level is the one in force when
// the line completes; changing level first flushes a pending partial line.
// ---------------------------------------------------------------------------
#define SR_LINE_MAX 240

typedef void (*sr_emit_fn)(void *ctx, const char *line, size_t len);

typedef struct {
    char        buf[SR_LINE_MAX];
    size_t      len;
    char        level;
    const char *src;
} sr_line_asm_t;

void   sr_line_init(sr_line_asm_t *a, const char *src, char level);
void   sr_line_set_level(sr_line_asm_t *a, char level, uint32_t ts_ms, sr_emit_fn emit, void *ctx);
void   sr_line_feed(sr_line_asm_t *a, uint32_t ts_ms, const char *data, size_t len,
                    sr_emit_fn emit, void *ctx);
void   sr_line_flush(sr_line_asm_t *a, uint32_t ts_ms, sr_emit_fn emit, void *ctx);
/** Format one line into out (always NUL-terminated, always '\n'-ended when cap
 *  allows); text is truncated to fit. Returns bytes written excluding NUL. */
size_t sr_format_line(char *out, size_t cap, uint32_t ts_ms, char level, const char *src,
                      const char *text, size_t len);

// ---------------------------------------------------------------------------
// Paths and query strings.
// ---------------------------------------------------------------------------
/** True if path equals route, or equals route followed by '?'. */
bool sr_path_is(const char *path, const char *route);
/** Copy the URL-decoded value of `key` from path's query string ('+' = space,
 *  %XX decoded). False if absent or if it does not fit in cap (incl. NUL). */
bool sr_query_get(const char *path, const char *key, char *out, size_t cap);
/** True if `key` is present with value "1" or "true". */
bool sr_query_flag(const char *path, const char *key);

#ifdef __cplusplus
}
#endif
