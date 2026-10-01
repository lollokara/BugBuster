// =============================================================================
// efuse_imon.cpp - E-fuse current monitor. See efuse_imon.h.
// =============================================================================

#include "efuse_imon.h"
#include "config.h"

#if ADGS_HAS_SELFTEST

#include "adgs2414d.h"
#include "pca9535.h"
#include "selftest.h"
#include "tasks.h"
#include "io_owner.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "efuse_imon";

static constexpr uint8_t  IMON_LOGICAL_CH  = 2;    // logical C = AD74416H physical D
static constexpr uint8_t  IMON_PHYSICAL_CH = 3;
static constexpr uint8_t  IMON_CH_SLOT     = 14;   // io_owner slot for logical C
static constexpr uint8_t  IMON_SESSION     = 0xFE; // distinct from selftest's 0xFF
static constexpr uint32_t EFUSE_OFF_SETTLE_MS = 200;
static constexpr uint32_t CHD_DISCHARGE_MS    = 50;   // ~5 tau of Ch D cap into R106
static constexpr uint32_t ADC_SETTLE_MS       = 300;
static constexpr uint32_t LEASE_RENEW_MS      = 60000;
static constexpr float    SATURATION_V        = 2.45f;

// IMON current source into R_IOCP (11k) in parallel with R106 (1M) on the U23 rail.
static constexpr float R_EFF_OHM = (IMON_R_IOCP_OHM * 1.0e6f) / (IMON_R_IOCP_OHM + 1.0e6f);
static constexpr float MA_PER_V  = 1000.0f / (IMON_GAIN_UA_PER_A * 1.0e-6f * R_EFF_OHM);

// Logical EFUSE1..4 -> U23 source switch. Logical 3/4 are physical pairs 4/3
// (see pca9535.cpp set_efuse_bit_internal), and EFUSE_MON_n follows the pair.
static const uint8_t IMON_SW[4] = {
    U23_SW_EFUSE1_IMON, U23_SW_EFUSE2_IMON, U23_SW_EFUSE4_IMON, U23_SW_EFUSE3_IMON,
};

static SemaphoreHandle_t s_lock;
static volatile uint8_t  s_efuse;        // 0 = none
static uint32_t          s_attached_ms;
static uint32_t          s_lease_ms;

static uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

static bool efuse_is_on(uint8_t efuse)
{
    const PCA9535State *pca = pca9535_get_state();
    return pca && pca->present && pca->efuse_en[efuse - 1];
}

