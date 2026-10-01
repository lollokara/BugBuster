// =============================================================================
// rail_power_hw.cpp - binds the pure rail sequencer (rail_power.cpp) to the
// PCA9535 / DS4424 / USB-PD drivers. Not built on the host.
// =============================================================================

#include "rail_power.h"

#include "config.h"        // delay_ms
#include "ds4424.h"
#include "pca9535.h"
#include "power/pd_manager.h"
#include "esp_log.h"

static const char *TAG = "rail_power";

// VADJ1/VADJ2 are buck rails fed from USB-C PD: raise the PD contract before
// raising the output, release headroom after lowering it (pd_manager.h).
bool rail_vadj_set_voltage(uint8_t rail, float volts)
{
    if (rail != 1 && rail != 2) return ds4424_set_voltage(rail, volts);
    PdConsumerId cid = (rail == 1) ? PD_CONSUMER_VADJ1 : PD_CONSUMER_VADJ2;
    bool going_up = volts >= pd_manager_consumer_v(cid);
    char warn[256] = {0};
    if (going_up) {
        pd_manager_ensure(cid, volts, PD_TYPE_BUCK, warn, sizeof(warn));
        if (warn[0]) ESP_LOGW(TAG, "%s", warn);
    }
    if (!ds4424_set_voltage(rail, volts)) return false;
    if (!going_up) {
        warn[0] = '\0';
        pd_manager_ensure(cid, volts, PD_TYPE_BUCK, warn, sizeof(warn));
        if (warn[0]) ESP_LOGW(TAG, "%s", warn);
    }
    return true;
}

static bool hw_efuse_arm(uint8_t logical, bool on) { return pca9535_user_arm_efuse(logical, on); }

static bool hw_vadj_enable(uint8_t rail, bool on)
{
    return pca9535_set_control(rail == 1 ? PCA_CTRL_VADJ1_EN : PCA_CTRL_VADJ2_EN, on);
}

static bool hw_vadj_set(uint8_t rail, float volts, float *applied_v)
{
    if (!rail_vadj_set_voltage(rail, volts)) return false;
    const DS4424State *ds = ds4424_get_state();
    *applied_v = ds ? ds->state[rail].target_v : volts;
    return true;
}

static void hw_delay(uint32_t ms) { delay_ms(ms); }

static bool hw_vadj_pg(uint8_t rail)
{
    pca9535_update();
    const PCA9535State *st = pca9535_get_state();
    return st && (rail == 1 ? st->vadj1_pg : st->vadj2_pg);
}

static bool hw_efuse_fault(uint8_t logical)
{
    const PCA9535State *st = pca9535_get_state();
    return st && logical < 4 && st->efuse_flt[logical];
}

static const RailPowerOps s_hw_ops = {
    hw_efuse_arm, hw_vadj_enable, hw_vadj_set, hw_delay, hw_vadj_pg, hw_efuse_fault,
};

const RailPowerOps *rail_power_ops_hw(void) { return &s_hw_ops; }
