#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "ddp_proto.h"
#include "version.h"

// DAQ HAT Display Protocol (DDP) UART slave. Listens for frames from the
// ESP32-P4, answers PING/GET_INFO, decodes SET_MEASUREMENT / SET_DIAGNOSTICS
// into shared snapshots for the UI, and can emit SET_CONFIG events upstream.

// Reported in RSP_INFO and relayed S3-ward as the C6's firmware version. These
// MUST track version.h -- they were hardcoded 0.2 until 2026-08-06, so the C6
// reported 0.2 no matter what was flashed. The DDP wire carries only
// major/minor, so the patch level is not visible upstream.
#define DDP_FW_MAJOR  FW_VERSION_MAJOR
#define DDP_FW_MINOR  FW_VERSION_MINOR
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

// Trigger a stateless one-shot operation on the P4 (daq_action_t).
void ddp_send_config_action(uint8_t action_id);

// Send a "Main Board Settings" request upstream to the P4 (C6 -> P4 -> S3).
// req_type is a DDP_MB_* value; args is the request-specific payload (may be
// NULL). The P4 forwards it to the S3 (only while not streaming) and relays the
// result back as DDP_CMD_MB_RESPONSE.
void ddp_send_mb_request(uint8_t req_type, const uint8_t *args, uint8_t args_len);

// Latest mainboard power snapshot (rail setpoints + e-fuse status) returned by
// the S3. Returns true if one has ever been received; *age_ms = ms since.
bool ddp_get_mb_power(ddp_mb_power_t *out, uint32_t *age_ms);

// Decoded MicroPython script snapshot returned by the S3 (list + engine state).
#define MB_SCRIPTS_MAX     12   // most script names shown on the C6
#define MB_SCR_NAME_MAX    24   // per-name display cap (names truncated to fit)
typedef struct {
    uint8_t state;                                   // DDP_MB_SCR_*
    char    err[40];                                 // last error message (may be "")
    uint8_t count;                                   // number of valid names
    char    name[MB_SCRIPTS_MAX][MB_SCR_NAME_MAX + 1];
} ddp_mb_scripts_t;

// Latest mainboard script snapshot returned by the S3. Returns true if one has
// ever been received; *age_ms = ms since the most recent.
bool ddp_get_mb_scripts(ddp_mb_scripts_t *out, uint32_t *age_ms);

// Latest firmware versions + GitHub release list relayed from the S3.
bool ddp_get_mb_fwinfo(ddp_mb_fwinfo_t *out, uint32_t *age_ms);

// DUT source calibration wizard (C6 -> P4 direct). op = DDP_CAL_OP_*, arg =
// mode for START (DDP_CAL_MODE_*). Every control frame also triggers a status
// push back from the P4.
void ddp_send_cal_ctrl(uint8_t op, uint8_t arg);

// Latest calibration status pushed by the P4. Returns true if one has ever been
// received; *age_ms = ms since the most recent.
bool ddp_get_cal_status(ddp_cal_status_t *out, uint32_t *age_ms);

// Announce our presence to the P4 (unsolicited RSP_INFO). Called periodically
// so the C6 and P4 discover each other regardless of boot order / a transient
// link drop. Safe to call when no P4 is attached (bytes are simply dropped).
void ddp_announce_presence(void);

// Return (and clear) the button events relayed from the P4 since the last call.
// Bits are DDP_BTN_* (== BTN_EV_*), so the result feeds menu_update() directly.
uint8_t ddp_take_buttons(void);

// True while the P4 has us in WiFi streaming mode (DDP_CMD_WIFI_STREAM_MODE):
// app_main should stop the normal readout/menu render and show the static
// streaming screen instead. Cleared back to false when the P4 signals exit.
bool ddp_wifi_stream_mode(void);
