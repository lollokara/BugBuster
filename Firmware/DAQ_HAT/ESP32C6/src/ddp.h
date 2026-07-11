#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "ddp_proto.h"

// DAQ HAT Display Protocol (DDP) UART slave. Listens for frames from the
// ESP32-P4, answers PING/GET_INFO, decodes SET_MEASUREMENT / SET_DIAGNOSTICS
// into shared snapshots for the UI, and can emit SET_CONFIG events upstream.

#define DDP_FW_MAJOR  0
#define DDP_FW_MINOR  2
#define DDP_HAT_TYPE  0x10   // DAQ display HAT

void ddp_init(void);

// Latest decoded measurement. Returns true if at least one valid
// SET_MEASUREMENT has ever been received; *age_ms is filled with how long ago
// (in ms) the most recent one arrived.
bool ddp_get_latest(float *v, float *i, uint8_t *flags, uint32_t *age_ms);

// Latest diagnostics snapshot pushed by the P4. Returns true if one has ever
// been received; *age_ms = ms since the most recent (0xFFFFFFFF if never).
bool ddp_get_diag(ddp_diag_t *out, uint32_t *age_ms);

// Send the current device settings upstream to the P4 (C6 -> P4 event). Safe
// to call when no P4 is attached (bytes are simply not received).
void ddp_send_config(const ddp_config_t *cfg);

// Send a key-addressed TLV batch upstream to the P4 (DDP_CMD_CONFIG_SET). Used
// by c6_config_send() when the user commits a menu edit.
void ddp_send_config_tlv(const uint8_t *tlvs, uint8_t len);

// Announce our presence to the P4 (unsolicited RSP_INFO). Called periodically
// so the C6 and P4 discover each other regardless of boot order / a transient
// link drop. Safe to call when no P4 is attached (bytes are simply dropped).
void ddp_announce_presence(void);

// Return (and clear) the button events relayed from the P4 since the last call.
// Bits are DDP_BTN_* (== BTN_EV_*), so the result feeds menu_update() directly.
uint8_t ddp_take_buttons(void);
