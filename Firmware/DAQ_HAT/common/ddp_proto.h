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
#define DDP_PROTO_VERSION   9u
// v9 (2026-08-06): Super-Resolution setting (DAQ_K_SR_MODE) rides the existing
// TLV config path, and the mainboard tunnel gains DDP_MB_FWINFO /
// DDP_MB_FW_APPLY so the C6 Firmware screen can show installed-vs-available
// versions for all four MCUs and drive a GitHub release update.
// v8 (2026-07-21): WiFi streaming mode handoff (DDP_CMD_WIFI_STREAM_MODE). The
// P4 tells the C6 when it is about to hand the shared SDIO link over to
// ESP-Hosted (iOS DAQ streaming) so the C6 can stop its own display refresh
// and show a static "WiFi streaming" screen instead of fighting the radio
// stack for CPU/bus time. The C6 acks with RSP_OK either way.
// v7 (2026-07-14): DUT source calibration wizard over DDP (DDP_CMD_CAL_CTRL /
// _CAL_STATUS) — the C6 drives the P4's smu_cal engine (voltage/current/
// baseline) with live phase + operator prompt + progress.
// v6 (2026-07-14): mainboard power tunnel carries rail enable + power-good state
// (ddp_mb_power_t.rail_en/rail_pg) and gains DDP_MB_SET_RAIL_EN so the C6 can
// toggle VLOGIC/level-shifter OE + VADJ1/VADJ2 enables. Additive; both chips
// flashed together.
// v5 (2026-07-14): mainboard settings tunnel (DDP_CMD_MB_REQUEST / _RESPONSE) —
// the C6 Mainboard menu drives S3 rails/efuses/scripts through the P4, which
// forwards to the S3 over the HAT link (guarded: deferred while the P4 streams
// to the PC). Additive; both chips are flashed together so no compat window.
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
#define DDP_CMD_MB_REQUEST      0x19u  // C6 -> P4: u8 req_type, then req args (mainboard tunnel)
#define DDP_CMD_MB_RESPONSE     0x1Au  // P4 -> C6: u8 req_type, u8 status, then result data
#define DDP_CMD_WIFI_STREAM_MODE 0x1Du // u8 enable (1=entering WiFi stream mode, 0=leaving)

// --- Events (C6 -> P4), 0x60..0x7F -----------------------------------------
// Emitted unsolicited by the C6 when the user changes settings on-device, so
// the P4 (the authoritative store) can apply + re-broadcast them.
#define DDP_CMD_SET_CONFIG      0x60u  // ddp_config_t — legacy fixed snapshot (deprecated)
#define DDP_CMD_CONFIG_SET      0x61u  // one or more TLVs — preferred key-addressed event
#define DDP_CMD_CONFIG_ACTION   0x62u  // u8 action_id (daq_action_t) — stateless one-shot

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
#define DDP_FLAG_SRC_ON         0x10u  // DUT supply (SMU) output is enabled

// Live current range packed into SET_MEASUREMENT flags bits 5-6, so the C6 home
// screen can show a range badge/triangle without an extra frame.
#define DDP_FLAG_RANGE_SHIFT    5u
#define DDP_FLAG_RANGE_MASK     (3u << 5)
#define DDP_RANGE_HI            0u   // 51 ohm
#define DDP_RANGE_MID           1u   // 2 ohm
#define DDP_RANGE_LO            2u   // 50 mohm
#define DDP_RANGE_UNKNOWN       3u   // auto-searching / unknown

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

// ---------------------------------------------------------------------------
// Mainboard settings tunnel (DDP_CMD_MB_REQUEST / DDP_CMD_MB_RESPONSE).
// ---------------------------------------------------------------------------
// The C6 "Main Board Settings" menu reads/writes S3-mainboard resources (VLOGIC/
// VADJ1/VADJ2 rail setpoints, per-efuse enable + fault, MicroPython scripts).
// The C6 has no path to the S3, so it tunnels: C6 -> P4 (this DDP command) ->
// S3 (HAT link). The P4 caches the request, the S3 polls for it (only while NOT
// streaming to the PC), executes it, and returns the result which the P4 relays
// back as DDP_CMD_MB_RESPONSE. Requests are only issued while the relevant menu
// is open, keeping the HAT link idle the rest of the time.

// DDP_CMD_MB_REQUEST payload: [u8 req_type][args...]
#define DDP_MB_POWER        0x01u  // read rail setpoints + efuse status (no args)
#define DDP_MB_SET_RAIL     0x02u  // args: u8 rail (0=VLOGIC,1=VADJ1,2=VADJ2), u16 mv
#define DDP_MB_SET_EFUSE    0x03u  // args: u8 idx (0..3), u8 on
#define DDP_MB_SCRIPTS      0x04u  // read script list + engine status (no args)
#define DDP_MB_SCRIPT_RUN   0x05u  // args: u8 name_len, char[name_len]
#define DDP_MB_SCRIPT_STOP  0x06u  // stop the running script (no args)
#define DDP_MB_SET_RAIL_EN  0x07u  // args: u8 rail (0=VLOGIC/lshift,1=VADJ1,2=VADJ2), u8 on
#define DDP_MB_FWINFO       0x08u  // read firmware versions + GitHub releases (no args)
#define DDP_MB_FW_APPLY     0x09u  // args: u8 rel_index, u8 targets (DDP_FW_T_* mask)

