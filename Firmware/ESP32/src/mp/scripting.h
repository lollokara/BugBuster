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
 * Copy src into a heap buffer and enqueue for execution.
 * persist=true keeps the MicroPython VM alive after eval (persistent mode).
 * In persistent mode, persist=false is a no-op (sticky until explicit reset).
 * Returns true if enqueued, false if the queue is full or src is NULL.
 * Thread-safe; callable from any context.
 */
bool scripting_run_string(const char *src, size_t len, bool persist);

/**
 * Load the named script file from SPIFFS and enqueue it for execution.
 * name must be a valid script name (validated by script_storage).
 * Returns true if the file was loaded and enqueued.
 * If out_id is non-NULL, the assigned script ID is written to *out_id.
 * Thread-safe; callable from any context.
 */
bool scripting_run_file(const char *name, uint32_t *out_id);

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

#ifdef __cplusplus
}
#endif
