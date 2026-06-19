#pragma once

// =============================================================================
// power_dsp.h — instantaneous power, energy/charge accumulation and running
// statistics for the BugBuster power analyzer.
//
// Fed one fused current sample i[n] (from current_fusion) plus the most recent
// voltage reading v (held / linearly interpolated from the slower VOLTAGE ADAQ).
// Produces:
//   * p[n] = v[n] * i[n]                          (instantaneous power, W)
//   * energy (J and mWh)                          trapezoidal, double accumulator
//   * charge (C and mAh)                           trapezoidal, double accumulator
//   * running statistics over the active window:  min/max/mean/RMS/std of i, v, p
//
// Energy and charge are accumulated in double to keep long-run drift negligible
// even at hundreds of kSPS. The accumulators and the statistics window are
// independently resettable (e.g. between PC-set markers).
//
// This module is allocation-free and intended to be called from the capture /
// processing task at the full sample rate.
// =============================================================================

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Running statistics for one signal over the active window.
typedef struct {
    double   sum;        // sum of values            (for mean)
    double   sum_sq;     // sum of squares           (for RMS / std)
    float    min;
    float    max;
    uint32_t count;
} running_stat_t;

// Snapshot of computed statistics (filled by power_dsp_get_stats).
typedef struct {
    float min, max, mean, rms, std;
    uint32_t count;
} stat_result_t;

typedef struct {
    // Timebase
    double   dt;             // seconds per current sample (1 / current_odr)
    bool     have_prev;      // a previous sample exists (for trapezoidal area)
    float    prev_i;         // previous current (A)
    float    prev_p;         // previous power (W)

    // Latest voltage (held between the slower voltage updates).
    float    voltage;

    // Accumulators (double for drift-free long runs).
    double   energy_j;       // joules
    double   charge_c;       // coulombs
    double   elapsed_s;      // integrated time over the accumulation window

    // Running statistics (current, voltage, power).
    running_stat_t st_i;
    running_stat_t st_v;
    running_stat_t st_p;

    // Latest instantaneous values (for quick readout).
    float    last_i;
    float    last_v;
    float    last_p;
} power_dsp_t;

/**
 * @brief Initialise the DSP for a given current sample rate.
 * @param current_odr_hz  fused current output data rate (samples/second).
 */
void power_dsp_init(power_dsp_t *d, float current_odr_hz);

/** @brief Update the sample period if the current ODR changes. */
void power_dsp_set_rate(power_dsp_t *d, float current_odr_hz);

/**
 * @brief Provide the latest voltage reading (V). Called whenever a new VOLTAGE
 *        ADAQ sample arrives; held/used for every current sample in between.
 */
void power_dsp_set_voltage(power_dsp_t *d, float volts);

/**
 * @brief Process one fused current sample. Computes p = v*i, advances the
 *        energy/charge accumulators (trapezoidal) and updates the statistics.
 * @return the instantaneous power for this sample (W).
 */
float power_dsp_push_current(power_dsp_t *d, float amps);

// ---- Accumulator readout ----------------------------------------------------

/** @brief Accumulated energy in milliwatt-hours. */
double power_dsp_energy_mwh(const power_dsp_t *d);

/** @brief Accumulated energy in joules. */
double power_dsp_energy_j(const power_dsp_t *d);

/** @brief Accumulated charge in milliamp-hours. */
double power_dsp_charge_mah(const power_dsp_t *d);

/** @brief Accumulated charge in coulombs. */
double power_dsp_charge_c(const power_dsp_t *d);

/** @brief Integrated time (seconds) over the current accumulation window. */
double power_dsp_elapsed_s(const power_dsp_t *d);

/** @brief Reset energy/charge/time accumulators (statistics untouched). */
void power_dsp_reset_energy(power_dsp_t *d);

// ---- Statistics -------------------------------------------------------------

typedef enum { PDSP_SIG_I = 0, PDSP_SIG_V, PDSP_SIG_P } pdsp_signal_t;

/** @brief Fill @p out with the statistics of the chosen signal. */
void power_dsp_get_stats(const power_dsp_t *d, pdsp_signal_t sig,
                         stat_result_t *out);

/** @brief Reset the statistics window (accumulators untouched). */
void power_dsp_reset_stats(power_dsp_t *d);

/** @brief Latest instantaneous current / voltage / power. */
static inline float power_dsp_last_i(const power_dsp_t *d) { return d->last_i; }
static inline float power_dsp_last_v(const power_dsp_t *d) { return d->last_v; }
static inline float power_dsp_last_p(const power_dsp_t *d) { return d->last_p; }

#ifdef __cplusplus
}
#endif
