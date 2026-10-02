#pragma once

// =============================================================================
// adgs_interlock.h - pure self-test mutual-exclusion rules.
//
// Any closed U23 self-test switch drives the AD74416H PHYSICAL channel D net
// (U23 S4 = U23_SW_ADC_CH_D). A main-MUX analog switch S3 that connects the
// physical-D channel to a terminal must never be closed at the same time.
// `d_net_dev_mask` lists the main MUX devices whose S3 can do that
// (config.h ADGS_D_NET_DEV_MASK). Kept free of hardware state so the decisions
// are host-testable (tests/firmware_host/test_adgs_interlock.py);
// adgs2414d.cpp is the only writer and calls these before every write.
// =============================================================================

#include <stdbool.h>
#include <stdint.h>

static inline bool adgs_dnet_s3_closed(const uint8_t *main_states, uint8_t n_main,
                                       uint8_t d_net_dev_mask, uint8_t s3_mask)
{
    for (uint8_t d = 0; d < n_main; d++) {
        if ((d_net_dev_mask & (1u << d)) && (main_states[d] & s3_mask)) return true;
    }
    return false;
}

// Whole main-device image write: refused if it closes a D-net S3 while U23 is active.
static inline bool adgs_interlock_main_ok(const uint8_t *new_states, uint8_t n_main,
                                          uint8_t d_net_dev_mask, uint8_t s3_mask,
                                          uint8_t u23_state)
{
    return u23_state == 0 || !adgs_dnet_s3_closed(new_states, n_main, d_net_dev_mask, s3_mask);
}

// Single switch write: only CLOSING a D-net S3 can be refused.
static inline bool adgs_interlock_switch_ok(uint8_t device, uint8_t sw, bool closed,
                                            uint8_t d_net_dev_mask, uint8_t s3_mask,
                                            uint8_t u23_state)
{
    if (!closed || !(d_net_dev_mask & (1u << device))) return true;
    if ((uint8_t)(1u << sw) != s3_mask) return true;
    return u23_state == 0;
}

// U23 self-test write: closing any U23 switch is refused while a D-net S3 is closed.
static inline bool adgs_interlock_selftest_ok(uint8_t sw_byte, const uint8_t *main_states,
                                              uint8_t n_main, uint8_t d_net_dev_mask,
                                              uint8_t s3_mask)
{
    return sw_byte == 0 || !adgs_dnet_s3_closed(main_states, n_main, d_net_dev_mask, s3_mask);
}
