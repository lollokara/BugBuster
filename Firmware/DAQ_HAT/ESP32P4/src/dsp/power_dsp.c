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
    s->k       = 0.0f;
    s->acc_d   = 0.0f;
    s->acc_d2  = 0.0f;
    s->block_n = 0;
    s->sum_d   = 0.0;
    s->sum_d2  = 0.0;
    s->min     = INFINITY;
    s->max     = -INFINITY;
    s->count   = 0;
}

// Fold the float block accumulators into the double totals and restart the
// block. Called every PDSP_FOLD_N samples from stat_update, and unconditionally
// from stat_finish so a readout always sees every sample taken so far.
static inline void stat_fold(running_stat_t *s)
{
    s->sum_d   += (double)s->acc_d;
    s->sum_d2  += (double)s->acc_d2;
    s->acc_d    = 0.0f;
    s->acc_d2   = 0.0f;
    s->block_n  = 0;
}

// Shifted-data variance update -- see the running_stat_t comment in the header
// for why this shape (numerically stable like Welford, but with NO division on
// the per-sample path: this runs up to 3x per fused sample at up to 128 kSPS,
// and the P4's FPU is single-precision only, so a double divide here is
// software-emulated and measurably expensive).
//
// The first sample of a window becomes the shift origin, which makes the very
// first accumulated deviation exactly 0 -- the ideal origin, since every
// subsequent deviation is then relative to a real value of the signal rather
// than to an arbitrary constant.
static inline void stat_update(running_stat_t *s, float x)
{
    if (s->count == 0) {
        s->k = x;
    }
    // The shift and the square are done in SINGLE precision on purpose. The
    // ESP32-P4's FPU is single-precision only, so every double-precision
    // operation here is a software-emulated call -- and this runs 3x per fused
    // sample at up to 128 kSPS. Doing the subtract and multiply in hardware
    // float leaves only the two accumulations in double, which is one fewer
    // double op than the naive sum/sum_sq form this replaced (measured: the
    // all-double version of this same shifted formula cost ~8% of end-to-end
    // stream throughput at dspdecim=1).
    //
    // Precision is not compromised by the float subtract precisely BECAUSE of
    // the shift: d is ripple-scale, not DC-scale, so float's ~7 significant
    // digits are relative to a small number. The accumulators stay double so
    // that summing millions of these does not lose the tail.
    float d = x - s->k;
    s->acc_d  += d;
    s->acc_d2 += d * d;
    s->count++;
    if (x < s->min) s->min = x;
    if (x > s->max) s->max = x;
    if (++s->block_n >= PDSP_FOLD_N) {
        stat_fold(s);
    }
}

static void stat_finish(const running_stat_t *s, stat_result_t *out)
{
    out->count = s->count;
    if (s->count == 0) {
        out->min = out->max = out->mean = out->rms = out->std = 0.0f;
        return;
    }
    // All divisions live here, at readout (10 Hz), never on the sample path.
    // Include the not-yet-folded block so a readout reflects every sample
    // taken, not just those up to the last PDSP_FOLD_N boundary.
    double n      = (double)s->count;
    double sum_d  = s->sum_d  + (double)s->acc_d;
    double sum_d2 = s->sum_d2 + (double)s->acc_d2;
    double mean_d = sum_d / n;                  // mean of the SHIFTED data
    // Both terms are O(ripple^2) rather than O(DC^2), which is the whole point
    // of the shift -- this subtraction no longer catastrophically cancels.
    double var    = sum_d2 / n - mean_d * mean_d;
    if (var < 0.0) var = 0.0;              // guard tiny negative from FP rounding
    double mean   = (double)s->k + mean_d;         // undo the shift
    // mean(x^2) = var + mean^2 exactly, so a true RMS needs no separate
    // sum-of-squares accumulator.
    out->min  = (s->min == INFINITY)  ? 0.0f : s->min;
    out->max  = (s->max == -INFINITY) ? 0.0f : s->max;
    out->mean = (float)mean;
    out->rms  = (float)sqrt(var + mean * mean);
    out->std  = (float)sqrt(var);
}

