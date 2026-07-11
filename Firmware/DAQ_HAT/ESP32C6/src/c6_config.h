#pragma once

// =============================================================================
// c6_config.h — bridge between the C6 settings_t and the registry TLV protocol.
//
// On a menu commit, c6_config_send() encodes the C6-tracked scalar settings as
// TLVs and sends them to the P4 (DDP_CMD_CONFIG_SET). When the P4 echoes a
// change made elsewhere (S3/desktop/web), ddp.c calls c6_config_apply_push() to
// fold the incoming TLVs back into g_settings and apply local side effects
// (backlight, theme). Key <-> field mapping is the single place that knows both
// the registry (common/daq_config_registry.h) and settings_t.
// =============================================================================

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Encode all C6-tracked scalar settings as back-to-back TLVs and send them to
// the P4 (C6 -> P4 CONFIG_SET event). Strings (ssid/password) are not sent from
// the C6 (they are app-set and only received).
void c6_config_send(void);

// Request the P4 to enable/disable the DUT supply (SOURCE_ENABLE). The supply
// is a P4-local resource, so this sends a single SOURCE_ENABLE TLV over DDP;
// the P4 applies it via smu_enable(). Used by the main-screen OK long-press.
void c6_config_send_source_enable(bool on);

// Apply one or more TLVs pushed by the P4 (CONFIG_PUSH) into g_settings, run
// side effects, and persist. @len is the payload byte count.
void c6_config_apply_push(const uint8_t *tlvs, uint16_t len);

#ifdef __cplusplus
}
#endif