static void set_chd_function(ChannelFunction f, AdcRange range)
{
    AD74416H *dev = tasks_get_device();
    if (!dev) return;
    dev->startAdcConversion(false, 0, 0);
    delay_ms(5);
    dev->setChannelFunction(IMON_PHYSICAL_CH, CH_FUNC_HIGH_IMP);
    if (f != CH_FUNC_HIGH_IMP) {
        delay_ms(50);
        dev->setChannelFunction(IMON_PHYSICAL_CH, f);
        delay_ms(50);
        dev->configureAdc(IMON_PHYSICAL_CH, ADC_MUX_LF_TO_AGND, range, ADC_RATE_20SPS);
    }
    dev->clearChannelAlert(IMON_PHYSICAL_CH);
    if (xSemaphoreTake(g_stateMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        ChannelState &cs = g_deviceState.channels[IMON_LOGICAL_CH];
        cs.function = f;
        cs.adcMux   = ADC_MUX_LF_TO_AGND;
        cs.adcRange = range;
        cs.adcRate  = ADC_RATE_20SPS;
        cs.adcValue = 0.0f;
        cs.adcRawCode = 0;
        xSemaphoreGive(g_stateMutex);
    }
    tasks_rebuild_adc_conv_ctrl();
}

static bool claim_slot(void)
{
    io_owner_slot_t cur = io_owner_get(IMON_CH_SLOT);
    if (cur.kind != IO_OWNER_NONE && cur.kind != IO_OWNER_INTERNAL) return false;
    io_owner_force_release(IMON_CH_SLOT);
    uint32_t t = now_ms();
    s_lease_ms = t;
    return io_owner_acquire(IMON_CH_SLOT, IO_OWNER_INTERNAL, IMON_SESSION, 0,
                            IO_OWNER_INTERNAL_MAX_LEASE_MS, t);
}

static void detach_locked(void)
{
    adgs_selftest_set_locked(false);
    set_chd_function(CH_FUNC_HIGH_IMP, ADC_RNG_0_12V);
    adgs_set_selftest(0x00);
    io_owner_force_release(IMON_CH_SLOT);
    s_efuse = EFUSE_IMON_NONE;
}

static efuse_imon_result_t attach_locked(uint8_t efuse)
{
    if (!claim_slot()) return EFUSE_IMON_SLOT_HELD;
    // Bleed the Ch D input cap before it meets the IMON node.
    if (!adgs_set_selftest(U23_SW_ADC_CH_D)) {
        io_owner_force_release(IMON_CH_SLOT);
        return EFUSE_IMON_HW_FAIL;
    }
    delay_ms(CHD_DISCHARGE_MS);
    if (!adgs_set_selftest(U23_SW_ADC_CH_D | IMON_SW[efuse - 1])) {
        adgs_set_selftest(0x00);
        io_owner_force_release(IMON_CH_SLOT);
        return EFUSE_IMON_HW_FAIL;
    }
    set_chd_function(CH_FUNC_VIN, ADC_RNG_NEG2_5_2_5V);
    adgs_selftest_set_locked(true);
    s_efuse = efuse;
    s_attached_ms = now_ms();
    return EFUSE_IMON_OK;
}

static const char *result_str(efuse_imon_result_t rc)
{
    switch (rc) {
        case EFUSE_IMON_OK:            return "ok";
        case EFUSE_IMON_NEEDS_CONFIRM: return "power-cycle not confirmed";
        case EFUSE_IMON_BUSY:          return "self-test, calibration or IO9 analog path active";
        case EFUSE_IMON_INVALID:       return "invalid e-fuse";
        case EFUSE_IMON_SLOT_HELD:     return "channel C owned by a client";
        case EFUSE_IMON_HW_FAIL:       return "U23 route rejected";
        default:                       return "unsupported";
    }
}

efuse_imon_result_t efuse_imon_select(uint8_t efuse, bool confirm_cycle)
{
    if (efuse > 4) return EFUSE_IMON_INVALID;
    if (!s_lock) s_lock = xSemaphoreCreateMutex();
    if (!s_lock || xSemaphoreTake(s_lock, pdMS_TO_TICKS(3000)) != pdTRUE) return EFUSE_IMON_BUSY;
    // Waits out any in-flight self-test measurement so the two never interleave on U23.
    if (!selftest_u23_lock(3000)) {
        xSemaphoreGive(s_lock);
        ESP_LOGW(TAG, "Request for EFUSE%u rejected: U23 busy with a self-test measurement", efuse);
        return EFUSE_IMON_BUSY;
    }

    efuse_imon_result_t rc = EFUSE_IMON_OK;
    uint8_t old = s_efuse;
    bool old_on = false, new_on = false;
    if (efuse == old) goto out;

    if (efuse != EFUSE_IMON_NONE && old == EFUSE_IMON_NONE) {
        if (selftest_is_busy() || adgs_u17_s3_active()) { rc = EFUSE_IMON_BUSY; goto out; }
        io_owner_slot_t cur = io_owner_get(IMON_CH_SLOT);
        if (cur.kind != IO_OWNER_NONE && cur.kind != IO_OWNER_INTERNAL) { rc = EFUSE_IMON_SLOT_HELD; goto out; }
    }

    old_on = old && efuse_is_on(old);
    new_on = efuse && efuse_is_on(efuse);
    if ((old_on || new_on) && !confirm_cycle) { rc = EFUSE_IMON_NEEDS_CONFIRM; goto out; }

    if (old_on) pca9535_user_arm_efuse(old - 1, false);
    if (new_on) pca9535_user_arm_efuse(efuse - 1, false);
    if (old_on || new_on) delay_ms(EFUSE_OFF_SETTLE_MS);

    if (old) detach_locked();
    if (efuse) rc = attach_locked(efuse);

    if (old_on) pca9535_user_arm_efuse(old - 1, true);
    if (new_on) pca9535_user_arm_efuse(efuse - 1, true);

out:
    selftest_u23_unlock();
    xSemaphoreGive(s_lock);

    if (efuse == old) {
        // No-op request; nothing to report.
    } else if (rc == EFUSE_IMON_NEEDS_CONFIRM) {
        ESP_LOGI(TAG, "EFUSE%u is ON: awaiting host confirmation to power-cycle",
                 new_on ? efuse : old);
    } else if (rc != EFUSE_IMON_OK) {
        if (efuse) ESP_LOGW(TAG, "Attach to EFUSE%u refused: %s", efuse, result_str(rc));
        else       ESP_LOGW(TAG, "Detach from EFUSE%u refused: %s", old, result_str(rc));
    } else if (!old) {
        ESP_LOGI(TAG, "Current monitor attached to EFUSE%u (%s)", efuse,
                 new_on ? "output power-cycled" : "output off, no power cycle");
    } else if (!efuse) {
        ESP_LOGI(TAG, "Current monitor detached from EFUSE%u (%s)", old,
                 old_on ? "output power-cycled" : "output off, no power cycle");
    } else {
        ESP_LOGI(TAG, "Current monitor moved EFUSE%u -> EFUSE%u (power-cycled:%s%s%s)",
                 old, efuse, old_on ? " old" : "", new_on ? " new" : "",
                 (old_on || new_on) ? "" : " none");
    }
    return rc;
}

void efuse_imon_get(efuse_imon_status_t *out)
{
    if (!out) return;
    *out = {};
    uint8_t efuse = s_efuse;
    out->efuse = efuse;
    if (!efuse) return;
    out->efuse_on = efuse_is_on(efuse);

    ChannelFunction f = CH_FUNC_HIGH_IMP;
    float v = 0.0f;
    uint32_t raw = 0;
    if (xSemaphoreTake(g_stateMutex, pdMS_TO_TICKS(20)) == pdTRUE) {
        const ChannelState &cs = g_deviceState.channels[IMON_LOGICAL_CH];
        f = cs.function;
        v = cs.adcValue;
        raw = cs.adcRawCode;
        xSemaphoreGive(g_stateMutex);
    }
    // An AD74416H reset drops Ch D back to HIGH_IMP: report invalid, not 0 mA.
    // raw stays 0 until the poll task lands a conversion on the new config.
    out->valid = (f == CH_FUNC_VIN) && raw != 0 &&
                 (now_ms() - s_attached_ms) >= ADC_SETTLE_MS;
    out->imon_v = v;
    out->current_ma = v * MA_PER_V;
    out->saturated = v >= SATURATION_V || raw >= 0xFF0000u;
}

bool efuse_imon_active(void)
{
    return s_efuse != EFUSE_IMON_NONE;
}

void efuse_imon_tick(uint32_t t)
{
    if (!s_efuse || (t - s_lease_ms) < LEASE_RENEW_MS) return;
    s_lease_ms = t;
    io_owner_acquire(IMON_CH_SLOT, IO_OWNER_INTERNAL, IMON_SESSION, 0,
                     IO_OWNER_INTERNAL_MAX_LEASE_MS, t);
}

#else  // !ADGS_HAS_SELFTEST

efuse_imon_result_t efuse_imon_select(uint8_t, bool) { return EFUSE_IMON_UNSUPPORTED; }
void efuse_imon_get(efuse_imon_status_t *out) { if (out) *out = {}; }
bool efuse_imon_active(void) { return false; }
void efuse_imon_tick(uint32_t) {}

#endif
