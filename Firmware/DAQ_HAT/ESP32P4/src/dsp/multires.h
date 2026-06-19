#pragma once

// =============================================================================
// multires.h — multi-resolution min/max/mean reduction tiers (Joulescope-style
// zoom) for the fused current / power stream.
//
// Each tier decimates the tier below by a fixed factor, keeping per-bucket
// min / max / mean. The PC can request any tier to pan/zoom over a long capture
// without transferring full-rate data: coarse tiers for overview, fine tiers
// (or raw) for detail.
//
//   tier 0 : every Nx samples              -> 1 reduced point
//   tier 1 : every Nx tier-0 points        -> Nx^2 raw samples / point
//   ...
//
// Reduced points are pushed to a per-tier callback (e.g. the USB framer) as they
// complete. The reducer is allocation-free and runs at the full sample rate.
// =============================================================================

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MULTIRES_MAX_TIERS   5

// One reduced bucket: min/max/mean over the samples it summarises.
typedef struct {
    float min;
    float max;
    float mean;
} multires_point_t;

// Called when a tier produces a completed reduced point.
//   tier  : tier index (0 = finest reduction)
//   pt    : the completed point
//   span  : number of raw samples this point summarises
typedef void (*multires_cb_t)(uint8_t tier, const multires_point_t *pt,
                              uint32_t span, void *user);

typedef struct {
    double   sum;
    float    min;
    float    max;
    uint32_t count;
    uint32_t target;     // samples (or lower-tier points) per bucket
    uint32_t raw_span;   // raw samples each completed point covers
} multires_tier_t;

typedef struct {
    uint8_t          tier_count;
    uint32_t         factor;                 // decimation per tier
    multires_tier_t  tiers[MULTIRES_MAX_TIERS];
    multires_cb_t    cb;
    void            *user;
} multires_t;

/**
 * @brief Initialise the reducer.
 * @param tier_count  number of decimation tiers (<= MULTIRES_MAX_TIERS).
 * @param factor      decimation factor between tiers (e.g. 100).
 * @param cb          completed-point callback (may be NULL).
 */
void multires_init(multires_t *m, uint8_t tier_count, uint32_t factor,
                   multires_cb_t cb, void *user);

/** @brief Feed one raw sample (fused current or power). */
void multires_push(multires_t *m, float x);

/** @brief Reset all tier accumulators. */
void multires_reset(multires_t *m);

#ifdef __cplusplus
}
#endif
