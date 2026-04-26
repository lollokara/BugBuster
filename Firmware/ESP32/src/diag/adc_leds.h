#pragma once

// =============================================================================
// adc_leds.h - AD74416H on-chip GPIO status LED driver
//
// Maps the 6 on-chip GPIOs (A..F) to 3 pairs of green/red status LEDs:
//   AB: Channel fault status    (A=green, B=red)
//   CD: Self-test & calibration (C=green, D=red)
//   EF: Supply / reference      (E=green, F=red)
//
// Auto-update is suspended when manual override is active (e.g. host is
// driving GPIOs via BBP_CMD_SET_GPIO_VALUE for test purposes).
// =============================================================================

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Configure AD74416H GPIOs A..F as push-pull outputs, drive all LOW,
 *        and set internal state to INIT_PENDING. Call once after device.begin().
 */
void adc_leds_init(void);

/**
 * @brief Read alert status registers and update all 6 LEDs.
 *        Throttled internally to ~200 ms minimum between updates.
 *        No-op if manual override is active.
 */
void adc_leds_tick(void);

/**
 * @brief Enable or disable manual override of LED GPIOs.
 *        true  = suspend auto-updates (host controls GPIOs directly).
 *        false = resume auto-updates.
 */
void adc_leds_set_manual(bool manual);

/**
 * @brief Return whether manual override is currently active.
 */
bool adc_leds_manual_active(void);

#ifdef __cplusplus
}
#endif
