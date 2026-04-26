#pragma once
// =============================================================================
// scripting.h — Public API for the MicroPython on-device scripting engine.
//
// Phase 1: in-memory eval only. No VFS, no file-run, no HTTP transport yet.
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
// Script submission
// ---------------------------------------------------------------------------

/**
 * Copy src into a heap buffer and enqueue for execution.
 * Returns true if enqueued, false if the queue is full or src is NULL.
 * Thread-safe; callable from any context.
 */
bool scripting_run_string(const char *src, size_t len);

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
} ScriptStatus;

void scripting_get_status(ScriptStatus *out);

#ifdef __cplusplus
}
#endif
