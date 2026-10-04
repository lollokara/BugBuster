#pragma once

// =============================================================================
// battsim_s1.h - live 1 s history window for the S3 (BS_HOP_S1_SINCE).
//
// The battsim task keeps the last hour of 1 s records in a PSRAM ring (s1.bin
// is only written at pause/stop). This copies the records newer than a cursor
// out of that ring as compact samples. Pure C: host-tested by
// tests/firmware_host/test_battsim_s1.py.
// =============================================================================

#include <stdint.h>
#include <stdbool.h>
#include "battsim_store.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct __attribute__((packed)) {
    uint32_t t_s;        // end of the 1 s interval, simulated s since run start
    uint16_t v_mv;       // v_avg_mv of the record
    uint16_t soc_x100;   // 0..10000
    int32_t  i_ua;       // interval mean DUT current (dq_dut / dt)
    uint16_t flags;      // BS_RF_*
    uint16_t dt_s;       // seconds integrated in the interval
} bs_s1_sample_t;
_Static_assert(sizeof(bs_s1_sample_t) == 16, "bs_s1_sample_t wire size");

#define BS_S1_SINCE_MAX 14u   // 4 B header + 14 * 16 B = 228 B <= 240 B HAT reply

/**
 * Copy up to `max` records with t_s > since_t_s, oldest first. `head` is the
 * next write slot, `count` the filled slots, `cap` the ring size. i_ua comes
 * from the cumulative charge of the previous ring record, or (i_min+i_max)/2
 * for the oldest retained one. *more is set when newer records remain.
 * Returns the number copied (0 for an empty ring).
 */
int bs_s1_since(const bs_hist_rec_v2_t *ring, uint32_t head, uint32_t count, uint32_t cap,
                uint32_t since_t_s, bs_s1_sample_t *out, int max, bool *more);

#ifdef __cplusplus
}
#endif
