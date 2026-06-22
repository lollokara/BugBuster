#pragma once

// ===========================================================================
// DAQ HAT Display Protocol (DDP)  -  ESP32-P4  <->  ESP32-C6   (SHARED HEADER)
// ===========================================================================
// Single source of truth for the P4<->C6 link, included by BOTH firmwares (the
// P4 acts as bus master / DDP sender, the C6 as slave / renderer). It lives in
// Firmware/DAQ_HAT/common so the two chips never drift. The framing mirrors the
// RP2040 HAT protocol so CRC code is shared.
//
//   +------+-----+-----+------------------+------+
//   | SYNC | LEN | CMD |     PAYLOAD      | CRC  |
//   | 1B   | 1B  | 1B  |     0..240 B     | 1B   |
//   +------+-----+-----+------------------+------+
//
//   SYNC = 0xAA
//   LEN  = payload length (0..DDP_MAX_PAYLOAD)
//   CMD  = command id (master->slave) or response id (slave->master)
//   CRC  = CRC-8 (poly 0x07, init 0x00) over CMD + PAYLOAD
//   Multi-byte fields are little-endian.
//
// v3 (2026-06-21): buttons moved to the P4 and are relayed to the C6
// (DDP_CMD_BUTTON_EVENT); full bidirectional settings sync via key-addressed
// TLV (DDP_CMD_CONFIG_PUSH P4->C6, DDP_CMD_CONFIG_SET C6->P4); neopixels and
// wifi added as TLV settings. Payload cap raised 32 -> 240 for TLV batches.
// ===========================================================================

#include <stdint.h>
#include <stddef.h>

#define DDP_SYNC            0xAAu
#define DDP_MAX_PAYLOAD     240u
#define DDP_PROTO_VERSION   4u
// v4 (2026-06-22): ddp_diag_t expanded into a full onboard-device snapshot
// (board + ADAQ + P4 + S3 die temperatures, fused I/V/P, SMU monitor currents,
// USB-PD + VADJ rails relayed from the S3, P4 runtime stats, validity flags).
// The C6 fills its own self-stats locally and renders a live sparkline detail
// view per sensor. Internal DDP version only — independent of BBP PROTO_VERSION.

// --- Commands (master P4 -> slave C6), 0x01..0x5F --------------------------
#define DDP_CMD_PING            0x01u  // -> RSP_OK
#define DDP_CMD_GET_INFO        0x02u  // -> RSP_INFO
#define DDP_CMD_SET_MEASUREMENT 0x10u  // float voltage_v, float current_a, u8 flags
#define DDP_CMD_SET_STATUS      0x11u  // u8 state, ASCII label (<=30 bytes)
#define DDP_CMD_SET_BACKLIGHT   0x12u  // u8 brightness 0..255
#define DDP_CMD_CLEAR           0x13u  // blank readouts / show "no data"
#define DDP_CMD_SET_DIAGNOSTICS 0x14u  // ddp_diag_t — full diagnostics snapshot
#define DDP_CMD_BUTTON_EVENT    0x15u  // u8 events bitmask (DDP_BTN_*) — P4 buttons
#define DDP_CMD_CONFIG_PUSH     0x16u  // one or more TLVs — P4 pushes current/changed settings
#define DDP_CMD_SET_CH_LEDS     0x18u  // 4x u8 channel colour codes (front 4-connector, pairs)

// --- Events (C6 -> P4), 0x60..0x7F -----------------------------------------
// Emitted unsolicited by the C6 when the user changes settings on-device, so
// the P4 (the authoritative store) can apply + re-broadcast them.
#define DDP_CMD_SET_CONFIG      0x60u  // ddp_config_t — legacy fixed snapshot (deprecated)
#define DDP_CMD_CONFIG_SET      0x61u  // one or more TLVs — preferred key-addressed event

// --- Responses (slave -> master), 0x80..0xFF -------------------------------
#define DDP_RSP_OK              0x80u
#define DDP_RSP_INFO            0x82u  // u8 hat_type, u8 fw_major, u8 fw_minor, u8 proto
#define DDP_RSP_ERR             0xFFu  // u8 error code

// --- Button event bits (must match ESP32C6 buttons.h BTN_EV_*) --------------
#define DDP_BTN_UP              0x01u
#define DDP_BTN_DOWN            0x02u
#define DDP_BTN_OK             0x04u
#define DDP_BTN_BACK           0x08u

// --- SET_MEASUREMENT flag bits ---------------------------------------------
#define DDP_FLAG_V_VALID        0x01u  // voltage field holds live data
#define DDP_FLAG_I_VALID        0x02u  // current field holds live data
#define DDP_FLAG_V_OVERRANGE    0x04u
#define DDP_FLAG_I_OVERRANGE    0x08u

// --- Link / connection state ------------------------------------------------
#define DDP_STATE_BOOT          0u
#define DDP_STATE_LIVE          1u   // receiving frames from P4
#define DDP_STATE_SIM           2u   // no link: locally generated demo data
#define DDP_STATE_FAULT         3u

// Payload of DDP_CMD_SET_MEASUREMENT (little-endian, 9 bytes).
typedef struct __attribute__((packed)) {
    float   voltage_v;   // volts
    float   current_a;   // amperes
    uint8_t flags;       // DDP_FLAG_*
} ddp_measurement_t;

