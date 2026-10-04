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

typedef enum {
    HUB_CLK_HUB = 0,
    HUB_CLK_SNTP = 1,
    HUB_CLK_P4_EPOCH = 2,
    HUB_CLK_EST = 3,
} hub_clk_src_t;

static inline const char *hub_clk_src_name(hub_clk_src_t src)
{
    switch (src) {
    case HUB_CLK_HUB:      return "HUB";
    case HUB_CLK_SNTP:     return "SNTP";
    case HUB_CLK_P4_EPOCH: return "P4_EPOCH";
    default:               return "EST";
    }
}

/** One battery-run sample as the hub wants it. ts = unix s at the END of the interval. */
typedef struct {
    uint32_t      ts;
    float         v, i, soc;                 /* V, A, percent */
    float         vmin, vmax, imin, imax;    /* valid only when ext (coarse tiers) */
    int8_t        state;                     /* 0..4 (hub state index) or -1 = unknown */
    bool          ext;
    hub_clk_src_t clk_src;
    uint32_t      clk_unc_ms;
} hub_sample_t;

/** One contiguous coverage range reported by GET /api/v1/runs/{uid}/coverage. */
typedef struct {
    double        from, to;                  /* unix s: first ts - res .. last ts */
    uint16_t      res;
    hub_clk_src_t clk_src;
    uint32_t      clk_unc_ms;
} hub_range_t;

#ifdef __cplusplus
}
#endif
