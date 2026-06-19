// =============================================================================
// multires.c — multi-resolution min/max/mean reduction tiers.
// =============================================================================

#include "multires.h"
#include <string.h>
#include <math.h>

static void tier_reset(multires_tier_t *t, uint32_t target, uint32_t raw_span)
{
    t->sum      = 0.0;
    t->min      = INFINITY;
    t->max      = -INFINITY;
    t->count    = 0;
    t->target   = target;
    t->raw_span = raw_span;
}

void multires_init(multires_t *m, uint8_t tier_count, uint32_t factor,
                   multires_cb_t cb, void *user)
{
    memset(m, 0, sizeof(*m));
    if (tier_count > MULTIRES_MAX_TIERS) tier_count = MULTIRES_MAX_TIERS;
    if (factor < 2) factor = 2;
    m->tier_count = tier_count;
    m->factor     = factor;
    m->cb         = cb;
    m->user       = user;

    uint32_t raw_span = factor;     // tier 0 covers `factor` raw samples
    for (uint8_t i = 0; i < tier_count; ++i) {
        tier_reset(&m->tiers[i], factor, raw_span);
        raw_span *= factor;
    }
}

void multires_reset(multires_t *m)
{
    uint32_t raw_span = m->factor;
    for (uint8_t i = 0; i < m->tier_count; ++i) {
        tier_reset(&m->tiers[i], m->factor, raw_span);
        raw_span *= m->factor;
    }
}

// Accumulate a value into a tier; when the bucket fills, emit a point and feed
// the tier's MEAN up to the next tier.
static void tier_accumulate(multires_t *m, uint8_t idx, float x)
{
    if (idx >= m->tier_count) return;
    multires_tier_t *t = &m->tiers[idx];

    t->sum += (double)x;
    if (x < t->min) t->min = x;
    if (x > t->max) t->max = x;
    t->count++;

    if (t->count >= t->target) {
        multires_point_t pt = {
            .min  = (t->min == INFINITY)  ? 0.0f : t->min,
            .max  = (t->max == -INFINITY) ? 0.0f : t->max,
            .mean = (float)(t->sum / (double)t->count),
        };
        if (m->cb) {
            m->cb(idx, &pt, t->raw_span, m->user);
        }
        // Propagate this tier's mean to the next coarser tier.
        tier_accumulate(m, (uint8_t)(idx + 1), pt.mean);
        // Restart this bucket.
        t->sum   = 0.0;
        t->min   = INFINITY;
        t->max   = -INFINITY;
        t->count = 0;
    }
}

void multires_push(multires_t *m, float x)
{
    tier_accumulate(m, 0, x);
}
