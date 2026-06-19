// =============================================================================
// current_fusion.c — seamless FINE+COARSE current reconstruction.
// =============================================================================

#include "current_fusion.h"
#include <string.h>
#include <math.h>

void current_fusion_set_timing(current_fusion_t *f, uint32_t settle_samples,
                               uint32_t blend_samples)
{
    f->settle_samples = settle_samples;
    f->blend_samples  = blend_samples;
}

void current_fusion_init(current_fusion_t *f, range_manager_t *rm,
                         uint32_t settle_samples, uint32_t blend_samples)
{
    memset(f, 0, sizeof(*f));
    f->rm         = rm;
    f->last_range = RANGE_UNKNOWN;
    current_fusion_set_timing(f, settle_samples, blend_samples);

    // Trust window per range: keep FINE only while it is comfortably inside its
    // shunt's linear span. Derived from the hardware over-current trip points.
    //   HI (51 ohm)  trips at 1.434 mA   -> trust up to ~1.4 mA
    //   MID (2 ohm)  trips at 36.56 mA   -> trust up to ~36 mA
    //   LO (50 mohm) full range          -> always COARSE
    f->fine_trust_max[RANGE_HI]  = 1.40e-3f;
    f->fine_trust_max[RANGE_MID] = 3.60e-2f;
    f->fine_trust_max[RANGE_LO]  = 0.0f;   // LO never uses FINE
}

// Convert a raw channel voltage to amps for a given range.
static float to_amps(current_fusion_t *f, current_range_t range, float v)
{
    return range_manager_volts_to_amps(f->rm, range, v);
}

void current_fusion_step(current_fusion_t *f, const fusion_input_t *in,
                         fusion_output_t *out)
{
    memset(out, 0, sizeof(*out));
    out->range = in->range;

    // Detect a range change -> start a settle blackout where we lean on COARSE.
    if (in->range != f->last_range) {
        if (f->last_range != RANGE_UNKNOWN) {
            f->settle_remaining = f->settle_samples;
            f->blend_remaining  = 0;   // (re)armed when FINE comes back
        }
        f->last_range = in->range;
    }

    // COARSE current (always valid on the 50 mohm shunt). Used as the fallback.
    float coarse_a = in->coarse_valid ? to_amps(f, RANGE_LO, in->coarse_v) : 0.0f;

    // LO range: COARSE is the measurement.
    if (range_uses_coarse(in->range) || in->range == RANGE_UNKNOWN) {
        out->amps      = coarse_a;
        out->source    = FUSE_SRC_COARSE;
        out->saturated = !in->coarse_valid;
        return;
    }

    // HI / MID range: FINE is the precision source when trusted.
    float fine_a = in->fine_valid ? to_amps(f, in->range, in->fine_v) : 0.0f;
    bool  fine_in_window = in->fine_valid &&
                           fabsf(fine_a) <= f->fine_trust_max[in->range];

    // During the post-switch settle blackout, use COARSE regardless of FINE.
    if (f->settle_remaining > 0) {
        f->settle_remaining--;
        out->amps   = coarse_a;
        out->source = FUSE_SRC_COARSE;
        // When the blackout ends, arm a cross-fade back to FINE.
        if (f->settle_remaining == 0 && fine_in_window) {
            f->blend_remaining = f->blend_samples;
            f->blend_from      = FUSE_SRC_COARSE;
        }
        return;
    }

    if (!fine_in_window) {
        // FINE saturated / out of window mid-range: fall back to COARSE.
        out->amps   = coarse_a;
        out->source = FUSE_SRC_COARSE;
        return;
    }

    // Cross-fade COARSE -> FINE after a settle, to avoid a step discontinuity.
    if (f->blend_remaining > 0 && f->blend_samples > 0) {
        float w = (float)f->blend_remaining / (float)f->blend_samples; // 1 -> 0
        f->blend_remaining--;
        out->amps   = w * coarse_a + (1.0f - w) * fine_a;
        out->source = FUSE_SRC_BLEND;
        return;
    }

    // Steady state: trust FINE.
    out->amps   = fine_a;
    out->source = FUSE_SRC_FINE;
}
