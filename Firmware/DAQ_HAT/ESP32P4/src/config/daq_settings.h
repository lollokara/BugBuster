#pragma once

// =============================================================================
// daq_settings.h — ESP32-P4 authoritative settings store.
//
// The P4 is the single source of truth for every DAQ HAT setting (see
// common/daq_config_registry.h). Both control planes write here:
//   * S3 mainboard  via HAT-protocol CONFIG_* commands (desktop/web/mobile/MCP)
//   * C6 display     via DDP CONFIG_* events (on-device menu)
// and both read back the same values. A change from one side is applied to the
// owning subsystem and notified to the other side so they stay in sync.
//
// This module only stores/validates/persists values and invokes two callbacks:
//   - apply : push the value into the relevant subsystem (range/smu/dsp/...)
//   - notify: forward the change to the OTHER side(s)
// The board layer binds these; the store itself has no subsystem dependencies.
// =============================================================================

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "daq_config_registry.h"

#ifdef __cplusplus
extern "C" {
#endif

// Origin of a change, so notifications skip the side that made it.
typedef enum {
    DAQ_SRC_BOOT  = 0,   // applied during init (notify all)
    DAQ_SRC_S3    = 1,   // came from the S3 mainboard
    DAQ_SRC_C6    = 2,   // came from the C6 menu
    DAQ_SRC_LOCAL = 3,   // changed by P4 firmware itself
} daq_src_t;

// Apply a freshly-changed value to its subsystem. For scalar keys @sval is NULL
// and @ival holds the value; for DAQ_T_STR keys @sval is the string and @ival 0.
typedef void (*daq_settings_apply_cb_t)(uint16_t key, int32_t ival,
                                        const char *sval, void *user);

// Forward a change to the other control plane(s). @src identifies the origin so
// the callback can avoid echoing back to it.
typedef void (*daq_settings_notify_cb_t)(uint16_t key, daq_src_t src, void *user);

// Run a stateless action (energy/charge/factory reset). Return true on success.
typedef bool (*daq_settings_action_cb_t)(uint8_t action_id, void *user);

// Initialise the store: seed from schema defaults, then overlay NVS overrides.
void daq_settings_init(void);

// Register board callbacks. Any may be NULL.
void daq_settings_set_callbacks(daq_settings_apply_cb_t apply,
                                daq_settings_notify_cb_t notify,
                                daq_settings_action_cb_t action,
                                void *user);

// --- Scalar (bool/u8/i8/u16/i16/u32/i32/enum) accessors --------------------
bool daq_settings_get_i32(uint16_t key, int32_t *out);
// Validate+clamp+store+persist+apply+notify. Returns true if accepted.
bool daq_settings_set_i32(uint16_t key, int32_t value, daq_src_t src);

// --- String accessors ------------------------------------------------------
// Copies up to @cap-1 bytes + NUL into @buf. Returns true if the key is a string.
bool daq_settings_get_str(uint16_t key, char *buf, size_t cap);
bool daq_settings_set_str(uint16_t key, const char *val, daq_src_t src);

// --- TLV bridges (used by the link handlers) -------------------------------
// Apply one TLV (key/type/value). Returns true if accepted.
bool daq_settings_apply_tlv(const uint8_t *tlv, size_t len, daq_src_t src);
// Encode current value of @key as a single TLV. Returns bytes or -1.
int  daq_settings_encode_one(uint16_t key, uint8_t *buf, size_t cap);
// Encode EVERY setting as back-to-back TLVs (for CONFIG_GET_ALL). Secret values
// (e.g. wifi password) are emitted with an empty value unless @include_secret.
int  daq_settings_encode_all(uint8_t *buf, size_t cap, bool include_secret);

// --- Actions ---------------------------------------------------------------
bool daq_settings_action(uint8_t action_id, daq_src_t src);

// Re-apply every setting to its subsystem (call once after callbacks are bound).
void daq_settings_apply_all(void);

#ifdef __cplusplus
}
#endif
