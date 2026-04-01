#pragma once

// =============================================================================
// selftest.h - Self-Test, Calibration, and E-fuse Current Monitoring
//
// Uses U23 (5th ADGS2414D in the daisy-chain) to route internal power rails
// and e-fuse IMON pins to AD74416H Channel D for measurement.
//
// Key features:
//   - Boot-time supply verification (VADJ1, VADJ2, 3V3_ADJ)
//   - Automatic IDAC calibration (no external connections needed)
//   - Background e-fuse current monitoring via IMON pins
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

// E-fuse identifiers (1-4 to match IO_Block numbering)
#define SELFTEST_EFUSE_1        1
#define SELFTEST_EFUSE_2        2
#define SELFTEST_EFUSE_3        3
#define SELFTEST_EFUSE_4        4
#define SELFTEST_EFUSE_COUNT    4

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

// E-fuse current monitoring result
typedef struct {
    float    current_a[SELFTEST_EFUSE_COUNT];  // measured current in amps (-1 = unavailable)
    uint32_t timestamp_ms;                      // when last measured
    bool     available;                         // false if U17 S2 is closed
} SelftestEfuseCurrents;

// Supply voltage monitoring result (background)
typedef struct {
    float    voltage[SELFTEST_RAIL_COUNT];     // VADJ1, VADJ2, 3V3_ADJ in volts (-1 = inactive)
    uint32_t timestamp_ms;
    bool     available;
} SelftestSupplyVoltages;

// Calibration result
typedef struct {
    uint8_t  status;            // CAL_STATUS_*
    uint8_t  channel;           // IDAC channel being calibrated (1 or 2)
    uint8_t  points_collected;  // number of cal points measured so far
    float    error_mv;          // final error after calibration (if success)
} SelftestCalResult;

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
 * @brief  Measure a single supply rail using U23 + ADC Channel D.
 *
 * @param rail  SELFTEST_RAIL_VADJ1, SELFTEST_RAIL_VADJ2, or SELFTEST_RAIL_3V3_ADJ
 * @return Measured voltage in volts (corrected for divider), or -1.0f on error.
 */
float selftest_measure_supply(uint8_t rail);

/**
 * @brief  Measure a single e-fuse output current via IMON.
 *
 * @param efuse  E-fuse number (1–4)
 * @return Current in amps, or -1.0f if unavailable (U17 S2 closed or error).
 */
float selftest_measure_efuse_current(uint8_t efuse);

/**
 * @brief  Get the cached e-fuse current readings (from background monitoring).
 *
 * @return Pointer to the e-fuse currents (static, updated periodically).
 */
const SelftestEfuseCurrents* selftest_get_efuse_currents(void);

/**
 * @brief  Get the cached supply voltage readings (from background monitoring).
 */
const SelftestSupplyVoltages* selftest_get_supply_voltages(void);

/**
 * @brief  Non-blocking background monitor step.
 *         Call periodically (e.g. every 200 ms from a task).
 *         Each call measures ONE channel (e-fuse or supply), cycling through
 *         all active channels over multiple calls.  This avoids blocking the
 *         main loop while still respecting MUX dead-time between switches.
 *
 *         Only runs if U17 S2 is open and calibration is not active.
 *         Monitors only active e-fuses and enabled voltage rails.
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

/**
 * @brief  Check if self-test / calibration is currently using U23.
 *         When true, U17 S2 must not be closed.
 */
bool selftest_is_busy(void);

#ifdef __cplusplus
}
#endif
