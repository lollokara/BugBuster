// =============================================================================
// rail_power.cpp - pure VADJ rail power-up sequencer. See rail_power.h.
// =============================================================================

#include "rail_power.h"

#include <math.h>

int rail_power_up(const RailPowerOps *ops, uint8_t rail, float volts,
                  uint16_t settle_ms, uint8_t flags, uint8_t efuse_mask,
                  RailPowerResult *out)
{
    if (!ops || !out) return 1;
    *out = RailPowerResult{};
    if (rail != 1 && rail != 2) return 1;
    if (isnan(volts) || volts < RAIL_PU_MIN_V || volts > RAIL_PU_MAX_V) return 1;
    if (volts > RAIL_PU_CONFIRM_ABOVE_V && !(flags & RAIL_PU_CONFIRM)) return 1;
    if (efuse_mask == 0) efuse_mask = 0x03;
    if (efuse_mask & ~0x03) return 1;

    const uint8_t first = (rail == 1) ? 0 : 2;   // logical e-fuse index

    // 1. E-fuses of the rail off: clears a latched trip and keeps the target
    //    unpowered while the rail is (re)programmed.
    for (uint8_t i = 0; i < 2; i++) {
        if ((efuse_mask & (1u << i)) && !ops->efuse_arm(first + i, false)) return 5;
    }

    // 2. Optional full power cycle so the target really resets.
    if (flags & RAIL_PU_POWER_CYCLE) {
        if (!ops->vadj_enable(rail, false)) return 5;
        ops->delay(RAIL_PU_DISCHARGE_MS);
    }

    // 3. Program the voltage BEFORE enabling, so the rail never comes up at a
    //    stale setpoint, then enable and let it settle.
    float applied = volts;
    if (!ops->vadj_set(rail, volts, &applied)) return 5;
    out->applied_v = applied;
    // DS4424 steps are ~0.1 V, so a few tens of mV is quantisation; only a
    // larger difference means the DAC limits clamped the request.
    out->clamped = fabsf(applied - volts) > RAIL_PU_CLAMP_TOL_V;
    if (!ops->vadj_enable(rail, true)) return 5;
    ops->delay(settle_ms);

    // 4. Arm the e-fuses (blackout gate), wait out the blackout so a short
    //    present at enable has tripped (PWR-02), then report.
    for (uint8_t i = 0; i < 2; i++) {
        if ((efuse_mask & (1u << i)) && !ops->efuse_arm(first + i, true)) return 5;
    }
    ops->delay(RAIL_PU_POST_ARM_MS);

    out->pg = ops->vadj_pg(rail);
    for (uint8_t i = 0; i < 2; i++) {
        out->fault[i] = (efuse_mask & (1u << i)) ? ops->efuse_fault(first + i) : false;
    }
    return 0;
}
