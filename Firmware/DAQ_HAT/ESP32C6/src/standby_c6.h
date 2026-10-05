#pragma once

// =============================================================================
// standby_c6.h - ESP32-C6 binding of the standby / boot-progress model
// (standby_c6_core) to the display, the neopixels and the DDP link.
// =============================================================================

#include <stdbool.h>
#include <stdint.h>
#include "standby_c6_core.h"

#ifdef __cplusplus
extern "C" {
#endif

// Start the boot view. Call right after display_init()/gfx_init().
void standby_c6_init(uint32_t now_ms);

// A boot milestone decided on this chip (display, settings, C6 radio).
void standby_c6_local(uint16_t bit, sb_item_t result);

// Any CRC-valid frame from the P4 arrived: the link is up. The "Analyzer" milestone
// is NOT marked by this - only the P4's own bring-up report does that.
void standby_c6_p4_alive(void);

// DDP_CMD_STANDBY from the P4: payload is bb_standby_request_t (16 B). The reply
// (bb_standby_reply_t, 16 B) is written to reply_out.
void standby_c6_on_request(const uint8_t *payload, uint8_t *reply_out);

// Main-loop service: deadlines, panel/LED actions, inhibitors. Call every pass.
void standby_c6_service(uint32_t now_ms);

sb_ui_mode_t standby_c6_ui_mode(uint32_t now_ms);
bool standby_c6_input_blocked(uint32_t now_ms);

// A meaningful local event (menu key, setting commit, user-originated request).
void standby_c6_activity(void);

// Draw + flush the boot / wake screen when its content changed (or force).
void standby_c6_render(uint32_t now_ms, bool force);

// --- Auto Standby menu ------------------------------------------------------
// Text for the menu value column ("5 min", "Off", "...", "?", "no reply").
void standby_c6_policy_text(char *buf, int n);
// Radio-list selection: 0=1 min, 1=5 min, 2=15 min, 3=Off, -1 unknown/pending.
int  standby_c6_policy_selected(void);
// Ask the S3 (through the P4 mailbox) to change it. Nothing is persisted here.
void standby_c6_policy_choose(int idx);
// DDP_CMD_MB_RESPONSE for DDP_MB_STANDBY_POLICY: [type][status][u16 LE seconds].
void standby_c6_mb_response(const uint8_t *payload, uint8_t len);

#ifdef __cplusplus
}
#endif
