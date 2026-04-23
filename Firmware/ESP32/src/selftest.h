#pragma once

// =============================================================================
// selftest.h - Self-Test, Calibration, and Supply Rail Monitoring
//
// Uses U23 (5th ADGS2414D in the daisy-chain) to route internal power rails
// to AD74416H Channel D for measurement.
//
// Key features:
//   - Boot-time supply verification (VADJ1, VADJ2, 3V3_ADJ)
//   - Automatic IDAC calibration (no external connections needed)
//   - Background supply rail monitoring (VADJ1, VADJ2, 3V3_ADJ)
//   - Safety interlocks with U17 S2 (IO 10 analog mode)
//
// PCB mode only — all functions are no-ops when ADGS_HAS_SELFTEST == 0.
// =============================================================================

#include <stdint.h>
#include <stdbool.h>
#include "config.h"

#ifdef __cplusplus
extern "C" {
#endif

// Supply rail identifiers for selftest_measure_supply()
#define SELFTEST_RAIL_VADJ1     0   // VADJ1_BUCK (via voltage divider)
#define SELFTEST_RAIL_VADJ2     1   // VADJ2_BUCK (via voltage divider)
#define SELFTEST_RAIL_3V3_ADJ   2   // 3V3_ADJ / VLOGIC (direct)
#define SELFTEST_RAIL_COUNT     3

// Calibration status
#define CAL_STATUS_IDLE         0
#define CAL_STATUS_RUNNING      1
#define CAL_STATUS_SUCCESS      2
#define CAL_STATUS_FAILED       3

// Boot self-test result
typedef struct {
    bool     ran;                           // true if boot test was executed
    bool     passed;                        // true if all supplies within tolerance
    float    vadj1_v;                       // measured VADJ1 voltage (or -1 if not measured)
    float    vadj2_v;                       // measured VADJ2 voltage
    float    vlogic_v;                      // measured 3V3_ADJ voltage
} SelftestBootResult;

// Background supply rail monitoring result
typedef struct {
    float    voltage[SELFTEST_RAIL_COUNT];  // VADJ1, VADJ2, 3V3_ADJ; -1.0f if disabled or unavailable
    uint32_t timestamp_ms;                  // when last measured
    bool     available;                     // false if U17 S2 interlock closed
} SelftestSupplyVoltages;

// Calibration result
typedef struct {
    uint8_t  status;            // CAL_STATUS_*
    uint8_t  channel;           // IDAC channel being calibrated (1 or 2)
    uint8_t  points_collected;  // number of cal points measured so far
    float    last_measured_v;   // latest measured rail voltage during sweep (-1 if none yet)
    float    error_mv;          // final error after calibration (if success)
} SelftestCalResult;

// Persistent trace of last calibration stage across warm resets.
typedef struct {
    uint32_t magic;
    uint8_t  stage;             // implementation-defined stage marker
    uint8_t  channel;
    uint8_t  point;
    int8_t   code;
    int32_t  measured_mv;
    uint8_t  active;            // 1 while calibration in progress
} SelftestCalTrace;

/**
 * @brief  Initialize the self-test module.
 *         Sets up internal state.  Does NOT run the boot test automatically —
 *         call selftest_boot_check() after all supplies are enabled.
 */
void selftest_init(void);

/**
 * @brief  Run the boot-time supply verification.
 *         Measures VADJ1, VADJ2, and 3V3_ADJ using U23.
 *         Requires ±15V supply and VADJ regulators to be already enabled.
 *
 * @return Pointer to the boot result (static, valid until next call).
 */
const SelftestBootResult* selftest_boot_check(void);

/**
 * @brief  Get cached boot self-test result without triggering new measurements.
 */
const SelftestBootResult* selftest_get_boot_result(void);

/**
 * @brief  Measure a single supply rail using U23 + ADC Channel D.
 *
 * @param rail  SELFTEST_RAIL_VADJ1, SELFTEST_RAIL_VADJ2, or SELFTEST_RAIL_3V3_ADJ
 * @return Measured voltage in volts (corrected for divider), or -1.0f on error.
 */
float selftest_measure_supply(uint8_t rail);

/**
 * @brief  Get the cached supply rail voltages (from background monitoring).
 *
 * @return Pointer to the supply voltages (static, updated periodically).
 */
const SelftestSupplyVoltages* selftest_get_supply_voltages(void);

/**
 * @brief  Get whether the background supply monitor worker is enabled.
 *         Default is disabled; persisted in NVS key selftest/st_worker_en.
 */
bool selftest_worker_enabled(void);

/**
 * @brief  Enable or disable the background supply monitor worker.
 *         The new state is persisted to NVS.
 *
 * @return true if the in-memory state was updated and NVS commit succeeded.
 */
bool selftest_set_worker_enabled(bool enabled);

/**
 * @brief  True when the supply monitor is enabled and not blocked by
 *         calibration or the Channel D analog interlock.
 */
bool selftest_is_supply_monitor_active(void);

/**
 * @brief  Non-blocking background monitor step.
 *         Call periodically (e.g. every 200 ms from a task).
 *         Each call measures ONE supply rail, cycling through all three rails
 *         over multiple calls.  This avoids blocking the main loop while still
 *         respecting MUX dead-time between switches.
 *
 *         Only runs if U17 S2 is open and calibration is not active.
 *         Skips rails whose PCA9535 enable bit is false (except 3V3_ADJ).
 */
void selftest_monitor_step(void);

/**
 * @brief  Start automatic IDAC calibration for a supply channel.
 *         Sweeps IDAC codes, measures actual output voltage via U23,
 *         builds calibration curve, and saves to NVS.
 *
 * @param idac_channel  IDAC channel to calibrate (1 = VADJ1, 2 = VADJ2)
 * @return true if calibration started, false if interlock or already running.
 */
bool selftest_start_auto_calibrate(uint8_t idac_channel);

/**
 * @brief  Get the current calibration status / result.
 */
const SelftestCalResult* selftest_get_cal_result(void);
const SelftestCalTrace* selftest_get_cal_trace(void);
void selftest_clear_cal_trace(void);

/**
 * @brief  Check if self-test / calibration is currently using U23.
 *         When true, U17 S2 must not be closed.
 */
bool selftest_is_busy(void);

// ── Internal ADC supply monitoring ──────────────────────────────────────────
// Uses the AD74416H's built-in diagnostic slots to measure internal supplies.
// Does NOT use U23 — works in both breadboard and PCB mode.

// AD74416H diagnostic source codes
#define DIAG_SRC_AGND       0
#define DIAG_SRC_TEMP       1
#define DIAG_SRC_DVCC       2   // Digital supply (breadboard: 5V, PCB: 3.3V)
#define DIAG_SRC_AVCC       3   // Analog supply (5V)
#define DIAG_SRC_LDO1V8     4   // Internal LDO (1.8V)
#define DIAG_SRC_AVDD_HI    5   // Positive analog supply (breadboard: 21.5V, PCB: 15V)
#define DIAG_SRC_AVDD_LO    6   // AVDD_LO
#define DIAG_SRC_AVSS       7   // Negative analog supply (breadboard: -16V, PCB: -15V)

// Internal supply measurement result
typedef struct {
    float avdd_hi_v;    // Positive analog supply
    float dvcc_v;       // Digital supply
    float avcc_v;       // Analog supply (AVCC)
    float avss_v;       // Negative analog supply
    float temp_c;       // Die temperature
    bool  valid;        // true if measurement was successful
    bool  supplies_ok;  // true if all supplies within expected range
} SelftestInternalSupplies;

/**
 * @brief  Measure all internal AD74416H supplies using diagnostic slots.
 *         Temporarily reconfigures diag slots 0–3, measures, then restores
 *         the original slot configuration.
 *
 * @return Pointer to static result (valid until next call).
 */
const SelftestInternalSupplies* selftest_measure_internal_supplies(void);

#ifdef __cplusplus
}
#endif
