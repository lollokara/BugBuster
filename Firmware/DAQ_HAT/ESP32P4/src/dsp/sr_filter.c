// =============================================================================
// sr_filter.c — Super-Resolution decimating low-pass filter (see sr_filter.h).
// =============================================================================

#include "sr_filter.h"

#include <math.h>
#include <string.h>

#include "dsps_dotprod.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// Blackman-windowed sinc low-pass. fc is normalised to the INPUT rate
// (cycles/sample), so fc = 0.45/decim puts the cutoff just under the output
// Nyquist of 0.5/decim.
static void design_lowpass(float *coef, uint16_t ntaps, double fc)
{
    const double mid = (double)(ntaps - 1) / 2.0;
    double sum = 0.0;

    for (uint16_t i = 0; i < ntaps; ++i) {
        double n = (double)i - mid;
        double sinc = (fabs(n) < 1e-9)
                          ? 2.0 * fc
                          : sin(2.0 * M_PI * fc * n) / (M_PI * n);
        double w = 0.42
                   - 0.5 * cos(2.0 * M_PI * (double)i / (double)(ntaps - 1))
                   + 0.08 * cos(4.0 * M_PI * (double)i / (double)(ntaps - 1));
        double h = sinc * w;
        coef[i] = (float)h;
        sum += h;
    }

    // Unity DC gain, so the filter cannot bias the calibrated amps/volts scale.
    if (sum != 0.0) {
        for (uint16_t i = 0; i < ntaps; ++i) coef[i] = (float)(coef[i] / sum);
    }
}

esp_err_t sr_filter_init(sr_filter_t *f, uint16_t decim)
{
    if (!f || decim == 0 || decim > SR_FILTER_MAX_DECIM) return ESP_ERR_INVALID_ARG;

    memset(f, 0, sizeof(*f));
    f->decim = decim;

    if (decim == 1) {
        f->ntaps = 0;          // pass-through
        f->ready = true;
        return ESP_OK;
    }

    uint32_t ntaps = 8u * decim + 1u;
    if (ntaps > SR_FILTER_MAX_TAPS) ntaps = SR_FILTER_MAX_TAPS;
    if (!(ntaps & 1u)) ntaps -= 1u;      // odd length -> integer group delay
    f->ntaps = (uint16_t)ntaps;

    design_lowpass(f->coef, f->ntaps, 0.45 / (double)decim);
    f->ready = true;
    return ESP_OK;
}

void sr_filter_reset(sr_filter_t *f)
{
    if (!f) return;
    memset(f->hist, 0, sizeof(f->hist));
    f->pos   = 0;
    f->count = 0;
}

bool sr_filter_push(sr_filter_t *f, float in, float *out)
{
    if (!f || !f->ready || !out) return false;

    if (f->ntaps == 0) {                 // decim == 1, nothing to do
        *out = in;
        return true;
    }

    // Mirrored write keeps a contiguous ntaps window available at every pos.
    f->hist[f->pos]            = in;
    f->hist[f->pos + f->ntaps] = in;
    f->pos = (uint16_t)((f->pos + 1u) % f->ntaps);

    if (++f->count < f->decim) return false;
    f->count = 0;

    float acc = 0.0f;
    dsps_dotprod_f32(&f->hist[f->pos], f->coef, &acc, f->ntaps);
    *out = acc;
    return true;
}
