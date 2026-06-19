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
    s->hop    = (uint16_t)(n / 2);   // 50% overlap
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
    for (uint16_t k = 0; k < s->nbins; ++k) {
        float re = s->fft_buf[2 * k];
        float im = s->fft_buf[2 * k + 1];
        float pw = (re * re + im * im) * norm;
        // Running average of periodograms (Welch).
        if (s->avg_count == 0) {
            s->psd[k] = pw;
        } else {
            s->psd[k] += (pw - s->psd[k]) / (float)(s->avg_count + 1);
        }
    }
    s->avg_count++;
}

bool spectrum_push(spectrum_t *s, float x)
{
    if (!s->enabled || !s->ready) {
        return false;
    }
    // Fill the analysis buffer; when full, run an FFT and slide down by `hop`
    // (50% overlap) so the memmove cost is amortised over hop samples, not 1.
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
