#pragma once

// =============================================================================
// spectrum.h — continuous Welch power-spectrum estimator (esp-dsp FFT).
//
// Fills an analysis buffer one sample at a time from the fused current (or
// power) stream. When the buffer is full it applies a window, runs a real FFT
// (esp-dsp), accumulates |X|^2 into an averaged periodogram (Welch's method,
// hop defaults to 0% overlap -- see spectrum_set_hop() to opt into overlap),
// and produces magnitude bins on demand for the USB FFT frame. The averaging
// depth is capped at SPECTRUM_AVG_MAX so the estimate stays a bounded-depth
// EMA instead of freezing once avg_count grows large.
//
// Sizes are fixed at compile time (max length) and selectable at runtime to a
// power-of-two <= SPECTRUM_MAX_N.
// =============================================================================

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SPECTRUM_MAX_N     4096   // max FFT length
#define SPECTRUM_MAX_BINS  (SPECTRUM_MAX_N / 2)

// Cap on the Welch averaging depth. Without a cap, avg_count grows forever
// and each new periodogram's contribution shrinks as 1/avg_count, so after
// a few thousand FFTs the display stops responding to the live signal at
// all (effectively frozen on the first seconds of data). Clamping turns the
// running average into a proper exponential moving average once the cap is
// hit: psd += (pw - psd) / SPECTRUM_AVG_MAX. 32 periodograms gives a ~5x
// variance reduction (sqrt(32) ~= 5.7x) while still settling to a new signal
// within ~32 hops -- at the hop/sample rates used here that's roughly a 1 s
// time constant, which is responsive enough for a live instrument.
#define SPECTRUM_AVG_MAX   32

typedef enum {
    SPEC_WIN_RECT = 0,
    SPEC_WIN_HANN,
    SPEC_WIN_BLACKMAN_HARRIS,
} spectrum_window_t;

typedef struct {
    uint16_t n;                 // active FFT length (power of two)
    uint16_t nbins;             // n/2
    spectrum_window_t window;
    bool     ready;             // esp-dsp tables initialised
    bool     enabled;           // continuous accumulation on/off

    uint16_t fill;              // samples loaded into the analysis buffer
    uint16_t hop;               // samples between FFTs (default n = no overlap)
    uint16_t since_last;        // samples since the last FFT
    uint32_t avg_count;         // periodograms averaged so far

    float    win[SPECTRUM_MAX_N];        // window coefficients
    float    fft_buf[SPECTRUM_MAX_N * 2];// interleaved re/im work buffer
    float    sample_buf[SPECTRUM_MAX_N]; // raw windowed-input ring
    float    psd[SPECTRUM_MAX_BINS];     // averaged power spectrum
    float    win_power;                  // sum(win^2) for normalisation
} spectrum_t;

/** @brief Initialise esp-dsp FFT tables and configure length + window. */
esp_err_t spectrum_init(spectrum_t *s, uint16_t n, spectrum_window_t window);

/** @brief Reconfigure FFT length / window (rebuilds the window + clears avg). */
esp_err_t spectrum_configure(spectrum_t *s, uint16_t n, spectrum_window_t window);

/** @brief Enable/disable continuous accumulation. */
void spectrum_set_enabled(spectrum_t *s, bool on);

/**
 * @brief Override the FFT hop (samples between FFTs). spectrum_configure()
 *        defaults this to @c n (0% overlap, one FFT per full buffer) to keep
 *        run_fft() -- which executes inline on the acquisition producer task
 *        -- off the critical path as much as possible; call this afterwards
 *        to opt back into overlap (e.g. n/2 for the old 50% Welch behaviour)
 *        for callers that want faster spectral update cadence and can afford
 *        the extra FFT/memmove cost. Clamped to 1..n; out-of-range values are
 *        ignored.
 */
void spectrum_set_hop(spectrum_t *s, uint16_t hop);

/** @brief Clear the averaged periodogram. */
void spectrum_reset(spectrum_t *s);

/**
 * @brief Feed one sample. When enough samples accumulate (hop), an FFT runs and
 *        is folded into the averaged PSD.
 * @return true if an FFT was computed on this call.
 */
bool spectrum_push(spectrum_t *s, float x);

/**
 * @brief Copy the current magnitude spectrum (sqrt of averaged PSD) into @p out.
 * @param out    destination, at least nbins floats.
 * @param max    capacity of @p out.
 * @return number of bins written.
 */
uint16_t spectrum_get_magnitude(const spectrum_t *s, float *out, uint16_t max);

/** @brief Number of active bins (n/2). */
static inline uint16_t spectrum_nbins(const spectrum_t *s) { return s->nbins; }

#ifdef __cplusplus
}
#endif
