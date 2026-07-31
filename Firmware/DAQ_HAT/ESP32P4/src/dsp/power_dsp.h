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
//
// SHIFTED-DATA variance: every sample is accumulated as an offset from `k`
// (the first sample of the window) rather than in absolute terms.
//
// The naive sum/sum_sq -> mean_of_squares - mean^2 form catastrophically
// cancels when there is a large DC offset and small AC ripple -- the normal
// case for a supply rail -- because sum_sq and mean*mean are then both huge
// and nearly equal. Subtracting a constant `k` shifts the data to ~zero mean
// without changing the variance (variance is translation-invariant), so the
// accumulators stay O(ripple^2) instead of O(DC^2) and the cancellation is
// gone.
//
// Welford's algorithm fixes the same numerical problem but needs a DIVISION
// per sample (mean += delta/n), and that is unaffordable here. The ESP32-P4's
// FPU is SINGLE precision only, so every double-precision operation on this
// path is a SOFTWARE-emulated call, and stat_update runs 3x per fused sample
// at up to 128 kSPS.
//
// Measured end-to-end on the bench (tests/tools/daq_usb_stream_bench.py, ODR
// ratio 64, dspdecim=1, WAVE_I samples/s delivered to a real USB host):
//     original naive sum/sum_sq ......... 73,514 Sa/s
//     Welford (double divide/sample) .... 46,175 Sa/s   (-38%)
//     shifted-data, all double ..........  67,900 Sa/s   (-8%)
//     shifted-data, float subtract ...... 76,308 Sa/s   (+3.8%)
//
// Shifted-data is what is implemented: division-free in the update, with the
// subtract/square in hardware single precision. See stat_update() for why the
// float subtract is safe here.
//
// RMS is derived at finish time from the identity mean(x^2) = var + mean^2,
// which is algebraically exact, so no separate sum-of-squares accumulator is
// needed -- strictly cheaper than the code it replaced.
//
// BLOCK ACCUMULATION: the per-sample path is now entirely single precision.
// Deviations are summed into `acc_*` in hardware float and folded into the
// double totals only once every PDSP_FOLD_N samples, so the amortised cost of
// a software double add drops by that factor. Precision is preserved because
// the shift already makes every term small and same-magnitude, so a block of
// PDSP_FOLD_N of them sums cleanly in float, and the block totals themselves
// are accumulated in double -- this is ordinary two-level (blocked) summation,
// which is MORE accurate than naive double summation over millions of samples,
// not less.
#define PDSP_FOLD_N  512u

typedef struct {
    float    k;           // shift origin: value of the first sample in the window
    float    acc_d;       // block sum of (x - k), folded every PDSP_FOLD_N
    float    acc_d2;      // block sum of (x - k)^2, folded every PDSP_FOLD_N
    uint32_t block_n;     // samples in the current unfolded block
    double   sum_d;       // folded total of (x - k)
    double   sum_d2;      // folded total of (x - k)^2
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

    // Accumulators. Same blocked-summation trick as running_stat_t: the
    // per-sample trapezoid areas are summed in float in PERIOD units (dt
    // factored out, since it is constant), and folded into the double totals
    // every PDSP_FOLD_N periods. That removes the last double multiply-adds
    // from the per-sample path.
    //
    // Elapsed time is folded the same way rather than summed per sample. It
    // cannot be a plain period-count x dt, because dt changes when the rate or
    // dspdecim changes mid-session and the periods already banked were taken
    // at the OLD dt.
    double   energy_j;       // joules   (folded total)
    double   charge_c;       // coulombs (folded total)
    float    acc_charge;     // block sum of 0.5*(i_prev+i)*n, in amp-periods
    float    acc_energy;     // block sum of 0.5*(p_prev+p)*n, in watt-periods
    double   elapsed_s;      // integrated time (folded total)
    uint32_t acc_periods;    // periods in the current unfolded block

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

/**
 * @brief Same as power_dsp_push_current(), but integrates energy/charge over
 *        n_periods * d->dt instead of a fixed one sample period (d->dt).
 *
 * Use this when the caller knows more samples' worth of real time elapsed
 * since the previous push than the fixed-dt call would assume -- e.g. a
 * dropped/discarded sample between two pushes -- so the trapezoidal area
 * between prev and this sample is stretched over the true elapsed time
 * instead of silently under-integrating. Still trapezoidal between prev_i/
 * prev_p and the new sample (a straight-line approximation over the whole
 * n_periods span, which is the best available without the missing samples'
 * actual values). power_dsp_push_current() is a thin wrapper for n_periods=1.
 *
 * @param n_periods  number of sample periods (multiples of d->dt) elapsed
 *                    since the previous call. Must be >= 1 for a real push;
 *                    passing 0 advances nothing (statistics still update).
 * @return the instantaneous power for this sample (W).
 */
float power_dsp_push_current_n(power_dsp_t *d, float amps, uint32_t n_periods);

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

/** @brief The held voltage (set by power_dsp_set_voltage), for computing an
 *         instantaneous p = v*i per sample without running the full DSP. */
static inline float power_dsp_voltage(const power_dsp_t *d) { return d->voltage; }

#ifdef __cplusplus
}
#endif
