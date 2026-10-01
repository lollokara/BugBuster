#pragma once

// =============================================================================
// adgs_interlock.h - pure U17-S3 / U23 self-test mutual-exclusion rules.
//
// U17 switch S3 (IO9 analog path) and any closed U23 self-test switch share the
// AD74416H channel-D net, so they must never be closed together. Kept free of
// hardware state so the decisions are host-testable
// (tests/firmware_host/test_adgs_interlock.py). adgs2414d.cpp is the only
// writer and calls these before every write.
// =============================================================================

#include <stdbool.h>
#include <stdint.h>

// Whole main-device image write: refused if it closes U17 S3 while U23 is active.
static inline bool adgs_interlock_main_ok(uint8_t u17_new_state, uint8_t u17_s3_mask,
                                          uint8_t u23_state)
{
    return !((u17_new_state & u17_s3_mask) && u23_state != 0);
}

// Single switch write: only CLOSING U17 S3 can be refused.
static inline bool adgs_interlock_switch_ok(uint8_t device, uint8_t sw, bool closed,
                                            uint8_t u17_idx, uint8_t u17_s3_mask,
                                            uint8_t u23_state)
{
    if (!closed || device != u17_idx) return true;
    if ((uint8_t)(1u << sw) != u17_s3_mask) return true;
    return u23_state == 0;
}

// U23 self-test write: closing any U23 switch is refused while U17 S3 is closed.
static inline bool adgs_interlock_selftest_ok(uint8_t sw_byte, uint8_t u17_state,
                                              uint8_t u17_s3_mask)
{
    return sw_byte == 0 || (u17_state & u17_s3_mask) == 0;
}
