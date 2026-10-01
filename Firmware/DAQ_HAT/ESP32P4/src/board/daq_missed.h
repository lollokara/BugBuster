#pragma once

// =============================================================================
// daq_missed.h - conversions the ADC produced that the P4 never captured
// (STATUS extension v9 `missed_conversions`, DAQ-05/P4-8).
//
// DRDY runs at exactly the configured ODR, so expected = ODR x elapsed time.
// When the capture loop falls behind, the edge latch coalesces and those
// samples are lost without touching the ring, so ring overflow cannot see them.
// This integrates expected samples window by window (ODR may change between
// windows) and subtracts the captured count. Pure: no ESP-IDF dependency.
// =============================================================================

#include <stdint.h>

typedef struct {
    int64_t  last_us;
    uint32_t last_count;
    uint64_t expected_nsamp;   // expected samples x 1e9 (milli-SPS x us)
    uint64_t captured;
} daq_missed_t;

static inline void daq_missed_reset(daq_missed_t *m, int64_t now_us, uint32_t count)
{
    m->last_us = now_us;
    m->last_count = count;
    m->expected_nsamp = 0;
    m->captured = 0;
}

/** Advance to @p now_us with the capture counter at @p count (u32, may wrap)
 *  and the current ODR in milli-SPS. Returns cumulative missed conversions,
 *  clamped at 0 and saturated at UINT32_MAX. */
static inline uint32_t daq_missed_update(daq_missed_t *m, int64_t now_us, uint32_t count,
                                         uint32_t odr_mhz)
{
    int64_t dt = now_us - m->last_us;
    if (dt > 0) m->expected_nsamp += (uint64_t)odr_mhz * (uint64_t)dt;
    m->captured += (uint32_t)(count - m->last_count);   // modular: wrap-safe
    m->last_us = now_us;
    m->last_count = count;
    uint64_t expected = m->expected_nsamp / 1000000000ull;
    if (expected <= m->captured) return 0;
    uint64_t missed = expected - m->captured;
    return missed > 0xFFFFFFFFull ? 0xFFFFFFFFu : (uint32_t)missed;
}
