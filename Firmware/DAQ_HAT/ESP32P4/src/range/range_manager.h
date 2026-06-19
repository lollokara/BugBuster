#pragma once

// =============================================================================
// range_manager.h — current-range observation, override, FINE input-mux control
// and per-range calibration for the BugBuster power analyzer front-end.
//
// Hardware model (FIRMWARE_HARDWARE_REFERENCE.md, netlist-verified):
//   * Shunt ladder 51 / 2 / 0.05 ohm. The 50 mohm (R30) is always in the path.
//   * Analog SR latches (U12 + ADCMP600) auto-close two bypass switches as the
//     DUT current rises:
//         GPIO52 -> U9  (51 ohm bypass)   HIGH = 51 ohm shorted
//         GPIO53 -> U13 (2 ohm bypass)    HIGH = 2 ohm shorted
//     Both lines are tapped through 1k resistors: read them as high-Z inputs to
//     OBSERVE the latch-selected range; drive them as outputs to OVERRIDE.
//   * The FINE ADAQ (U1) reads the 51 ohm OR 2 ohm CSA, selected by the U24
//     mux (A0=GPIO50, A1=GPIO51, EN=GPIO49). The range manager keeps the mux
//     pointed at the CSA matching the active range.
//
// Range decode from (bypass51, bypass2):
//     (0,0) = HI  (51 ohm)   -> FINE, mux -> 51 ohm CSA
//     (1,0) = MID (2 ohm)    -> FINE, mux -> 2 ohm CSA
//     (1,1) = LO  (50 mohm)  -> COARSE ADAQ
//     (0,1) = transition / invalid
//
// This module does not sample the ADCs; it is handed an ADAQ voltage (already
// in volts) plus the range and returns amps.
// =============================================================================

#include <stdint.h>
#include <stdbool.h>
#include "driver/gpio.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    RANGE_HI = 0,   // 51 ohm   — nA .. ~1.4 mA  (FINE ADAQ)
    RANGE_MID,      // 2 ohm    — ~1.4 .. 37 mA  (FINE ADAQ)
    RANGE_LO,       // 0.05 ohm — ~37 mA .. 3 A  (COARSE ADAQ)
    RANGE_COUNT,
    RANGE_UNKNOWN = 0xFF,
} current_range_t;

// Per-range calibration:
//   I = (v_adc - offset_v) / (shunt_ohm * amp_gain) * gain_corr
typedef struct {
    float shunt_ohm;     // nominal/measured shunt for this range
    float amp_gain;      // current-sense amplifier gain (V/V)
    float offset_v;      // measured zero-current output offset (V)
    float gain_corr;     // multiplicative gain correction (1.0 = ideal)
} range_cal_t;

typedef struct {
    // Bidirectional bypass-control lines.
    gpio_num_t bypass51_pin;   // U9  (51 ohm bypass)
    gpio_num_t bypass2_pin;    // U13 (2 ohm bypass)

    // FINE input mux (U24).
    gpio_num_t mux_a0_pin;
    gpio_num_t mux_a1_pin;
    gpio_num_t mux_en_pin;     // shared U24/U25 enable

    // Calibration per range.
    range_cal_t cal[RANGE_COUNT];

    // Runtime state.
    current_range_t current;
    current_range_t previous;
    volatile uint32_t change_count;
    bool override_active;
} range_manager_t;

/**
 * @brief Initialise: bypass lines as inputs (observe mode), mux pins as outputs,
 *        enable the mux, load default calibration from the SHUNT_*_OHM /
 *        ISENSE_AMP_GAIN constants, and read the initial range.
 */
esp_err_t range_manager_init(range_manager_t *rm);

/**
 * @brief Read the bypass lines, update the observed range, and (if the range
 *        changed) re-point the FINE mux to the matching CSA.
 * @return the freshly read range.
 */
current_range_t range_manager_poll(range_manager_t *rm);

/** @brief Last observed range without re-reading the GPIOs. */
current_range_t range_manager_current(const range_manager_t *rm);

/** @brief True if the most recent poll observed a different range than before. */
bool range_manager_changed(range_manager_t *rm);

/** @brief True while the active range is a transition/invalid decode. */
bool range_manager_in_transition(const range_manager_t *rm);

/**
 * @brief Force a specific range by driving the bypass lines as outputs (and
 *        setting the FINE mux). Pass RANGE_UNKNOWN to release the override:
 *        the bypass lines return to high-Z inputs and the analog loop resumes.
 */
esp_err_t range_manager_force(range_manager_t *rm, current_range_t range);

/** @brief Point the FINE input mux at the CSA for a given range (HI/MID). */
esp_err_t range_manager_set_fine_mux(range_manager_t *rm, current_range_t range);

/** @brief Convert an ADAQ voltage reading + range tag into amps. */
float range_manager_volts_to_amps(const range_manager_t *rm,
                                   current_range_t range, float v_adc);

/** @brief True if a range is read by the COARSE ADAQ (LO), false for FINE. */
bool range_uses_coarse(current_range_t range);

/** @brief Install per-range calibration (e.g. loaded from NVS). */
void range_manager_set_cal(range_manager_t *rm, current_range_t range,
                           const range_cal_t *cal);

/** @brief Read the current calibration for a range. */
const range_cal_t *range_manager_get_cal(const range_manager_t *rm,
                                         current_range_t range);

/** @brief Nominal shunt for a range (ohms). */
float range_manager_shunt_ohm(const range_manager_t *rm, current_range_t range);

/** @brief Human-readable range name for logging. */
const char *range_manager_name(current_range_t range);

#ifdef __cplusplus
}
#endif