// Payload of DDP_CMD_SET_DIAGNOSTICS — full onboard-device snapshot (little-
// endian, packed). The P4 gathers every readable device once per housekeeping
// tick, relays the S3 mainboard telemetry it has cached, and pushes this whole
// struct to the C6 (~1 Hz). All fields are compact fixed-point so the C6 needs
// no FPU to format them. Temperatures are 0.1 C; an out-of-range sensor reports
// DDP_DIAG_TEMP_NA. The `valid` bitmask says which sources are fresh; the C6
// shows "--" for anything not flagged valid.
//
// C6-local self-stats (its own heap / die temp / uptime) are NOT carried here —
// the C6 reads them directly and merges them into the Diagnostics menu.
#define DDP_DIAG_TEMP_NA   ((int16_t)0x7FFF)  // sentinel: sensor unreadable

// Bits for ddp_diag_t.valid.
#define DDP_DIAG_V_BOARD0  (1u << 0)   // AD7415 U2
#define DDP_DIAG_V_BOARD1  (1u << 1)   // AD7415 U28
#define DDP_DIAG_V_ADAQ0   (1u << 2)   // ADAQ7769-1 U1 die temp
#define DDP_DIAG_V_ADAQ1   (1u << 3)   // ADAQ7769-1 U22 die temp
#define DDP_DIAG_V_ADAQ2   (1u << 4)   // ADAQ7769-1 U23 die temp
#define DDP_DIAG_V_P4TEMP  (1u << 5)   // ESP32-P4 internal tsens
#define DDP_DIAG_V_IVP     (1u << 6)   // fused current/voltage/power
#define DDP_DIAG_V_SMU     (1u << 7)   // IINMON/IOUTMON SMU monitor currents
#define DDP_DIAG_V_VDUT    (1u << 8)   // measured V_DUT
#define DDP_DIAG_V_S3      (1u << 9)   // S3 telemetry block fresh (rails/PD/die)
#define DDP_DIAG_V_S3PD    (1u << 10)  // USB-PD attached/negotiated

typedef struct __attribute__((packed)) {
    // --- Temperatures (0.1 C, DDP_DIAG_TEMP_NA if unreadable) ---------------
    int16_t  t_board0_c10;     // AD7415 U2  (ADG/analog area)
    int16_t  t_board1_c10;     // AD7415 U28 (power-supply area)
    int16_t  t_adaq0_c10;      // ADAQ7769-1 U1 die
    int16_t  t_adaq1_c10;      // ADAQ7769-1 U22 die
    int16_t  t_adaq2_c10;      // ADAQ7769-1 U23 die
    int16_t  t_p4_c10;         // ESP32-P4 internal sensor
    int16_t  t_s3_c10;         // ESP32-S3 AD74416H die (relayed)
    // --- Fused measurement (signed micro-units; survive full DUT range) -----
    int32_t  i_ua;             // current, microamps
    int32_t  v_uv;             // bus voltage, microvolts
    int32_t  p_uw;             // power, microwatts
    // --- SMU monitor currents (LTM8056 IINMON/IOUTMON) ----------------------
    int16_t  smu_iin_ma;       // input current, mA
    int16_t  smu_iout_ma;      // output current, mA
    uint16_t vdut_mv;          // measured V_DUT, mV
    // --- S3 mainboard rails / USB-PD (relayed over the HAT link) ------------
    uint16_t pd_mv;            // USB-PD negotiated voltage, mV
    uint16_t pd_ma;            // USB-PD negotiated current cap, mA
    uint16_t vadj1_mv;         // VADJ1_BUCK rail, mV
    uint16_t vadj2_mv;         // VADJ2_BUCK rail, mV
    uint16_t vlogic_mv;        // 3V3_ADJ / VLOGIC rail, mV
    // --- P4 runtime stats ---------------------------------------------------
    uint16_t p4_free_mem_kb;   // P4 free heap, KB
    uint16_t p4_free_stack_b;  // P4 min free stack, bytes
    uint8_t  p4_tasks;         // P4 FreeRTOS task count
    uint32_t p4_uptime_s;      // P4 uptime, seconds
    // --- Source validity ----------------------------------------------------
    uint16_t valid;            // DDP_DIAG_V_* bitmask
} ddp_diag_t;

// Payload of the legacy DDP_CMD_SET_CONFIG (little-endian, 10 bytes). Retained
// for backward compatibility; new code uses the TLV config path (CONFIG_SET /
// CONFIG_PUSH) which covers every setting in daq_config_registry.h.
typedef struct __attribute__((packed)) {
    uint8_t  autoranging;      // 0/1
    uint8_t  range_idx;        // 0=A, 1=mA, 2=uA
    uint8_t  sample_rate_idx;  // 0..4
    uint8_t  reserved;
    uint16_t dut_current_ma;   // 100..2500
    uint16_t dut_voltage_mv;   // 1800..20000
    uint8_t  brightness_pct;   // 10..100
    uint8_t  dark_mode;        // 0/1
} ddp_config_t;

// CRC-8, poly 0x07, init 0x00 (matches RP2040 HAT protocol).
static inline uint8_t ddp_crc8(const uint8_t *data, size_t len)
{
    uint8_t crc = 0x00;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int j = 0; j < 8; j++) {
            crc = (crc & 0x80) ? (uint8_t)((crc << 1) ^ 0x07) : (uint8_t)(crc << 1);
        }
    }
    return crc;
}