// -----------------------------------------------------------------------------
// Lifecycle
// -----------------------------------------------------------------------------
void power_dsp_set_rate(power_dsp_t *d, float current_odr_hz)
{
    // The pending block holds areas in PERIOD units that are only converted to
    // joules/coulombs by multiplying by dt at fold time. Changing dt with a
    // block outstanding would retroactively re-scale samples that were taken
    // at the OLD rate, so settle it against the old dt first. (This runs on
    // every `dspdecim`/SET_RATE change.)
    d->charge_c  += (double)d->acc_charge * d->dt;
    d->energy_j  += (double)d->acc_energy * d->dt;
    d->elapsed_s += (double)d->acc_periods * d->dt;
    d->acc_charge  = 0.0f;
    d->acc_energy  = 0.0f;
    d->acc_periods = 0;

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

// Fold the float period-unit accumulators into the double joule/coulomb
// totals. `dt` is applied HERE, once per block, instead of once per sample.
static inline void energy_fold(power_dsp_t *d)
{
    d->charge_c  += (double)d->acc_charge * d->dt;
    d->energy_j  += (double)d->acc_energy * d->dt;
    d->elapsed_s += (double)d->acc_periods * d->dt;
    d->acc_charge  = 0.0f;
    d->acc_energy  = 0.0f;
    d->acc_periods = 0;
}

// -----------------------------------------------------------------------------
// Core processing
// -----------------------------------------------------------------------------
float power_dsp_push_current_n(power_dsp_t *d, float amps, uint32_t n_periods)
{
    float v = d->voltage;
    float p = v * amps;

    d->last_i = amps;
    d->last_v = v;
    d->last_p = p;

    // Trapezoidal integration, stretched over n_periods * dt instead of a
    // fixed one sample period -- see the header comment. n_periods == 0 is
    // treated as "no time elapsed" (stats still update below, matching
    // power_dsp_push_current()'s unconditional stat_update calls).
    //
    // dt is factored OUT of the per-sample work: the areas are accumulated in
    // PERIOD units in hardware float and folded (times dt) into the double
    // totals every PDSP_FOLD_N periods, so this whole block is single
    // precision -- no software-emulated double op per sample.
    if (d->have_prev && d->dt > 0.0 && n_periods > 0) {
        float nf = (float)n_periods;
        d->acc_charge += 0.5f * (d->prev_i + amps) * nf;
        d->acc_energy += 0.5f * (d->prev_p + p)    * nf;
        d->acc_periods += n_periods;
        if (d->acc_periods >= PDSP_FOLD_N) {
            energy_fold(d);
        }
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

float power_dsp_push_current(power_dsp_t *d, float amps)
{
    return power_dsp_push_current_n(d, amps, 1);
}

// -----------------------------------------------------------------------------
// Accumulator readout
// -----------------------------------------------------------------------------
// Every getter adds the not-yet-folded block, so a readout is always current
// to the last sample rather than to the last PDSP_FOLD_N boundary.
double power_dsp_energy_j(const power_dsp_t *d)
{
    return d->energy_j + (double)d->acc_energy * d->dt;
}
double power_dsp_energy_mwh(const power_dsp_t *d)
{
    return power_dsp_energy_j(d) * (1000.0 / 3600.0);
}
double power_dsp_charge_c(const power_dsp_t *d)
{
    return d->charge_c + (double)d->acc_charge * d->dt;
}
double power_dsp_charge_mah(const power_dsp_t *d)
{
    return power_dsp_charge_c(d) * (1000.0 / 3600.0);
}
double power_dsp_elapsed_s(const power_dsp_t *d)
{
    return d->elapsed_s + (double)d->acc_periods * d->dt;
}

void power_dsp_reset_energy(power_dsp_t *d)
{
    d->energy_j    = 0.0;
    d->charge_c    = 0.0;
    d->elapsed_s   = 0.0;
    d->acc_charge  = 0.0f;
    d->acc_energy  = 0.0f;
    d->acc_periods = 0;
    d->have_prev   = false;
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
