#pragma once

// =============================================================================
// s3_link.h — ESP32-S3 mainboard link: HAT-protocol slave on the P4 DAQ board.
//
// Implements the existing BugBuster HAT protocol (Firmware/HAT_Protocol.md) so
// the S3 mainboard can drive the DAQ HAT exactly as it drove the RP2040 HAT:
//
//   Frame: [SYNC 0xAA][LEN][CMD][PAYLOAD 0..32][CRC8]   (CRC over CMD+PAYLOAD)
//   S3 = master (sends commands), P4 = slave (sends responses).
//   The P4 may assert the open-drain IRQ line LOW to request a poll, but never
//   transmits unsolicited frames.
//
// On boot the S3 detects the HAT, sends GET_INFO, reads hat_type =
// HAT_TYPE_DAQ_POWER, and dynamically loads the DAQ resource set (no hotswap).
//
// Standard commands handled here: PING, GET_INFO, GET_CAPS, RESET.
// DAQ-specific commands (0x50..0x5F) are dispatched to the board via a callback.
// =============================================================================

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#ifdef __cplusplus
extern "C" {
#endif

// Framing constants (match HAT_Protocol.md). The LEN field is a full byte, so
// the DAQ link raises the payload cap above the legacy 32-byte convention to
// carry OTA metadata (56 B) and stream firmware in larger chunks. Standard
// control commands still use small (<= 32 B) payloads for compatibility.
#define HATP_SYNC            0xAAu
#define HATP_MAX_PAYLOAD     240u
#define HATP_CRC_POLY        0x07u

// Largest firmware chunk per OTA_DATA frame: payload = offset(u32) + data.
#define HATP_OTA_CHUNK_MAX   (HATP_MAX_PAYLOAD - 4u)

// OTA target selector (optional trailing byte of OTA_BEGIN; absent => P4 self).
// When 1, the image is flashed to the on-module ESP32-C6 via the P4 ROM-loader.
#define HATP_OTA_TARGET_P4   0u
#define HATP_OTA_TARGET_C6   1u
// When 2, the image is written into the P4 `staging` partition only (no local
// P4/C6 flash write) so it can later be relayed to C6 (via relay_c6) or pulled
// by the S3 itself (via HATP_CMD_STAGE_READ) instead of being applied directly.
#define HATP_OTA_TARGET_STAGE 2u

// Standard commands (master -> slave).
#define HATP_CMD_PING        0x01u
#define HATP_CMD_GET_INFO    0x02u
#define HATP_CMD_RESET       0x05u
#define HATP_CMD_GET_CAPS    0x06u

// DAQ power-analyzer specific commands (vendor sub-range 0x50..0x5F).
#define HATP_CMD_DAQ_START       0x50u   // start acquisition (payload: optional)
#define HATP_CMD_DAQ_STOP        0x51u   // stop acquisition
#define HATP_CMD_DAQ_SET_SOURCE  0x52u   // payload: vdut(f32) ilimit(f32) en(u8)
#define HATP_CMD_DAQ_GET_STATUS  0x53u   // -> range/streaming/energy summary
#define HATP_CMD_DAQ_SYNC        0x54u   // sync epoch (pre/post acquisition)
#define HATP_CMD_SET_CH_LEDS     0x55u   // 4x u8 channel colour codes -> C6 neopixels

// SMU factory calibration (vendor sub-range 0x56..0x59).
#define HATP_CMD_DAQ_CAL_START   0x56u   // payload: mode u8 (0=voltage,1=current)
#define HATP_CMD_DAQ_CAL_ACK     0x57u   // operator acknowledged the prompt
#define HATP_CMD_DAQ_CAL_STATUS  0x58u   // -> smu_cal_status_t
#define HATP_CMD_DAQ_CAL_ABORT   0x59u   // abort + restore safe SMU state

// S3 mainboard telemetry push for the C6 Diagnostics menu (die temp, USB-PD
// contract, VADJ/VLOGIC rails). Payload = s3link_telemetry_t; P4 caches it and
// relays it to the C6 inside ddp_diag_t. Fire-and-forget (-> RSP_OK).
#define HATP_CMD_DAQ_TELEMETRY   0x5Au

// Trigger / flag support. The S3 owns the 12 mainboard IOs and detects edge
// events (digital GPIO ISR or AD74416H analog/comparator); the P4 owns the
// sample clock and emits USB MARKER records aligned to the live sample index.
#define HATP_CMD_DAQ_ARM         0x5Bu   // payload: s3link_daq_arm_t (arm/disarm pre-roll latch)
#define HATP_CMD_DAQ_MARK        0x5Cu   // payload: s3link_daq_mark_t (IO event -> emit MARKER)

// Mainboard settings tunnel (C6 <-> P4 <-> S3). The S3 polls the P4 for a
// pending C6 request (MB_POLL) and returns execution results (MB_RESULT). The
// P4 defers MB_POLL (returns an empty RSP_MB_REQ) while it is streaming to the
// PC, so the tunnel never runs during acquisition.
#define HATP_CMD_MB_POLL         0x5Du   // S3->P4: fetch pending C6 request -> RSP_MB_REQ (or 0-len)
#define HATP_CMD_MB_RESULT       0x5Eu   // S3->P4: [type][status][seq][flags][data] chunk -> reassemble+relay to C6

// HATP_CMD_MB_RESULT chunk framing (MUST match ESP32 hat.h HAT_MB_RSLT_*).
#define HATP_MB_RSLT_HDR         4u      // [type][status][seq][flags]
#define HATP_MB_RSLT_LAST        0x01u   // flags bit: final chunk

// DAQ WiFi streaming bring-up (S3->P4). MUST stay byte-for-byte identical to
// the S3 mainboard's own mirror (HAT_CMD_DAQ_WIFI_STREAM_START/STOP/INFO in
// Firmware/ESP32/src/hat/hat.h) -- that side is already built and live.
#define HATP_CMD_DAQ_WIFI_STREAM_START  0x5Fu   // S3->P4: start DAQ WiFi streaming (empty payload) -> 1-byte accept/reject
#define HATP_CMD_DAQ_WIFI_STREAM_STOP   0x67u   // S3->P4: stop DAQ WiFi streaming (empty payload) -> 1-byte ack
#define HATP_CMD_DAQ_WIFI_STREAM_INFO   0x68u   // S3->P4: poll for wifi-stream credentials -> chunked RSP_DAQ_WIFI_STREAM_INFO
#define HATP_CMD_DAQ_WIFI_STREAM_RECYCLE  0x79u   // S3->P4: unconditional teardown to IDLE (empty payload) -> 1-byte ack

// HATP_CMD_DAQ_WIFI_STREAM_INFO chunk framing: [u8 status][u8 seq][u8 flags][data...].
// status: 0=starting (softAP not up yet, empty data), 1=ready (final chunk,
// flags has LAST set, data is the tail of the blob), 2=failed.
#define HATP_WIFI_INFO_HDR         3u      // [status][seq][flags]
#define HATP_WIFI_INFO_LAST        0x01u   // flags bit: final chunk
#define HATP_WIFI_INFO_ST_STARTING 0u
#define HATP_WIFI_INFO_ST_READY    1u
#define HATP_WIFI_INFO_ST_FAILED   2u

// The S3's real per-frame wire limit (MUST match ESP32 hat.h HAT_FRAME_MAX_LEN)
// -- unlike HATP_MAX_PAYLOAD (this project's own larger local buffer budget),
// this is what actually fits in one frame to the S3. Chunked P4-initiated
// responses (this command) must size against this, not HATP_MAX_PAYLOAD.
#define HAT_WIRE_FRAME_MAX_LEN     32u

// Multi-MCU OTA orchestration (S3-initiated request + P4 reply, modeled on
// HATP_CMD_DAQ_VDUT_STATUS — NOT a push). MUST stay byte-for-byte identical to
// the HAT_CMD_DAQ_* mirrors in Firmware/ESP32/src/hat/hat.h.
//
// This is deliberately ONE new command, not three. Two of the three originally
// planned commands duplicated surface that already exists on this link:
//   - a "DAQ_OTA_STATUS" would have shadowed HATP_CMD_OTA_STATUS (0x65), which
//     is already dispatched and already special-cases the C6 target. It only
//     lacked the relay_stage fields, so 0x65's reply was widened instead (see
//     s3link_ota_status_t) rather than adding a second status opcode.
//   - a "DAQ_FW_INFO" would have shadowed HATP_CMD_GET_VERSION (0x60), which
//     already reports the P4's own version. Only the C6's version was actually
//     missing, so 0x6A answers for the C6 alone.
// Two opcodes returning the same state is how the two sides drift apart.
#define HATP_CMD_DAQ_RELAY_APPLY 0x7Au   // no payload: apply the staged C6 image
#define HATP_CMD_DAQ_C6_VERSION  0x6Au   // no payload -> s3link_c6_version_t

// Version + OTA commands (vendor sub-range 0x60..0x6F).
//
// NOTE: HATP_CMD_DAQ_WIFI_STREAM_STOP (above) reuses byte 0x67, which used to
// be HATP_CMD_OTA_ROLLBACK in this file. The S3 mainboard's hat.h already
// hardcodes 0x67 for WIFI_STREAM_STOP (built/verified independently of this
// file), and nothing on the S3 side currently sends OTA_ROLLBACK over this
// link (grepped clean), so OTA_ROLLBACK was moved to the next free byte
// (0x69) instead of colliding on the wire.
#define HATP_CMD_GET_VERSION     0x60u   // -> fw version (u32 + string)
#define HATP_CMD_OTA_BEGIN       0x61u   // payload: ota_meta (size,ver,sha,prod)
#define HATP_CMD_OTA_DATA        0x62u   // payload: firmware bytes chunk
#define HATP_CMD_OTA_END         0x63u   // finalise + verify
#define HATP_CMD_OTA_ABORT       0x64u   // abort in-progress update
#define HATP_CMD_OTA_STATUS      0x65u   // -> ota progress/state
#define HATP_CMD_OTA_CONFIRM     0x66u   // confirm running image (cancel rollback)
#define HATP_CMD_OTA_ROLLBACK    0x69u   // revert to previous image (reboots) -- moved from 0x67, see note above

// Relay staging: S3 pulls a previously-staged image back out of the P4's
// `staging` partition (see relay_stage.h) in order to feed its own
// esp_ota_write() path — the P4 never pushes to the S3 unsolicited, since
// this link is strictly S3-master/P4-slave.
#define HATP_CMD_STAGE_READ      0x75u   // payload: s3link_stage_read_req_t

// VDUT (programmable DUT power supply, smu.{c,h}) request/reply commands.
// S3-initiated request + P4 reply, modeled on HATP_CMD_STAGE_READ above (NOT
// the one-way HATP_CMD_DAQ_TELEMETRY push pattern) -- the S3 doesn't own this
// hardware, it has to ask the P4 for status and issue enable/setpoint writes.
// MUST match S3 mainboard hat.h HAT_CMD_DAQ_VDUT_* exactly.
#define HATP_CMD_DAQ_VDUT_STATUS   0x76u   // no payload -> HATP_RSP_DAQ_VDUT_STATUS
#define HATP_CMD_DAQ_VDUT_ENABLE   0x77u   // payload: u8 enable -> OK/ERROR
#define HATP_CMD_DAQ_VDUT_SETPOINT 0x78u   // payload: s3link_vdut_setpoint_t -> OK/ERROR

// Acquisition configuration (ADAQ7769-1 digital filter + hardware decimation,
// which per the current design IS the sample rate -- see s3link_acq_config_t
// below for why there is no separate "sample rate" field). Applied to the two
// CURRENT ADAQs (FINE + COARSE) only, via the P4's ctrl_queue path (like HATP
// internal SET_RATE), never inline on this dispatcher -- bracketed by a
// stop_fast/run_fast pause exactly like apply_adaq_filter() in
// daq_settings_glue.c, both to release the SPI bus the capture task holds for
// the whole session and to force FINE/COARSE fusion to relearn its pairing
// offset on the new session. VOLTAGE is deliberately left alone: it shares
// SPI bus B and one common SYNC line with COARSE and cannot be
// phase-staggered (bench-verified, config.h VOLTAGE_ODR_TARGET_SPS), so it
// always tracks its own fixed target rate regardless of this command. MUST
// match S3 mainboard hat.h HAT_CMD_DAQ_SET_ACQ_CONFIG exactly.
#define HATP_CMD_DAQ_SET_ACQ_CONFIG 0x7Du   // payload: s3link_acq_config_t -> OK/ERROR

// Settings/config commands (vendor sub-range 0x70..0x7F). These read/write the
// authoritative settings store (common/daq_config_registry.h) using key-
// addressed TLV values, so the S3 (desktop/web/mobile/MCP) can configure every
// DAQ setting and read it back. Mirrors the C6 DDP config protocol.
#define HATP_CMD_CONFIG_GET      0x70u   // payload: key u16 LE        -> RSP_CONFIG_VALUE (one TLV)
#define HATP_CMD_CONFIG_SET      0x71u   // payload: one TLV           -> OK / ERROR
#define HATP_CMD_CONFIG_GET_ALL  0x72u   // payload: start_idx u8, flags u8 -> RSP_CONFIG_VALUE ([next_idx u8][TLVs])
#define HATP_CMD_CONFIG_SCHEMA   0x73u   // payload: key u16 LE        -> RSP_CONFIG_SCHEMA
#define HATP_CMD_CONFIG_ACTION   0x74u   // payload: action_id u8      -> OK / ERROR

// CONFIG_GET_ALL flags.
#define HATP_CONFIG_FLAG_SECRET  0x01u   // include secret values (e.g. wifi pw)

// Responses (slave -> master).
#define HATP_RSP_OK          0x80u
#define HATP_RSP_ERROR       0x81u
#define HATP_RSP_INFO        0x82u
#define HATP_RSP_CAPS        0x87u
#define HATP_RSP_DAQ_STATUS  0x90u   // DAQ status payload
#define HATP_RSP_VERSION     0x91u   // version payload
#define HATP_RSP_OTA_STATUS  0x92u   // OTA status payload
#define HATP_RSP_CONFIG_VALUE  0x93u // TLV value(s); GET_ALL prefixes [next_idx u8]
#define HATP_RSP_CONFIG_SCHEMA 0x94u // schema descriptor for one key
#define HATP_RSP_DAQ_CAL_STATUS 0x95u // smu_cal_status_t snapshot
#define HATP_RSP_MB_REQ        0x96u // pending C6 mainboard request ([req_type][args]); 0-len = none
#define HATP_RSP_STAGE_DATA    0x97u // payload: firmware bytes read from `staging` at the requested offset
#define HATP_RSP_DAQ_WIFI_STREAM_INFO 0x8Cu // response to HATP_CMD_DAQ_WIFI_STREAM_INFO (mirrors S3 HAT_RSP_DAQ_WIFI_STREAM_INFO)
#define HATP_RSP_DAQ_VDUT_STATUS 0x98u // payload: s3link_vdut_status_t; response to HATP_CMD_DAQ_VDUT_STATUS
#define HATP_RSP_DAQ_C6_VERSION  0x99u // payload: s3link_c6_version_t; response to HATP_CMD_DAQ_C6_VERSION

// Firmware version reported in GET_INFO.
#define S3LINK_FW_MAJOR      1u
#define S3LINK_FW_MINOR      0u

// DAQ status payload (response to DAQ_GET_STATUS). MUST stay byte-for-byte
// identical to the S3-side mirror hat_daq_status_t in
// Firmware/ESP32/src/hat/hat.h. 24 bytes -- well under the 32-byte
// HAT_WIRE_FRAME_MAX_LEN wire cap the S3's hat_recv_frame() enforces (see the
// HATP_MAX_PAYLOAD-vs-reply-cap note above handle_config_get_all()), so this
// reply needs no paging.
typedef struct __attribute__((packed)) {
    uint8_t  range;          // current_range_t
    uint8_t  streaming;      // bool
    uint8_t  source_enabled; // bool
    uint8_t  _pad;
    float    last_i;
    float    last_v;
    float    last_p;
    float    energy_mwh;
    // Live softAP association count (wifi_ap_sta_count()), added so
    // GET /api/daq/wifi_stream/status can tell a broken 60s idle-teardown
    // timer (daq_board.c:1981, resets while any station is associated) from
    // a phone that silently auto-joined the DAQ hotspot. 0 when the softAP
    // is not up. Queried live on every HTTP poll rather than folded into the
    // WIFI_STREAM_INFO bring-up blob, because that blob is only reassembled
    // once per bring-up (S3 stops polling it after reaching READY) and would
    // go stale for exactly the window this field needs to stay live in.
    uint32_t sta_count;
} s3link_daq_status_t;

// HATP_CMD_DAQ_VDUT_SETPOINT (0x78) payload. MUST stay byte-for-byte identical
// to the S3-side mirror hat_vdut_setpoint_t in Firmware/ESP32/src/hat/hat.h.
typedef struct __attribute__((packed)) {
    float vdut_v;      // target DUT voltage (V), clamped to [SMU_VDUT_MIN, SMU_VDUT_MAX]
    float ilimit_a;     // target current limit (A), clamped to [SMU_ILIMIT_MIN_A, SMU_ILIMIT_FULLSCALE_A]
} s3link_vdut_setpoint_t;

// HATP_CMD_DAQ_SET_ACQ_CONFIG (0x7A) payload. MUST stay byte-for-byte
// identical to the S3-side mirror hat_acq_config_t in Firmware/ESP32/src/hat/hat.h.
//
// There is deliberately no "sample rate" or "stream decimation" field here:
// the sample rate IS the ODR, set via the ADC's own digital filter/decimation
// (x32..x1024 for Sinc5/wideband, an arbitrary multiple of 32 for Sinc3).
// Stream decimation (daq_board.c wave_decim) is a naive keep-1-of-N drop with
// no anti-alias filter -- it folds everything above the new Nyquist back into
// the band as spurious signal -- so it is intentionally NOT exposed as a
// user-facing knob through this command; it stays fixed at 1 here and is only
// ever touched by the legacy USB_CMD_SET_RATE path (BBP-less iOS/USB hosts).
typedef struct __attribute__((packed)) {
    uint8_t  filter;    // ADAQ_FILTER_* (adaq7769_regs.h)
    uint8_t  adc_dec;   // ADAQ_DEC_* for every filter except SINC3, where it
                        // instead carries (decimation / 32) since SINC3 takes
                        // an arbitrary multiple of 32 rather than a fixed step
    uint8_t  sr_mode;   // 1 = Super Resolution: the P4 ignores filter/adc_dec
                        // above and pins the ADAQs to Sinc3 at max decimation,
                        // then FIR-decimates to DAQ_SR_*_SPS. APPENDED field --
                        // a 2-byte payload from an older S3 means "SR off".
} s3link_acq_config_t;

// HATP_RSP_DAQ_VDUT_STATUS (0x98) payload: response to HATP_CMD_DAQ_VDUT_STATUS.
// MUST stay byte-for-byte identical to the S3-side mirror hat_vdut_status_t.
typedef struct __attribute__((packed)) {
    uint8_t  present;      // bool: SMU hardware detected (DS4424 IDAC present)
    uint8_t  enabled;      // bool: LTM8056 RUN asserted
    uint8_t  fault;        // bool: SMU in a fault state (hardware absent / read error)
    uint8_t  _pad;
    float    vdut_set_v;   // programmed DUT voltage setpoint (V)
    float    ilimit_set_a; // programmed current limit setpoint (A)
    float    meas_v;       // measured DUT voltage (V), from the ADAQ7769 acquisition chain
    float    meas_i;       // measured DUT current (A), from the ADAQ7769 acquisition chain
} s3link_vdut_status_t;

// HATP_CMD_OTA_STATUS (0x65) reply. The first 10 bytes are the original wire
// shape and MUST NOT move: older S3 builds read exactly {state, pending_verify,
// received, image_size} and stop. The relay_* tail is appended, so a short
// 10-byte reply from an older P4 is still valid — the S3 must treat a reply
// shorter than sizeof(s3link_ota_status_t) as "relay fields unknown" rather
// than as an error.
//
// Exposes BOTH durability models because the two OTA targets resume from
// different sources: a P4-target transfer resumes from @received (ota.c streams
// straight to the A/B slot, no staging), while a C6-target transfer resumes
// from @relay_staged_bytes (relay_stage.c).
typedef struct __attribute__((packed)) {
    uint8_t  state;                 // ota_state_t of ota.c
    uint8_t  pending_verify;        // bool: image awaiting ota_confirm()
    uint32_t received;              // ota_received(): exact, byte-accurate
    uint32_t image_size;
    // --- appended for multi-MCU OTA; absent in replies from older P4 builds ---
    uint8_t  relay_state;           // relay_state_t of relay_stage.c
    uint32_t relay_staged_bytes;    // may trail by up to ~64KB (NVS persist interval)
    uint32_t relay_pushed_bytes;
    uint8_t  relay_target;          // relay_target_t: 1=C6, 2=S3, 0=nothing staged
} s3link_ota_status_t;               // 20 bytes

// HATP_CMD_DAQ_C6_VERSION (0x6A) reply. The C6 ONLY — the P4 reports its own
// version through HATP_CMD_GET_VERSION (0x60), which already exists. Sourced
// from the P4's last DDP-reported C6 version (see daq_board.c c6 version cache);
// all-zero if the C6 has not reported yet.
typedef struct __attribute__((packed)) {
    char c6_version[16];            // NUL-terminated semver, e.g. "1.4.0"
    char c6_build_id[16];
} s3link_c6_version_t;               // 32 bytes

// HATP_CMD_DAQ_TELEMETRY (0x5A) payload: S3 mainboard telemetry relayed to the
// C6 Diagnostics menu. MUST stay byte-for-byte identical to the S3-side mirror
// hat_daq_telemetry_t in Firmware/ESP32/src/hat/hat.h.
#define S3LINK_TLM_NA      ((int16_t)0x7FFF)  // sentinel: value unreadable
#define S3LINK_TLM_F_PD    0x01u  // USB-PD attached / contract valid
#define S3LINK_TLM_F_RAILS 0x02u  // VADJ1/VADJ2/VLOGIC fields valid
#define S3LINK_TLM_F_DIE   0x04u  // die_temp_c10 valid

typedef struct __attribute__((packed)) {
    int16_t  die_temp_c10;   // AD74416H die temp, 0.1 C (S3LINK_TLM_NA if invalid)
    uint16_t pd_mv;          // USB-PD negotiated voltage, mV (0 if unattached)
    uint16_t pd_ma;          // USB-PD negotiated current cap, mA
    uint16_t vadj1_mv;       // VADJ1_BUCK rail, mV (0 if unavailable)
    uint16_t vadj2_mv;       // VADJ2_BUCK rail, mV
    uint16_t vlogic_mv;      // 3V3_ADJ / VLOGIC rail, mV
    uint8_t  flags;          // S3LINK_TLM_F_*
} s3link_telemetry_t;        // 13 bytes

// HATP_CMD_DAQ_ARM (0x5B) payload: arm/disarm the trigger latch + pre-roll.
typedef struct __attribute__((packed)) {
    uint8_t  armed;          // 1 = arm trigger latch, 0 = free-run/disarm
    uint8_t  trig_logic;     // 0 = none, 1 = OR, 2 = AND (S3 evaluates; info only)
    uint16_t _pad;
    uint32_t pre_samples;    // requested pre-trigger depth (fused samples)
} s3link_daq_arm_t;

// HATP_CMD_DAQ_MARK (0x5C) payload: a digital event detected on an S3 IO. The
// P4 stamps it with the live sample index and emits a USB MARKER record.
#define S3LINK_MARK_KIND_FLAG     0u  // informational flag (vertical line)
#define S3LINK_MARK_KIND_TRIGGER  1u  // acquisition trigger fired (defines t=0)
typedef struct __attribute__((packed)) {
    uint8_t  channel;        // S3 IO number (1..12)
    uint8_t  edge;           // 0 = falling, 1 = rising
    uint8_t  kind;           // S3LINK_MARK_KIND_*
    uint8_t  _pad;
} s3link_daq_mark_t;

// OTA_DATA payload: a firmware chunk at a given byte offset (for ordered,
// resumable streaming so the S3 never stages the whole image). The image is
// written straight to the P4 flash one chunk at a time.
typedef struct __attribute__((packed)) {
    uint32_t offset;         // byte offset of this chunk within the image
    // followed by up to HATP_OTA_CHUNK_MAX firmware bytes
} s3link_ota_data_hdr_t;

// HATP_CMD_STAGE_READ payload: request up to @len bytes from `staging` at
// @offset. Response (HATP_RSP_STAGE_DATA) payload is exactly the requested
// bytes with no header (offset is implicit — the requester already knows
// what it asked for); a short response (< len) means end-of-image.
typedef struct __attribute__((packed)) {
    uint32_t offset;
    uint8_t  len;            // <= HATP_OTA_CHUNK_MAX
} s3link_stage_read_req_t;

// Fixed-width blob reassembled from successive HATP_CMD_DAQ_WIFI_STREAM_INFO
// chunks (once status == HATP_WIFI_INFO_ST_READY). MUST stay byte-for-byte
// identical to the S3-side mirror in Firmware/ESP32/src/hat/hat.h. 104 bytes.
typedef struct __attribute__((packed)) {
    char     ssid[33];       // NUL-terminated, <=32 ASCII chars
    char     password[65];   // NUL-terminated, <=64 ASCII chars
    uint16_t port;           // little-endian
    uint8_t  host[4];        // raw IPv4 octets
} s3link_wifi_stream_info_t;   // 104 bytes

// Command callback: the board handles DAQ-specific commands and (optionally)
// fills a response payload. Return the number of response bytes written into
// @p resp (<= HATP_MAX_PAYLOAD), or a negative value to send RSP_ERROR.
//   cmd     : the HATP_CMD_DAQ_* code.
//   payload : received payload (may be NULL if len == 0).
//   len     : received payload length.
//   resp    : response payload buffer (HATP_MAX_PAYLOAD bytes).
typedef int (*s3link_cmd_cb_t)(uint8_t cmd, const uint8_t *payload, uint8_t len,
                               uint8_t *resp, void *user);

typedef struct {
    s3link_cmd_cb_t cmd_cb;
    void           *cmd_user;
    TaskHandle_t    task;
    volatile bool   running;
    uint32_t        rx_frames;
    uint32_t        crc_errors;
} s3_link_t;

/**
 * @brief Initialise UART1 + the IRQ line and register the DAQ command callback.
 */
esp_err_t s3_link_init(s3_link_t *s, s3link_cmd_cb_t cmd_cb, void *user);

/** @brief Launch the slave service task (parses frames, sends responses). */
esp_err_t s3_link_start(s3_link_t *s, int task_core, int task_prio);

/** @brief Stop the service task. */
void s3_link_stop(s3_link_t *s);

/** @brief Assert the IRQ line LOW to request a poll from the S3. */
void s3_link_assert_irq(s3_link_t *s);

/** @brief Release the IRQ line (high-Z; external pull-up restores HIGH). */
void s3_link_release_irq(s3_link_t *s);

/** @brief CRC-8 (poly 0x07, init 0x00) over CMD+PAYLOAD — exposed for tests. */
uint8_t s3_link_crc8(const uint8_t *data, uint32_t len);

#ifdef __cplusplus
}
#endif