// DDP_CMD_MB_RESPONSE payload: [u8 req_type][u8 status][data...]
#define DDP_MB_ST_OK        0x00u
#define DDP_MB_ST_BUSY      0x01u  // deferred: the P4 is streaming (acquisition)
#define DDP_MB_ST_ERR       0x02u

// Rail selectors for DDP_MB_SET_RAIL (index into the DS4424 IDAC channels).
#define DDP_MB_RAIL_VLOGIC  0u
#define DDP_MB_RAIL_VADJ1   1u
#define DDP_MB_RAIL_VADJ2   2u

// DDP_MB_POWER result data (follows [req_type][status]).
typedef struct __attribute__((packed)) {
    uint16_t vlogic_mv;   // VLOGIC setpoint (DS4424 ch0), mV
    uint16_t vadj1_mv;    // VADJ1 setpoint  (DS4424 ch1), mV
    uint16_t vadj2_mv;    // VADJ2 setpoint  (DS4424 ch2), mV
    uint8_t  efuse_en;    // bit i (0..3) = e-fuse (i+1) enabled
    uint8_t  efuse_flt;   // bit i (0..3) = e-fuse (i+1) fault active
    uint8_t  rail_en;     // bit0=VLOGIC/level-shifter OE, bit1=VADJ1_EN, bit2=VADJ2_EN
    uint8_t  rail_pg;     // bit1=VADJ1 power-good, bit2=VADJ2 power-good
} ddp_mb_power_t;

// Rail-enable bit positions for ddp_mb_power_t.rail_en / rail_pg (index by rail).
#define DDP_MB_RAILEN_VLOGIC  (1u << 0)   // level-shifter OE
#define DDP_MB_RAILEN_VADJ1   (1u << 1)
#define DDP_MB_RAILEN_VADJ2   (1u << 2)

// ---------------------------------------------------------------------------
// Firmware versions + GitHub updates (DDP_MB_FWINFO / DDP_MB_FW_APPLY).
// ---------------------------------------------------------------------------
// Only the S3 has a radio, so it is the only chip that can reach GitHub. The C6
// Firmware screen therefore asks through the same C6 -> P4 -> S3 tunnel used by
// the mainboard menus. The S3 answers from a CACHED snapshot and refreshes it on
// a worker task -- an HTTPS release query must never run inside the tunnel poll,
// which has to answer within the HAT link timeout.
//
// Release choices are identified by INDEX into the S3's release list rather than
// by tag string, because that is exactly what update_manager_apply_release_index()
// consumes; the tags below are for display only.

// Per-MCU bits for update_avail / DDP_MB_FW_APPLY targets. These MUST match
// update_target_t in Firmware/ESP32/src/update/update_manager.h.
#define DDP_FW_T_RP2040   0x01u
#define DDP_FW_T_S3       0x02u
#define DDP_FW_T_P4       0x04u
#define DDP_FW_T_C6       0x08u

#define DDP_FW_STR        16u   // version string field width (NUL-terminated)
#define DDP_FW_DEV_MAX     4u   // installed[] slots, indexed by DDP_FW_IDX_*
#define DDP_FW_REL_MAX     5u   // matches update_manager's 5-option ceiling

// Indices into ddp_mb_fwinfo_t.installed[].
#define DDP_FW_IDX_RP2040  0u
#define DDP_FW_IDX_S3      1u
#define DDP_FW_IDX_P4      2u
#define DDP_FW_IDX_C6      3u

// ddp_mb_fwinfo_t.state
#define DDP_FW_ST_IDLE     0u
#define DDP_FW_ST_CHECKING 1u   // a GitHub query is in flight; retry shortly
#define DDP_FW_ST_APPLYING 2u   // an update is being applied; do not start another
#define DDP_FW_ST_ERROR    3u   // last check failed (no network / API error)

