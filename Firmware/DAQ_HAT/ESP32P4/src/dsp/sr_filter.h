#pragma once

// =============================================================================
// sr_filter.h — Super-Resolution decimating low-pass filter.
//
// Second half of the two-stage noise-reduction chain used by SR mode:
//
//   stage 1 (analog/ADC) : ADAQ7769-1 Sinc3 @ maximum decimation — the lowest
//                          noise-bandwidth filter the part offers.
//   stage 2 (this file)  : windowed-sinc FIR low-pass + integer decimation down
//                          to the SR output rate (1 ksps current / 500 sps
//                          voltage).
//
// The FIR does the anti-alias filtering that a bare "keep 1 of N" decimator
// lacks: without it, stage-1 broadband noise above the SR Nyquist would fold
// straight back into the passband and the extra resolution would be lost.
// Averaging N samples buys ~sqrt(N) noise reduction, so the effective
// resolution gain is log2(sqrt(N)) bits.
//
// Allocation-free and called from the acquisition task, so the MAC uses
// esp-dsp's dsps_dotprod_f32 and the history buffer is kept contiguous
// (mirrored) to avoid a modulo in the inner loop.
// =============================================================================

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Longest filter we will build. 8 taps per decimation stage gives a transition
// band narrow enough that the stop-band starts before the output Nyquist.
#define SR_FILTER_MAX_TAPS   129
// The voltage path decimates from VOLTAGE_ODR_TARGET_SPS (~50-64 ksps) down to
// DAQ_SR_VOLTAGE_SPS, so the factor is far larger than the current path's.
#define SR_FILTER_MAX_DECIM  256

typedef struct {
    // Mirrored history: hist[pos] and hist[pos + ntaps] hold the same sample so
    // the dot product can always read a contiguous ntaps-long window.
    float    hist[2 * SR_FILTER_MAX_TAPS] __attribute__((aligned(16)));
    float    coef[SR_FILTER_MAX_TAPS]     __attribute__((aligned(16)));
    uint16_t ntaps;
    uint16_t pos;        // write cursor in [0, ntaps)
    uint16_t decim;      // input samples per output sample
    uint16_t count;      // inputs seen since the last output
    bool     ready;
} sr_filter_t;

/**
 * @brief Design a low-pass decimator for @p decim : 1.
 *
 * Cutoff is placed at 0.45 / decim (normalised to the INPUT rate), i.e. just
 * below the output Nyquist, using a Blackman-windowed sinc. Passing decim == 1
 * configures a pass-through (no filtering, no delay).
 *
 * @return ESP_ERR_INVALID_ARG when @p decim exceeds SR_FILTER_MAX_DECIM.
 */
esp_err_t sr_filter_init(sr_filter_t *f, uint16_t decim);

/** @brief Drop all history, keeping the designed coefficients. */
void sr_filter_reset(sr_filter_t *f);

/**
 * @brief Feed one input sample.
 *
 * @param out  Receives the filtered sample on the 1-in-@c decim calls that
 *             produce one; untouched otherwise.
 * @return true when @p out was written.
 */
bool sr_filter_push(sr_filter_t *f, float in, float *out);

#ifdef __cplusplus
}
#endif
