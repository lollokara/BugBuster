#pragma once

// =============================================================================
// range_cal.h — per-board autorange threshold calibration.
//
// PROBLEM: The HI-range zero-current CSA offset is 1.7–2.0 V (per-unit,
// V_DUT-dependent). This means the HI→MID SET threshold (~3.9 V) and RESET
// threshold (~0.6 V on MID CSA) translate to different physical currents on
// each unit. Hardcoded thresholds produce incorrect fine_trust_max windows,
// and small per-unit differences can keep the autorange in COARSE unnecessarily.
//
// SOLUTION: A two-pass sweep using the onboard SMU and two precision resistors
// (user-supplied, connected across the DUT output terminals):
//
//   Pass A — calibrates HI↔MID boundary:
//     Connect R_CAL_A ≈ 5.6 kΩ ±0.1%. V_DUT sweeps 1.76 V → 20 V.
//     Current range: ~314 µA → 3.6 mA — straddles the HI/MID flip-flop.
//     Records: v_set_hi (raw HI-CSA volts at FF_HI POSEDGE)
//              v_reset_hi (raw MID-CSA volts at FF_HI NEGEDGE)
//
//   Pass B — calibrates MID↔LO boundary:
//     Connect R_CAL_B ≈ 56 Ω ±0.1%. V_DUT sweeps 1.76 V → 20 V.
//     Current range: ~31 mA → 357 mA — straddles the MID/LO flip-flop.
//     Records: v_set_mid (raw MID-CSA volts at FF_MID POSEDGE)
//              v_reset_mid (raw LO-CSA volts at FF_MID NEGEDGE)
//
// Results are stored to NVS and applied to range_manager + current_fusion at
// boot, making the trust window and confirm thresholds board-specific.
//
// Calibration must run with fast acquisition STOPPED. The phase state machine
// mirrors smu_cal for consistency — desktop drives it via USB_CMD_RANGE_CAL_*.
// =============================================================================

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>   // offsetof
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

struct daq_board;  // forward-declared to avoid circular includes

#ifdef __cplusplus
extern "C" {
#endif

// ---- Calibration data (stored in NVS, loaded at boot) ----------------------

#define RANGE_CAL_NVS_NS     "range_thr_cal"
#define RANGE_CAL_NVS_KEY    "cal_v1"
#define RANGE_CAL_MAGIC      0xBBC4u   // 16-bit tag; bump when struct layout changes

typedef struct __attribute__((packed)) {
    uint16_t magic;

    // Raw CSA output voltages (V, including all offsets) at each FF transition.
    float v_set_hi;       // HI-CSA volts at FF_HI POSEDGE  (nominal ~3.9 V)
    float v_set_mid;      // MID-CSA volts at FF_MID POSEDGE (nominal ~3.9 V)
    float v_reset_hi;     // MID-CSA volts at FF_HI NEGEDGE  (nominal ~0.6 V)
    float v_reset_mid;    // LO-CSA volts  at FF_MID NEGEDGE (nominal ~0.6 V)

    // R_cal values entered by the operator (Ω, informational / sanity check).
    float r_cal_a_ohm;    // Pass A (HI/MID boundary), e.g. 5600 Ω
    float r_cal_b_ohm;    // Pass B (MID/LO boundary), e.g. 56 Ω

    // Derived: trust window fractions stored so runtime doesn't recompute.
    // I_safe_down = (v_set - offset) / (shunt * gain) * RANGE_CAL_SAFE_FRAC
    float i_trust_hi;     // fine_trust_max for RANGE_HI (A)
    float i_trust_mid;    // fine_trust_max for RANGE_MID (A)

    uint32_t crc;         // CRC32 of the bytes before this field
} range_threshold_cal_t;

// Fraction of the SET-threshold current considered the safe trust-window top.
// 0.85 = 85% of SET: leaves 15% headroom before the flip-flop trips.
#define RANGE_CAL_SAFE_FRAC   0.85f

// ---- Phase state machine (mirrors smu_cal pattern) -------------------------

typedef enum {
    RANGE_CAL_IDLE    = 0,
    RANGE_CAL_PROMPT_A,    // "connect R_CAL_A and press ACK"
    RANGE_CAL_RUNNING_A,   // pass A sweep in progress
    RANGE_CAL_PROMPT_B,    // "connect R_CAL_B and press ACK"
    RANGE_CAL_RUNNING_B,   // pass B sweep in progress
    RANGE_CAL_SUCCESS,
    RANGE_CAL_FAILED,
} range_cal_phase_t;

typedef struct {
    struct daq_board *board;
    range_cal_phase_t phase;
    uint8_t           error_code;  // filled on FAILED; 0 = OK

    // Intermediate results (filled as each pass completes).
    float v_set_hi;
    float v_reset_hi;
    float v_set_mid;
    float v_reset_mid;

    // Operator-supplied R_cal values (set before ACK; default filled by host).
    float r_cal_a_ohm;    // default: 5600.0f
    float r_cal_b_ohm;    // default: 56.0f

    // Volatile progress (0–100 %) for the desktop progress bar.
    volatile uint8_t progress;

    // Internal: background task handle.
    TaskHandle_t task;
} range_cal_engine_t;

// ---- Error codes -----------------------------------------------------------
#define RANGE_CAL_ERR_ADAQ        1   // ADAQ read failed
#define RANGE_CAL_ERR_NO_SET_HI   2   // FF_HI never triggered during pass A up-sweep
#define RANGE_CAL_ERR_NO_RST_HI   3   // FF_HI never un-triggered during pass A down-sweep
#define RANGE_CAL_ERR_NO_SET_MID  4   // FF_MID never triggered during pass B up-sweep
#define RANGE_CAL_ERR_NO_RST_MID  5   // FF_MID never un-triggered during pass B down-sweep
#define RANGE_CAL_ERR_IMPLAUSIBLE 6   // threshold voltages fail sanity check
#define RANGE_CAL_ERR_NVS         7   // NVS write failed

// ---- Lifecycle -------------------------------------------------------------

/**
 * @brief Initialise the range_cal handle and load stored calibration from NVS
 *        into *out_cal (if present and valid). Call from daq_board_init().
 * @param[out] out_cal  Receives the loaded data; un-changed if NVS has no
 *                      valid entry (caller uses hardcoded defaults).
 * @return ESP_OK always (NVS miss is not an error).
 */
esp_err_t range_cal_load(range_threshold_cal_t *out_cal);

/**
 * @brief Start a background calibration task (phase PROMPT_A).
 *        r_cal_a_ohm / r_cal_b_ohm should be set before calling.
 *        Fast acquisition must be stopped by the caller.
 */
esp_err_t range_cal_start(range_cal_engine_t *c, struct daq_board *b);

/**
 * @brief Acknowledge an operator prompt (phase PROMPT_A or PROMPT_B).
 *        Call when the operator has completed the requested physical action
 *        (connected the calibration resistor).
 */
void range_cal_ack(range_cal_engine_t *c);

/**
 * @brief Abort a running calibration and return to IDLE.
 */
void range_cal_abort(range_cal_engine_t *c);

/**
 * @brief Retrieve current status (phase, progress, error_code).
 *        Thread-safe read of volatile fields; may be called any time.
 */
void range_cal_get_status(const range_cal_engine_t *c, range_cal_phase_t *phase,
                          uint8_t *progress, uint8_t *error_code);

#ifdef __cplusplus
}
#endif