// DDP_MB_FWINFO result data (follows [req_type][status]). 157 bytes, which fits
// both the 210-byte tunnel blob cap and a single 240-byte DDP frame.
typedef struct __attribute__((packed)) {
    char     installed[DDP_FW_DEV_MAX][DDP_FW_STR]; // running version per MCU ("" = unknown)
    char     rel[DDP_FW_REL_MAX][DDP_FW_STR];       // GitHub release tags, newest first
    uint8_t  rel_count;      // valid entries in rel[]
    uint8_t  update_avail;   // DDP_FW_T_* bits whose newest release differs from installed
    uint8_t  state;          // DDP_FW_ST_*
    uint8_t  active_target;  // DDP_FW_T_* bit of the MCU being updated; valid only when state==APPLYING
    uint32_t progress_done;  // bytes transferred for active_target; valid only when state==APPLYING
    uint32_t progress_total; // total bytes for active_target, 0 if not yet known; valid only when state==APPLYING
} ddp_mb_fwinfo_t;

// ---------------------------------------------------------------------------
// DUT source (SMU) calibration wizard (C6 <-> P4 direct; no S3 involvement).
// ---------------------------------------------------------------------------
// The P4 runs the calibration on a background task (see ESP32P4/src/cal/smu_cal).
// The C6 drives it: START a mode, poll STATUS while the wizard screen is open,
// ACK the operator prompt ("short/disconnect/leave open the output"), or ABORT.
// ddp_cal_status_t MUST stay byte-identical to smu_cal_status_t.
#define DDP_CMD_CAL_CTRL     0x1Bu  // C6 -> P4: [u8 op][u8 arg]
#define DDP_CMD_CAL_STATUS   0x1Cu  // P4 -> C6: ddp_cal_status_t

#define DDP_CAL_OP_START     0x00u  // arg = mode (DDP_CAL_MODE_*)
#define DDP_CAL_OP_ACK       0x01u  // acknowledge the operator prompt
#define DDP_CAL_OP_ABORT     0x02u  // abort + restore safe SMU state
#define DDP_CAL_OP_STATUS    0x03u  // request a CAL_STATUS push

#define DDP_CAL_MODE_VOLTAGE   0u   // sweep V_FB, output DISCONNECTED
#define DDP_CAL_MODE_CURRENT   1u   // sweep I_FB, output SHORTED
#define DDP_CAL_MODE_BASELINE  2u   // open-circuit offset per range, output OPEN

#define DDP_CAL_PH_IDLE     0u
#define DDP_CAL_PH_PROMPT   1u      // blocked on operator action (see prompt)
#define DDP_CAL_PH_RUNNING  2u
#define DDP_CAL_PH_SUCCESS  3u
#define DDP_CAL_PH_FAILED   4u

#define DDP_CAL_PR_NONE        0u
#define DDP_CAL_PR_DISCONNECT  1u   // remove the DUT load
#define DDP_CAL_PR_SHORT       2u   // short the output
#define DDP_CAL_PR_OPEN        3u   // leave the output open

#define DDP_CAL_PERSIST_RAM     0u
#define DDP_CAL_PERSIST_SAVING  1u
#define DDP_CAL_PERSIST_SAVED   2u
#define DDP_CAL_PERSIST_FAILED  3u

// ddp_cal_status_t.flags bit: cal aborted because the USB-PD contract was below
// the requirement (20 V / 3 A for current cal). Mirrors SMU_CAL_FLAG_NO_PD.
#define DDP_CAL_FLAG_NO_PD      0x0100u

typedef struct __attribute__((packed)) {
    uint8_t  phase;      // DDP_CAL_PH_*
    uint8_t  prompt;     // DDP_CAL_PR_*
    uint8_t  mode;       // DDP_CAL_MODE_*
    uint8_t  progress;   // 0..100
    uint8_t  point;      // current point index
    int8_t   code;       // current DS4424 code
    uint8_t  persist;    // DDP_CAL_PERSIST_*
    uint8_t  _unused;
    float    measured;   // last stable measurement (V or A)
    float    min_v;      // min across the sweep
    float    max_v;      // max across the sweep
    uint16_t flags;      // validation bitfield (SMU_CAL_FLAG_*)
    uint8_t  vcount;     // stored voltage points
    uint8_t  icount;     // stored current points
} ddp_cal_status_t;

// DDP_MB_SCRIPTS / _SCRIPT_RUN / _SCRIPT_STOP result data (follows
// [req_type][status]), a variable-length blob describing the S3 MicroPython
// engine and stored scripts:
//   [u8 engine_state]                       DDP_MB_SCR_*
//   [u8 err_len][char err[err_len]]         last error message (may be empty)
//   [u8 count]                              number of script names that follow
//   count x { u8 name_len, char name[name_len] }
// The whole blob is capped so the assembled DDP_CMD_MB_RESPONSE stays within a
// single DDP frame; the S3 chunks it over the 32-byte HAT link and the P4
// reassembles before relaying.
#define DDP_MB_SCR_IDLE     0u   // never ran / nothing loaded
#define DDP_MB_SCR_RUNNING  1u   // a script is executing
#define DDP_MB_SCR_CRASHED  2u   // last run ended with an error
#define DDP_MB_SCR_EXITED   3u   // last run completed cleanly

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
