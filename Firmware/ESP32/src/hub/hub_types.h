#pragma once

// =============================================================================
// hub_types.h - plain data shared by the S3 hub streaming modules (spec
// 2026-10-03 section 6). No ESP-IDF includes: host-testable.
// =============================================================================

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** One battery-run sample as the hub wants it. ts = unix s at the END of the interval. */
typedef struct {
    uint32_t ts;
    float    v, i, soc;                 /* V, A, percent */
    float    vmin, vmax, imin, imax;    /* valid only when ext (coarse tiers) */
    int8_t   state;                     /* 0..4 (hub state index) or -1 = unknown */
    bool     ext;
} hub_sample_t;

/** One contiguous coverage range reported by GET /api/v1/runs/{uid}/coverage. */
typedef struct {
    double   from, to;                  /* unix s: first ts - res .. last ts */
    uint16_t res;
} hub_range_t;

#ifdef __cplusplus
}
#endif
