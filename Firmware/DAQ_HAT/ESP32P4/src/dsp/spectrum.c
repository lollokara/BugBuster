// =============================================================================
// spectrum.c — continuous Welch power-spectrum estimator (esp-dsp FFT).
// =============================================================================

#include "spectrum.h"
#include <string.h>
#include <math.h>
#include "esp_log.h"
#include "dsps_fft2r.h"
#include "dsps_wind_hann.h"
#include "dsps_wind_blackman_harris.h"

static const char *TAG = "spectrum";

static bool is_pow2(uint16_t n) { return n && ((n & (n - 1)) == 0); }

static void build_window(spectrum_t *s)
{
    switch (s->window) {
        case SPEC_WIN_HANN:
            dsps_wind_hann_f32(s->win, s->n);
            break;
        case SPEC_WIN_BLACKMAN_HARRIS:
            dsps_wind_blackman_harris_f32(s->win, s->n);
            break;
        case SPEC_WIN_RECT:
        default:
            for (uint16_t i = 0; i < s->n; ++i) s->win[i] = 1.0f;
            break;
    }
    double wp = 0.0;
    for (uint16_t i = 0; i < s->n; ++i) wp += (double)s->win[i] * s->win[i];
    s->win_power = (float)wp;
    if (s->win_power <= 0.0f) s->win_power = 1.0f;
}

esp_err_t spectrum_configure(spectrum_t *s, uint16_t n, spectrum_window_t window)
{
    if (!is_pow2(n) || n < 64 || n > SPECTRUM_MAX_N) {
        return ESP_ERR_INVALID_ARG;
    }
    s->n      = n;
    s->nbins  = (uint16_t)(n / 2);
    s->window = window;
    // Default to 0% overlap (hop = n), not the previous 50%. run_fft() runs
    // INLINE on daq_fast_task, the acquisition producer that must never
    // stall, so every FFT here is on its critical path; 50% overlap doubled
    // the FFT (and post-FFT memmove) rate for statistical smoothing that the
    // bounded-depth EMA in run_fft() now provides far more cheaply. Callers
    // that still want overlap can request it via spectrum_set_hop().
    s->hop    = n;
    s->fill        = 0;
    s->since_last  = 0;
    build_window(s);
    spectrum_reset(s);
    return ESP_OK;
}

esp_err_t spectrum_init(spectrum_t *s, uint16_t n, spectrum_window_t window)
{
    memset(s, 0, sizeof(*s));
    esp_err_t err = dsps_fft2r_init_fc32(NULL, SPECTRUM_MAX_N);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "dsps_fft2r_init_fc32 failed: %s", esp_err_to_name(err));
        return err;
    }
    s->ready = true;
    return spectrum_configure(s, n, window);
}

void spectrum_set_enabled(spectrum_t *s, bool on)
{
    s->enabled = on;
}

void spectrum_set_hop(spectrum_t *s, uint16_t hop)
{
    if (hop < 1 || hop > s->n) {
        return;   // out of range -- leave the current hop untouched
    }
    s->hop = hop;
}

void spectrum_reset(spectrum_t *s)
{
    memset(s->psd, 0, sizeof(s->psd));
    s->avg_count = 0;
}

// Run one FFT over the current analysis buffer and fold |X|^2 into the PSD.
static void run_fft(spectrum_t *s)
{
    // Load interleaved complex: real = windowed sample, imag = 0.
    for (uint16_t i = 0; i < s->n; ++i) {
        s->fft_buf[2 * i]     = s->sample_buf[i] * s->win[i];
        s->fft_buf[2 * i + 1] = 0.0f;
    }
    dsps_fft2r_fc32(s->fft_buf, s->n);
    dsps_bit_rev_fc32(s->fft_buf, s->n);

    float norm = 1.0f / s->win_power;

    // Running average of periodograms (Welch), clamped to a bounded-depth EMA.
    // Unclamped, avg_count grows forever and each new periodogram's weight
    // (1/avg_count) decays towards zero, so after a few thousand FFTs the
    // display silently freezes on the first seconds of the session. Decide
    // this frame's weight BEFORE folding it in (not after, as a naive
    // clamp-then-divide-by-avg_count+1 would do) so that once avg_count
    // saturates at SPECTRUM_AVG_MAX the divisor stays pinned there instead of
    // drifting to SPECTRUM_AVG_MAX+1. With the divisor pinned, this is
    // exactly a first-order EMA with alpha = 1/SPECTRUM_AVG_MAX -- still
    // converges to the true mean under a stationary signal, just with a
    // bounded ~1 s time constant instead of an ever-growing one.
    bool first = (s->avg_count == 0);
    if (s->avg_count < SPECTRUM_AVG_MAX) {
        s->avg_count++;
    }
    float inv_count = 1.0f / (float)s->avg_count;

    for (uint16_t k = 0; k < s->nbins; ++k) {
        float re = s->fft_buf[2 * k];
        float im = s->fft_buf[2 * k + 1];
        float pw = (re * re + im * im) * norm;
        if (first) {
            s->psd[k] = pw;
        } else {
            s->psd[k] += (pw - s->psd[k]) * inv_count;
        }
    }
}

bool spectrum_push(spectrum_t *s, float x)
{
    if (!s->enabled || !s->ready) {
        return false;
    }
    // Fill the analysis buffer; when full, run an FFT and slide down by `hop`
    // samples (default: hop == n, i.e. no overlap -- see spectrum_set_hop()).
    // A smaller hop reintroduces overlap; the memmove cost is amortised over
    // hop samples either way, not 1.
    s->sample_buf[s->fill++] = x;
    if (s->fill < s->n) {
        return false;
    }
    run_fft(s);
    memmove(s->sample_buf, s->sample_buf + s->hop,
            (s->n - s->hop) * sizeof(float));
    s->fill = (uint16_t)(s->n - s->hop);
    return true;
}

uint16_t spectrum_get_magnitude(const spectrum_t *s, float *out, uint16_t max)
{
    uint16_t n = (s->nbins < max) ? s->nbins : max;
    for (uint16_t k = 0; k < n; ++k) {
        out[k] = sqrtf(s->psd[k]);
    }
    return n;
}
