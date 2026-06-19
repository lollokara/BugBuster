#pragma once

// =============================================================================
// spectrum.h — continuous Welch power-spectrum estimator (esp-dsp FFT).
//
// Fills an analysis buffer one sample at a time from the fused current (or
// power) stream. When the buffer is full it applies a window, runs a real FFT
// (esp-dsp), accumulates |X|^2 into an averaged periodogram (Welch's method
// with 50% overlap), and produces magnitude bins on demand for the USB FFT
// frame.
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
    uint16_t hop;               // samples between FFTs (50% overlap = n/2)
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
