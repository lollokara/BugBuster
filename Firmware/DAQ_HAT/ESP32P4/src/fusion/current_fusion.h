#pragma once

// =============================================================================
// current_fusion.h — seamless nA..3A current reconstruction from the FINE and
// COARSE ADAQ channels plus the range tag.
//
// The two current ADCs are SYNC-aligned (shared MCLK), so a FINE sample and a
// COARSE sample taken in the same conversion period describe the same instant.
// This module fuses them into one calibrated current i[n] with no holes when
// the analog autorange loop switches ranges:
//
//   * Normal operation: use FINE while its range (HI/MID) is settled and the
//     reading is inside the trusted window; use COARSE in LO.
//   * Range transition: the analog loop flips a bypass switch and the FINE
//     filter must re-settle. During that blackout, COARSE (always valid on the
//     50 mohm shunt) carries the signal so the fused stream never drops out.
//   * Cross-fade: across the settle window the output is blended FINE<->COARSE
//     to avoid a step discontinuity.
//   * Hysteresis: range-boundary thresholds use separate up/down trip points so
//     a signal sitting on a boundary does not chatter between sources.
//
// The fusion stage is fed per-sample readings; it does not own the ADCs or the
// range GPIOs. It consults the range_manager for the active range and shunt.
// =============================================================================

#include <stdint.h>
#include <stdbool.h>
#include "range_manager.h"

#ifdef __cplusplus
extern "C" {
#endif

// Which source produced the fused value (for diagnostics / stream tagging).
typedef enum {
    FUSE_SRC_FINE = 0,
    FUSE_SRC_COARSE,
    FUSE_SRC_BLEND,
} fuse_source_t;

// One synchronized input set for a single conversion period.
typedef struct {
    float           fine_v;       // FINE ADAQ reading (volts), or NaN if absent
    float           coarse_v;     // COARSE ADAQ reading (volts)
    bool            fine_valid;   // FINE sample present & not saturated/unsettled
    bool            coarse_valid; // COARSE sample present & not saturated
    current_range_t range;        // range observed for this period
} fusion_input_t;

// One fused output sample.
typedef struct {
    float           amps;         // calibrated, seamless current
    current_range_t range;        // range used
    fuse_source_t   source;       // FINE / COARSE / BLEND
    bool            saturated;    // COARSE clipped with no FINE fallback (over-range)
} fusion_output_t;

typedef struct {
    range_manager_t *rm;          // calibration + shunt source

    // Transition handling.
    current_range_t  last_range;
    uint32_t         settle_samples;   // FINE blackout length after a switch
    uint32_t         settle_remaining; // counts down during a transition
    uint32_t         blend_samples;    // cross-fade length
    uint32_t         blend_remaining;  // counts down during a cross-fade
    fuse_source_t    blend_from;       // source being faded out

    // Over-range thresholds (amps) with hysteresis. Informational: the analog
    // loop owns the actual switching; these classify confidence in FINE.
    float            fine_trust_max[RANGE_COUNT];  // above -> distrust FINE
} current_fusion_t;

/**
 * @brief Initialise the fusion engine.
 * @param rm              range manager (provides per-range cal + shunt).
 * @param settle_samples  number of fused samples to lean on COARSE after a range
 *                        change (set from the ADAQ filter settle time at the ODR).
 * @param blend_samples   cross-fade length once FINE is trusted again.
 */
void current_fusion_init(current_fusion_t *f, range_manager_t *rm,
                         uint32_t settle_samples, uint32_t blend_samples);

/**
 * @brief Fuse one synchronized FINE+COARSE pair into a single current sample.
 */
void current_fusion_step(current_fusion_t *f, const fusion_input_t *in,
                         fusion_output_t *out);

/**
 * @brief Recompute settle/blend sample counts for a new ODR (call when the
 *        sample rate changes).
 */
void current_fusion_set_timing(current_fusion_t *f, uint32_t settle_samples,
                               uint32_t blend_samples);

/**
 * @brief Update the FINE trust windows from calibrated SET threshold currents.
 *        Call after range calibration to apply per-board thresholds without
 *        rebooting.  Clamps to sane minima to prevent runaway.
 * @param i_trust_hi   new fine_trust_max[RANGE_HI] (A)
 * @param i_trust_mid  new fine_trust_max[RANGE_MID] (A)
 */
void current_fusion_set_trust(current_fusion_t *f,
                              float i_trust_hi, float i_trust_mid);

#ifdef __cplusplus
}
#endif
