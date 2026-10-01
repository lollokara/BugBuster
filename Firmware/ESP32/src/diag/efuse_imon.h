#pragma once

// =============================================================================
// efuse_imon.h - E-fuse current monitor (TPS1641x IMON via U23 -> AD74416H D)
//
// Exactly one e-fuse at a time. Attaching or detaching the U23 path disturbs
// the IMON node (it also sets the current limit), so every mux change happens
// with the e-fuse OFF: off -> 200 ms -> mux -> restore. While a monitor is
// active U23 is locked and every other U23 user is refused.
// =============================================================================

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define EFUSE_IMON_NONE  0   // efuse argument: 0 = off, 1..4 = logical EFUSE1..4

typedef enum {
    EFUSE_IMON_OK             = 0,
    EFUSE_IMON_NEEDS_CONFIRM  = 1,  // an affected e-fuse is ON; retry with confirm
    EFUSE_IMON_BUSY           = 2,  // self-test/cal running, or IO9 analog path closed
    EFUSE_IMON_INVALID        = 3,  // bad e-fuse index
    EFUSE_IMON_SLOT_HELD      = 4,  // logical channel C held by a client
    EFUSE_IMON_HW_FAIL        = 5,  // mux / expander write failed
    EFUSE_IMON_UNSUPPORTED    = 6,  // build without the U23 self-test mux
} efuse_imon_result_t;

typedef struct {
    uint8_t efuse;        // 0 = none, 1..4
    bool    valid;        // reading is live (channel D configured and settled)
    bool    saturated;    // IMON at/above the +/-2.5 V range ceiling
    bool    efuse_on;     // monitored e-fuse output enabled
    float   imon_v;       // voltage on the IMON pin
    float   current_ma;   // derived e-fuse output current
} efuse_imon_status_t;

/** Select the monitored e-fuse (0 = stop). confirm_cycle allows power-cycling
 *  an e-fuse that is currently ON. Blocks for ~0.5 s when the mux moves. */
efuse_imon_result_t efuse_imon_select(uint8_t efuse, bool confirm_cycle);

void efuse_imon_get(efuse_imon_status_t *out);

bool efuse_imon_active(void);

/** Keep the INTERNAL claim on the channel slot alive. Call from the main loop. */
void efuse_imon_tick(uint32_t now_ms);

#ifdef __cplusplus
}
#endif
