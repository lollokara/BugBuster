#pragma once
// =============================================================================
// scripting.h — Public API for the MicroPython on-device scripting engine.
//
// Phase 2: persistent VM, VFS, file-run, HTTP/BBP transport.
// VFS enables `import` from /spiffs/scripts/.
// DO NOT include MicroPython headers here — would create circular includes.
// =============================================================================

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "script_runtime.h"   // ScriptSource / ScriptState / ScriptExit (pure C)
#include "script_storage.h"   // SCRIPT_NAME_MAX


#ifdef __cplusplus
extern "C" {
#endif

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

/** Allocate GC heap, create queue and FreeRTOS task. Call once after cmd_registry_init(). */
void scripting_init(void);

// ---------------------------------------------------------------------------
// Interpreter persistence mode
// ---------------------------------------------------------------------------

typedef enum {
    SCRIPTING_MODE_EPHEMERAL  = 0,  // Default: gc_init/mp_init/mp_deinit per eval
    SCRIPTING_MODE_PERSISTENT = 1,  // VM stays alive across evals; reset on idle/watermark
} ScriptingMode;

// ---------------------------------------------------------------------------
// Script submission
// ---------------------------------------------------------------------------

/**
 * Enqueue an eval (source = manual, name "<eval>"). Thin wrapper over
 * scripting_submit(); refused (false) while a file script holds the slot.
 * persist=true keeps the MicroPython VM alive after eval (persistent mode).
 * Thread-safe; callable from any context.
 */
bool scripting_run_string(const char *src, size_t len, bool persist);

/**
 * Compile a Python source string to verify its syntax.
 * Enqueues a lint job to the MicroPython task queue and blocks until completed.
 * Returns true if syntax is valid, false otherwise (writing the error to out_err).
 */
bool scripting_lint_string(const char *src, size_t len, char *out_err, size_t max_err);

/**
 * Load the named script file from SPIFFS and enqueue it (source = manual,
 * takes the file-script slot, never replaces). Wrapper over
 * scripting_submit_file(). Returns true if enqueued.
 */
bool scripting_run_file(const char *name, uint32_t *out_id);

// ---------------------------------------------------------------------------
// Runtime v2 submission (spec 2026-10-03 §2)
// ---------------------------------------------------------------------------

typedef enum {
    SCRIPT_SUBMIT_OK = 0,
    SCRIPT_SUBMIT_BUSY,         // a file/autorun script holds the slot (HTTP 409)
    SCRIPT_SUBMIT_QUEUE_FULL,   // queue full or out of memory
    SCRIPT_SUBMIT_NOT_FOUND,    // file missing, unreadable or empty
    SCRIPT_SUBMIT_DISABLED,     // engine not initialised or bad arguments
} ScriptSubmitResult;

typedef struct {
    const char  *name;     // NULL -> "<repl>" for SCRIPT_SRC_REPL, else "<eval>"
    ScriptSource source;
    bool         is_file;  // takes the single file-script slot
    bool         persist;  // persistent VM (see scripting_run_string)
    bool         replace;  // is_file only: stop the holder first (MP_REPLACE_STOP_TIMEOUT_MS, then VM reset)
} ScriptSubmitOpts;

/** Copy src and enqueue it under the single-slot rule. *out_id gets the id on OK. */
ScriptSubmitResult scripting_submit(const char *src, size_t len, const ScriptSubmitOpts *opts,
                                    uint32_t *out_id);

/** Read /scripts/<name> and submit it as a file script. */
ScriptSubmitResult scripting_submit_file(const char *name, ScriptSource source, bool replace,
                                         uint32_t *out_id);

/** Block until no file script holds the slot, or timeout. True if free. */
bool scripting_wait_slot_free(uint32_t timeout_ms);

/** Write one `<ts> <level> sys <text>` line to the log ring (and ESP_LOGI). */
void scripting_log_event(char level, const char *fmt, ...) __attribute__((format(printf, 2, 3)));


// ---------------------------------------------------------------------------
// Control
// ---------------------------------------------------------------------------

/** Request the running script to stop. Sets a volatile flag polled by the VM. */
void scripting_stop(void);

// ---------------------------------------------------------------------------
// Log ring
// ---------------------------------------------------------------------------

/**
 * Drain up to `max` bytes from the log ring into `out`.
 * Returns number of bytes copied. Clears the drained bytes from the ring.
 * Thread-safe.
 */
size_t scripting_get_logs(char *out, size_t max);

/**
 * Copy log bytes written at or after absolute offset `since` without draining.
 * `out_next` receives the next absolute offset to request. If `since` is older
 * than the retained ring window, copying starts from the oldest retained byte.
 * Thread-safe.
 */
size_t scripting_get_logs_since(char *out, size_t max, uint64_t since, uint64_t *out_next);

/**
 * Push `len` bytes of script stdout into the log ring.
 * Called by mp_hal_stdout_tx_strn — also tees to stderr for IDF console.
 * Thread-safe.
 */
void scripting_log_push(const char *str, size_t len);

/** Observer of every finished structured log line ("<ts_ms> <L> <src> <text>\n"). Called with the
 *  script log mutex held: it must never block. NULL removes it. */
typedef void (*scripting_log_tee_fn)(const char *line, size_t len);
void scripting_set_log_tee(scripting_log_tee_fn fn);


// ---------------------------------------------------------------------------
// Stop flag accessor (for mphalport.c and the VM hook)
// ---------------------------------------------------------------------------

/** Returns true if scripting_stop() has been called and the flag is still set. */
bool scripting_stop_requested(void);

// ---------------------------------------------------------------------------
// VM cooperative stop hook (called from mpconfigport.h VM hook macros)
// ---------------------------------------------------------------------------

/** Raises KeyboardInterrupt inside the VM if stop was requested and nlr_top != NULL. */
void scripting_vm_hook(void);

// ---------------------------------------------------------------------------
// Status
// ---------------------------------------------------------------------------

typedef struct {
    bool     is_running;
    uint32_t current_script_id;
    uint32_t last_script_id;    // ID of the last completed script (0 on cold boot)
    uint32_t total_runs;
    uint32_t total_errors;
    char     last_error_msg[64];
    // V2-A persistent-mode fields (zero in EPHEMERAL mode)
    ScriptingMode mode;              // current interpreter persistence mode
    uint32_t globals_bytes_est;      // estimated bytes used by global dict (0 when VM idle)
    uint32_t globals_count;          // number of entries in the global dict
    uint32_t auto_reset_count;       // how many times watermark/idle triggered auto-reset
    uint32_t last_eval_at_ms;        // xTaskGetTickCount() ms of last eval enqueue
    uint32_t idle_for_ms;            // ms since last eval (0 when running)
    bool     watermark_soft_hit;     // true if GC heap >= MP_HEAP_SOFT_WATERMARK_PCT
    // Script runtime v2 (spec 2026-10-03 §2) — appended; existing fields unchanged.
    char         name[SCRIPT_NAME_MAX + 1];   // running/last script: file name, "<eval>", "<repl>"
    ScriptSource source;
    ScriptState  state;
    ScriptExit   last_exit;
    uint32_t     started_at;                  // epoch s when started_at_epoch, else uptime ms
    bool         started_at_epoch;
    uint32_t     file_slot_id;                // id holding the single file-script slot, 0 = free
    char         file_slot_name[SCRIPT_NAME_MAX + 1];

} ScriptStatus;

void scripting_get_status(ScriptStatus *out);

// ---------------------------------------------------------------------------
// VM reset (persistent mode only)
// ---------------------------------------------------------------------------

/**
 * Request an immediate VM teardown and re-init.
 * No-op in EPHEMERAL mode.  Safe to call from any context.
 */
void scripting_reset_vm(void);

// ---------------------------------------------------------------------------
// IO-ownership session identity (IO_OWNER_SCRIPT caller ID)
// ---------------------------------------------------------------------------

/**
 * Monotonic session counter, incremented once per vm_do_init() call.
 * Used by MicroPython IO-ownership bindings to fill io_owner_t.session_id.
 * Wraps at 255 (uint8_t), which is intentional — only distinguishes
 * back-to-back sessions, not long-term identity.
 * Thread-safe (atomic read, written only from taskMicroPython).
 */
uint8_t scripting_get_mp_session(void);

#ifdef __cplusplus
}
#endif
