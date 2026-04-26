#pragma once
// =============================================================================
// autorun.h — Phase 6b: autorun.py three-gate safety + OTA rollback
//
// Three gates (all must pass):
//   1. Sentinel file  /spiffs/.autorun_enabled  must exist
//   2. 5-second boot grace window — any inbound activity cancels autorun
//   3. IO12 must read HIGH at boot. Default (internal pull-up, IO12 floating)
//      reads HIGH → autorun runs. Pull IO12 LOW (jumper to GND / button /
//      external pull-down) to suppress autorun: hold-LOW-to-disable.
//
// OTA integration: esp_ota_mark_app_valid_cancel_rollback() is called by
// autorun_boot_check() AFTER the grace window, so a crash before that point
// causes the bootloader to roll back to the previous OTA slot.
// =============================================================================

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// ---------------------------------------------------------------------------
// Status snapshot
// ---------------------------------------------------------------------------

typedef struct {
    bool     enabled;        // sentinel /spiffs/.autorun_enabled exists
    bool     has_script;     // /spiffs/autorun.py exists
    bool     io12_high;      // IO12 sample at status query. true = HIGH = gate
                             // PASSES (autorun runs); false = LOW = gate BLOCKS.
                             // Default with no jumper (internal pull-up) → true.
    bool     last_run_ok;    // true if most recent autorun completed without error
    uint32_t last_run_id;    // script_id of last autorun attempt (0 = never)
} AutorunStatus;

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

/**
 * Called once from app_main AFTER mainLoopTask is started.
 * Runs the 5-second grace window, reads IO12, optionally runs autorun.py,
 * then calls esp_ota_mark_app_valid_cancel_rollback().
 */
void autorun_boot_check(void);

/**
 * Enable autorun: copy /spiffs/scripts/<script_name> → /spiffs/autorun.py
 * and create sentinel /spiffs/.autorun_enabled.
 * Returns true on success; fills err on failure.
 */
bool autorun_set_enabled(const char *script_name, char *err, size_t err_len);

/**
 * Disable autorun: remove sentinel /spiffs/.autorun_enabled.
 * autorun.py is left in place (non-destructive).
 * Returns true on success; fills err on failure.
 */
bool autorun_set_disabled(char *err, size_t err_len);

/**
 * Fill *out with current autorun state.
 */
void autorun_get_status(AutorunStatus *out);

/**
 * Manually trigger autorun.py immediately (ignores gates).
 * Returns true if the script was enqueued; fills out_id with script_id.
 */
bool autorun_run_now(uint32_t *out_id, char *err, size_t err_len);

/**
 * Record inbound activity (BBP frame, HTTP request, CLI input).
 * Called from transport entry points to cancel the boot grace window.
 */
void autorun_note_inbound(void);

#ifdef __cplusplus
}
#endif
