#pragma once

// =============================================================================
// rail_power.h - firmware-owned VADJ rail power-up sequence (PWR-REFAC/PWR-11).
//
// One sequence for every caller (BBP RAIL_POWER_UP, HTTP /api/power/rail_up,
// and through them the MCP target tools and the Python HAL):
//
//   e-fuses off -> [VADJ off + discharge] -> set V -> VADJ on -> settle ->
//   e-fuses armed through the blackout gate -> wait blackout -> PG / FLT read
//
// Pure: hardware access goes through RailPowerOps so the order is host-tested
// (tests/firmware_host/test_rail_power_seq.py). rail_power_ops_hw() in
// rail_power_hw.cpp binds the real PCA9535 / DS4424 / PD drivers.
// =============================================================================

#include <stdbool.h>
#include <stdint.h>

#define RAIL_PU_CONFIRM       0x01  // required above RAIL_PU_CONFIRM_ABOVE_V
#define RAIL_PU_POWER_CYCLE   0x02  // switch VADJ off and discharge first

#define RAIL_PU_MIN_V            3.0f
#define RAIL_PU_MAX_V           15.0f
#define RAIL_PU_CONFIRM_ABOVE_V 12.0f
#define RAIL_PU_DISCHARGE_MS     200u
#define RAIL_PU_POST_ARM_MS      120u  // e-fuse blackout (100 ms) + margin

typedef struct {
    bool (*efuse_arm)(uint8_t logical, bool on);           // via the blackout gate
    bool (*vadj_enable)(uint8_t rail, bool on);
    bool (*vadj_set)(uint8_t rail, float volts, float *applied_v);
    void (*delay)(uint32_t ms);
    bool (*vadj_pg)(uint8_t rail);
    bool (*efuse_fault)(uint8_t logical);
} RailPowerOps;

typedef struct {
    float applied_v;   // what the DAC was actually set to (after clamping)
    bool  clamped;     // applied_v differs from the request
    bool  pg;          // VADJ power-good after the sequence
    bool  fault[2];    // FLT of the rail's first / second e-fuse
} RailPowerResult;

// rail 1 = VADJ1 (EFUSE1+2), 2 = VADJ2 (EFUSE3+4). efuse_mask bit0/bit1 select
// the rail's first/second e-fuse; 0 = both. Returns 0 or a CmdError code
// (1 = bad argument, 5 = hardware) - nothing is touched on a bad argument.
int rail_power_up(const RailPowerOps *ops, uint8_t rail, float volts,
                  uint16_t settle_ms, uint8_t flags, uint8_t efuse_mask,
                  RailPowerResult *out);

// Real-hardware ops (rail_power_hw.cpp, not built on the host).
const RailPowerOps *rail_power_ops_hw(void);

// Set VADJ1/VADJ2 with the USB-PD contract raised first / released after.
bool rail_vadj_set_voltage(uint8_t rail, float volts);

#define RAIL_PU_MAX_SETTLE_MS 5000u
