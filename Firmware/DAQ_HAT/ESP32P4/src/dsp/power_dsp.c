// =============================================================================
// power_dsp.c — instantaneous power, energy/charge accumulation and running
// statistics.
// =============================================================================

#include "power_dsp.h"
#include <string.h>
#include <math.h>

// -----------------------------------------------------------------------------
// Running-statistics helpers
// -----------------------------------------------------------------------------
static void stat_reset(running_stat_t *s)
{
    s->sum    = 0.0;
    s->sum_sq = 0.0;
    s->min    = INFINITY;
    s->max    = -INFINITY;
    s->count  = 0;
}

static inline void stat_update(running_stat_t *s, float x)
{
    s->sum    += (double)x;
    s->sum_sq += (double)x * (double)x;
    if (x < s->min) s->min = x;
    if (x > s->max) s->max = x;
    s->count++;
}

static void stat_finish(const running_stat_t *s, stat_result_t *out)
{
    out->count = s->count;
    if (s->count == 0) {
        out->min = out->max = out->mean = out->rms = out->std = 0.0f;
        return;
    }
    double n    = (double)s->count;
    double mean = s->sum / n;
    double ms   = s->sum_sq / n;           // mean of squares
    double var  = ms - mean * mean;        // population variance
    if (var < 0.0) var = 0.0;              // guard tiny negative from rounding
    out->min  = (s->min == INFINITY)  ? 0.0f : s->min;
    out->max  = (s->max == -INFINITY) ? 0.0f : s->max;
    out->mean = (float)mean;
    out->rms  = (float)sqrt(ms);
    out->std  = (float)sqrt(var);
}

// -----------------------------------------------------------------------------
// Lifecycle
// -----------------------------------------------------------------------------
void power_dsp_set_rate(power_dsp_t *d, float current_odr_hz)
{
    d->dt = (current_odr_hz > 0.0f) ? (1.0 / (double)current_odr_hz) : 0.0;
}

void power_dsp_init(power_dsp_t *d, float current_odr_hz)
{
    memset(d, 0, sizeof(*d));
    power_dsp_set_rate(d, current_odr_hz);
    power_dsp_reset_energy(d);
    power_dsp_reset_stats(d);
}

void power_dsp_set_voltage(power_dsp_t *d, float volts)
{
    d->voltage = volts;
}

// -----------------------------------------------------------------------------
// Core processing
// -----------------------------------------------------------------------------
float power_dsp_push_current(power_dsp_t *d, float amps)
{
    float v = d->voltage;
    float p = v * amps;

    d->last_i = amps;
    d->last_v = v;
    d->last_p = p;

    // Trapezoidal integration for energy and charge over one sample period.
    if (d->have_prev && d->dt > 0.0) {
        d->charge_c  += 0.5 * ((double)d->prev_i + (double)amps) * d->dt;
        d->energy_j  += 0.5 * ((double)d->prev_p + (double)p)    * d->dt;
        d->elapsed_s += d->dt;
    }
    d->prev_i    = amps;
    d->prev_p    = p;
    d->have_prev = true;

    // Running statistics.
    stat_update(&d->st_i, amps);
    stat_update(&d->st_v, v);
    stat_update(&d->st_p, p);

    return p;
}

// -----------------------------------------------------------------------------
// Accumulator readout
// -----------------------------------------------------------------------------
double power_dsp_energy_j(const power_dsp_t *d)   { return d->energy_j; }
double power_dsp_energy_mwh(const power_dsp_t *d)  { return d->energy_j * (1000.0 / 3600.0); }
double power_dsp_charge_c(const power_dsp_t *d)    { return d->charge_c; }
double power_dsp_charge_mah(const power_dsp_t *d)  { return d->charge_c * (1000.0 / 3600.0); }
double power_dsp_elapsed_s(const power_dsp_t *d)   { return d->elapsed_s; }

void power_dsp_reset_energy(power_dsp_t *d)
{
    d->energy_j   = 0.0;
    d->charge_c   = 0.0;
    d->elapsed_s  = 0.0;
    d->have_prev  = false;
}

// -----------------------------------------------------------------------------
// Statistics
// -----------------------------------------------------------------------------
void power_dsp_get_stats(const power_dsp_t *d, pdsp_signal_t sig,
                         stat_result_t *out)
{
    switch (sig) {
        case PDSP_SIG_V: stat_finish(&d->st_v, out); break;
        case PDSP_SIG_P: stat_finish(&d->st_p, out); break;
        case PDSP_SIG_I:
        default:         stat_finish(&d->st_i, out); break;
    }
}

void power_dsp_reset_stats(power_dsp_t *d)
{
    stat_reset(&d->st_i);
    stat_reset(&d->st_v);
    stat_reset(&d->st_p);
}
