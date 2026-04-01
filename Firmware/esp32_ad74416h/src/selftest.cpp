// =============================================================================
// selftest.cpp - Self-Test, Calibration, and E-fuse Current Monitoring
//
// Uses U23 (5th ADGS2414D) to route VADJ / IMON / 3V3_ADJ to Channel D.
// See selftest.h for the public API and config.h for switch definitions.
// =============================================================================

#include "selftest.h"
#include "adgs2414d.h"
#include "config.h"
#include "tasks.h"
#include "ds4424.h"
#include "pca9535.h"
#include "esp_log.h"
#include "esp_timer.h"

#include <string.h>
#include <math.h>

static const char *TAG = "selftest";

#if ADGS_HAS_SELFTEST

// -----------------------------------------------------------------------------
// Internal state
// -----------------------------------------------------------------------------

static SelftestBootResult    s_boot_result   = {};
static SelftestEfuseCurrents s_efuse_curr   = {};
static SelftestSupplyVoltages s_supply_volt  = {};
static SelftestCalResult     s_cal_result   = {};
static bool                  s_initialized  = false;

// Non-blocking monitor state machine
// Cycles: EFUSE1 → EFUSE2 → EFUSE3 → EFUSE4 → VADJ1 → VADJ2 → 3V3_ADJ → repeat
static uint8_t s_monitor_idx = 0;  // 0-3 = efuse, 4-6 = supply rail
#define MONITOR_TOTAL_CHANNELS  7  // 4 efuses + 3 supply rails

// U23 switch masks for each e-fuse IMON pin
static const uint8_t EFUSE_IMON_SW[5] = {
    0,                      // index 0 unused (e-fuses are 1-based)
    U23_SW_EFUSE1_IMON,     // efuse 1 → S2 (bit 1)
    U23_SW_EFUSE2_IMON,     // efuse 2 → S3 (bit 2)
    U23_SW_EFUSE3_IMON,     // efuse 3 → S1 (bit 0)
    U23_SW_EFUSE4_IMON,     // efuse 4 → S5 (bit 4)
};

// U23 switch masks for each supply rail
static const uint8_t RAIL_SW[SELFTEST_RAIL_COUNT] = {
    U23_SW_VADJ1,           // VADJ1 → S6 (bit 5)
    U23_SW_VADJ2,           // VADJ2 → S7 (bit 6)
    U23_SW_3V3_ADJ,         // 3V3_ADJ → S8 (bit 7)
};

// Whether each rail has a voltage divider and its correction factor
static const float RAIL_CORRECTION[SELFTEST_RAIL_COUNT] = {
    1.0f / VADJ_DIVIDER_RATIO,  // VADJ1: measured × 1/0.7418 = actual
    1.0f / VADJ_DIVIDER_RATIO,  // VADJ2: same divider
    1.0f,                        // 3V3_ADJ: direct, no divider
};

// -----------------------------------------------------------------------------
// Internal helpers
// -----------------------------------------------------------------------------

// Configure AD74416H Channel D as voltage input and read it.
// Returns the ADC reading in volts, or -1 on error.
// Caller must have set U23 switches BEFORE calling this.
static float read_channel_d(uint8_t adc_range)
{
    // We directly access the device through the tasks module extern.
    // Channel D = channel index 3.
    // The ADC config is set, we wait for a fresh conversion, then read.

    AD74416H *dev = tasks_get_device();
    if (!dev) return -1.0f;

    // Configure Ch D as VIN with the requested range
    dev->setChannelFunction(3, CH_FUNC_VIN);
    dev->setAdcConfig(3, adc_range, 0x06 /* 200 SPS */, 0x00 /* LF_TO_AGND */);

    // Wait for ADC to produce a valid reading (need at least 2 conversion cycles)
    delay_ms(50);

    // Read the ADC value
    uint32_t raw = 0;
    dev->readAdcResult(3, &raw);

    // Convert raw to voltage based on range
    float voltage = 0.0f;
    switch (adc_range) {
        case 0: // V_0_12
            voltage = (float)raw / 16777216.0f * 12.0f;
            break;
        case 5: // V_0_625MV
            voltage = (float)raw / 16777216.0f * 0.625f;
            break;
        default:
            voltage = (float)raw / 16777216.0f * 12.0f;
            break;
    }

    // Return Ch D to HIGH_IMP
    dev->setChannelFunction(3, CH_FUNC_HIGH_IMP);

    return voltage;
}

// Perform a single measurement through U23.
// Opens all U23 switches, sets the requested switches, reads ADC, cleans up.
// Returns voltage in volts, or -1 on error.
static float measure_via_u23(uint8_t source_sw, uint8_t adc_range)
{
    // The measurement path: close S4 (Ch D → shared rail) + source switch
    uint8_t sw_byte = U23_SW_ADC_CH_D | source_sw;

    if (!adgs_set_selftest(sw_byte)) {
        ESP_LOGE(TAG, "U23 interlock prevented measurement (U17 S2 active?)");
        return -1.0f;
    }

    delay_ms(10);  // allow MUX and signal to settle

    float v = read_channel_d(adc_range);

    // Always clean up — open all U23 switches
    adgs_set_selftest(0x00);

    return v;
}

// -----------------------------------------------------------------------------
// Public API
// -----------------------------------------------------------------------------

void selftest_init(void)
{
    memset(&s_boot_result, 0, sizeof(s_boot_result));
    memset(&s_efuse_curr, 0, sizeof(s_efuse_curr));
    memset(&s_supply_volt, 0, sizeof(s_supply_volt));
    memset(&s_cal_result, 0, sizeof(s_cal_result));

    for (int i = 0; i < SELFTEST_EFUSE_COUNT; i++)
        s_efuse_curr.current_a[i] = -1.0f;
    for (int i = 0; i < SELFTEST_RAIL_COUNT; i++)
        s_supply_volt.voltage[i] = -1.0f;
    s_efuse_curr.available = false;
    s_supply_volt.available = false;
    s_cal_result.status = CAL_STATUS_IDLE;
    s_initialized = true;

    ESP_LOGI(TAG, "Self-test module initialized (U23 at device index %d)", ADGS_SELFTEST_DEV);
}

const SelftestBootResult* selftest_boot_check(void)
{
    ESP_LOGI(TAG, "Running boot self-test...");
    s_boot_result.ran = true;
    s_boot_result.passed = true;

    // Measure VADJ1
    s_boot_result.vadj1_v = selftest_measure_supply(SELFTEST_RAIL_VADJ1);
    if (s_boot_result.vadj1_v < 0) {
        ESP_LOGW(TAG, "  VADJ1: measurement failed");
        s_boot_result.passed = false;
    } else {
        ESP_LOGI(TAG, "  VADJ1: %.3f V", s_boot_result.vadj1_v);
    }

    // Measure VADJ2
    s_boot_result.vadj2_v = selftest_measure_supply(SELFTEST_RAIL_VADJ2);
    if (s_boot_result.vadj2_v < 0) {
        ESP_LOGW(TAG, "  VADJ2: measurement failed");
        s_boot_result.passed = false;
    } else {
        ESP_LOGI(TAG, "  VADJ2: %.3f V", s_boot_result.vadj2_v);
    }

    // Measure 3V3_ADJ (VLOGIC)
    s_boot_result.vlogic_v = selftest_measure_supply(SELFTEST_RAIL_3V3_ADJ);
    if (s_boot_result.vlogic_v < 0) {
        ESP_LOGW(TAG, "  VLOGIC: measurement failed");
        s_boot_result.passed = false;
    } else {
        ESP_LOGI(TAG, "  VLOGIC: %.3f V", s_boot_result.vlogic_v);
    }

    ESP_LOGI(TAG, "Boot self-test %s", s_boot_result.passed ? "PASSED" : "FAILED");
    return &s_boot_result;
}

float selftest_measure_supply(uint8_t rail)
{
    if (rail >= SELFTEST_RAIL_COUNT) return -1.0f;

    // Use 0-12V ADC range for supply measurements
    float raw_v = measure_via_u23(RAIL_SW[rail], 0 /* V_0_12 */);
    if (raw_v < 0) return -1.0f;

    // Apply voltage divider correction
    return raw_v * RAIL_CORRECTION[rail];
}

float selftest_measure_efuse_current(uint8_t efuse)
{
    if (efuse < 1 || efuse > SELFTEST_EFUSE_COUNT) return -1.0f;

    // Check interlock
    if (adgs_u17_s2_active()) {
        return -1.0f;  // cannot measure while IO 10 analog is active
    }

    // Use 0-12V range — IMON voltage at 1.8A is ~990 mV, well within range
    // (0-625mV range would clip at ~1.14A; use 0-12V for safety, less resolution is OK)
    float v_imon = measure_via_u23(EFUSE_IMON_SW[efuse], 0 /* V_0_12 */);
    if (v_imon < 0) return -1.0f;

    // Convert IMON voltage to current: I = V / (G_IMON × R_IOCP)
    // V_IMON = I_OUT × IMON_MV_PER_A / 1000
    // I_OUT = V_IMON / (IMON_MV_PER_A / 1000)
    float current_a = v_imon / (IMON_MV_PER_A / 1000.0f);
    return current_a;
}

const SelftestEfuseCurrents* selftest_get_efuse_currents(void)
{
    return &s_efuse_curr;
}

const SelftestSupplyVoltages* selftest_get_supply_voltages(void)
{
    return &s_supply_volt;
}

void selftest_monitor_step(void)
{
    // Don't run if calibration is active or U17 S2 is closed
    if (s_cal_result.status == CAL_STATUS_RUNNING) return;

    if (adgs_u17_s2_active()) {
        s_efuse_curr.available = false;
        s_supply_volt.available = false;
        for (int i = 0; i < SELFTEST_EFUSE_COUNT; i++)
            s_efuse_curr.current_a[i] = -1.0f;
        for (int i = 0; i < SELFTEST_RAIL_COUNT; i++)
            s_supply_volt.voltage[i] = -1.0f;
        return;
    }

    // Each call measures ONE channel, then advances to the next.
    // The MUX dead-time between switches is handled by measure_via_u23()
    // which opens all switches, waits, then closes the new path.
    // By measuring only one channel per call, we let the main loop run
    // between measurements.

    uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000ULL);

    // Get PCA9535 state to check which rails/efuses are enabled
    const PCA9535State *pca = pca9535_get_state();

    if (s_monitor_idx < SELFTEST_EFUSE_COUNT) {
        // Measure e-fuse current (indices 0-3 → efuse 1-4)
        uint8_t ef = s_monitor_idx + 1;

        // Skip inactive e-fuses — only measure if enabled via PCA9535
        if (pca && pca->present && pca->efuse_en[s_monitor_idx]) {
            float current = selftest_measure_efuse_current(ef);
            s_efuse_curr.current_a[s_monitor_idx] = current;
        } else {
            s_efuse_curr.current_a[s_monitor_idx] = -1.0f;  // inactive
        }
        s_efuse_curr.available = true;
        s_efuse_curr.timestamp_ms = now_ms;
    } else {
        // Measure supply voltage (indices 4-6 → rail 0-2)
        uint8_t rail = s_monitor_idx - SELFTEST_EFUSE_COUNT;

        // Skip rails whose regulator is not enabled
        bool rail_active = false;
        if (pca && pca->present) {
            if (rail == SELFTEST_RAIL_VADJ1)     rail_active = pca->vadj1_en;
            else if (rail == SELFTEST_RAIL_VADJ2) rail_active = pca->vadj2_en;
            else if (rail == SELFTEST_RAIL_3V3_ADJ) rail_active = true;  // always on when PCA present
        }

        if (rail_active) {
            float voltage = selftest_measure_supply(rail);
            s_supply_volt.voltage[rail] = voltage;
        } else {
            s_supply_volt.voltage[rail] = -1.0f;  // inactive
        }
        s_supply_volt.available = true;
        s_supply_volt.timestamp_ms = now_ms;
    }

    // Advance to next channel, wrapping around
    s_monitor_idx = (s_monitor_idx + 1) % MONITOR_TOTAL_CHANNELS;
}

bool selftest_start_auto_calibrate(uint8_t idac_channel)
{
    if (idac_channel < 1 || idac_channel > 2) {
        ESP_LOGE(TAG, "Invalid IDAC channel %d (must be 1 or 2)", idac_channel);
        return false;
    }

    if (s_cal_result.status == CAL_STATUS_RUNNING) {
        ESP_LOGW(TAG, "Calibration already running");
        return false;
    }

    if (adgs_u17_s2_active()) {
        ESP_LOGE(TAG, "Cannot calibrate: U17 S2 is closed (IO 10 analog mode)");
        return false;
    }

    ESP_LOGI(TAG, "Starting auto-calibration for IDAC channel %d", idac_channel);
    s_cal_result.status = CAL_STATUS_RUNNING;
    s_cal_result.channel = idac_channel;
    s_cal_result.points_collected = 0;
    s_cal_result.error_mv = 0;

    // ── Safety: disable level shifter OE and all e-fuses during calibration ──
    // This prevents any signal from reaching the physical terminals while we
    // manipulate the DCDC voltage across its full range.
    ESP_LOGI(TAG, "  Safety: disabling level shifter OE and all e-fuses");
    pin_write(PIN_LSHIFT_OE, 0);  // level shifter OFF

    const PCA9535State *pca_before = pca9535_get_state();
    bool efuse_was_on[4] = {};
    if (pca_before && pca_before->present) {
        for (int i = 0; i < 4; i++) {
            efuse_was_on[i] = pca_before->efuse_en[i];
            if (efuse_was_on[i]) {
                pca9535_set_control((PcaControl)(PCA_CTRL_EFUSE1_EN + i), false);
            }
        }
    }
    delay_ms(50);  // let e-fuses discharge

    // Determine which U23 switch to use for this IDAC channel
    uint8_t rail = (idac_channel == 1) ? SELFTEST_RAIL_VADJ1 : SELFTEST_RAIL_VADJ2;

    // Clear existing calibration for this channel
    ds4424_cal_clear(idac_channel);

    // Sweep IDAC codes from -100 to +100 in steps of 20
    // At each step: set code → wait for DCDC to settle → measure via U23
    const int8_t codes[] = { -100, -80, -60, -40, -20, 0, 20, 40, 60, 80, 100 };
    const int num_codes = sizeof(codes) / sizeof(codes[0]);

    for (int i = 0; i < num_codes; i++) {
        int8_t code = codes[i];

        // Set IDAC code
        ds4424_set_code(idac_channel, code);
        delay_ms(300);  // wait for DCDC output to settle

        // Measure actual voltage via U23
        float measured_v = selftest_measure_supply(rail);
        if (measured_v < 0) {
            ESP_LOGE(TAG, "  Cal point code=%d: measurement failed", code);
            s_cal_result.status = CAL_STATUS_FAILED;
            ds4424_set_code(idac_channel, 0);
            // Restore safety state
            goto restore;
        }

        ESP_LOGI(TAG, "  Cal point: code=%+4d → %.4f V", code, measured_v);

        // Add calibration point
        ds4424_cal_add_point(idac_channel, code, measured_v);
        s_cal_result.points_collected++;
    }

    // Return IDAC to code 0 (nominal)
    ds4424_set_code(idac_channel, 0);
    delay_ms(100);

    // Verify: set to a test voltage and compare
    float target_v = 8.0f;
    ds4424_set_voltage(idac_channel, target_v);
    delay_ms(300);
    float verify_v = selftest_measure_supply(rail);
    ds4424_set_code(idac_channel, 0);

    if (verify_v > 0) {
        s_cal_result.error_mv = fabsf(verify_v - target_v) * 1000.0f;
        ESP_LOGI(TAG, "  Verification: target=%.2fV actual=%.4fV error=%.1f mV",
                 target_v, verify_v, s_cal_result.error_mv);
    }

    // Save calibration to NVS
    ds4424_cal_save();

    s_cal_result.status = CAL_STATUS_SUCCESS;
    ESP_LOGI(TAG, "Auto-calibration complete for IDAC ch%d (%d points, error=%.1f mV)",
             idac_channel, s_cal_result.points_collected, s_cal_result.error_mv);

restore:
    // ── Restore safety state: re-enable level shifter OE and e-fuses ──
    ESP_LOGI(TAG, "  Restoring level shifter OE and e-fuses");
    pin_write(PIN_LSHIFT_OE, 1);  // level shifter back ON
    if (pca_before && pca_before->present) {
        for (int i = 0; i < 4; i++) {
            if (efuse_was_on[i]) {
                pca9535_set_control((PcaControl)(PCA_CTRL_EFUSE1_EN + i), true);
            }
        }
    }

    return s_cal_result.status == CAL_STATUS_SUCCESS;
}

const SelftestCalResult* selftest_get_cal_result(void)
{
    return &s_cal_result;
}

bool selftest_is_busy(void)
{
    return s_cal_result.status == CAL_STATUS_RUNNING || adgs_selftest_active();
}

#endif // ADGS_HAS_SELFTEST

// =============================================================================
// Internal ADC supply monitoring (works in ALL modes — no U23 needed)
// Uses the AD74416H's 4 diagnostic slots temporarily.
// =============================================================================

static SelftestInternalSupplies s_internal_supplies = {};

// Helper: send a diag config command through the task queue (proper synchronization)
static void send_diag_config(uint8_t slot, uint8_t source)
{
    Command cmd = {};
    cmd.type = CMD_DIAG_CONFIG;
    cmd.diagCfg.slot = slot;
    cmd.diagCfg.source = source;
    xQueueSend(g_cmdQueue, &cmd, pdMS_TO_TICKS(100));
}

// Helper: wait for the periodic diagnostic poll to update a slot with fresh data.
// The fault monitor reads diagnostics every 5th iteration (~1s).
// We wait until the rawCode changes from 0 (invalidated) to a non-zero value,
// or until timeout.
static bool wait_for_diag_update(uint8_t slot, uint32_t timeout_ms)
{
    uint32_t start = (uint32_t)(esp_timer_get_time() / 1000ULL);
    while (true) {
        uint32_t now = (uint32_t)(esp_timer_get_time() / 1000ULL);
        if (now - start > timeout_ms) return false;

        if (xSemaphoreTake(g_stateMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
            bool ready = (g_deviceState.diag[slot].rawCode != 0);
            xSemaphoreGive(g_stateMutex);
            if (ready) return true;
        }
        delay_ms(100);
    }
}

const SelftestInternalSupplies* selftest_measure_internal_supplies(void)
{
    AD74416H *dev = tasks_get_device();
    if (!dev) {
        s_internal_supplies.valid = false;
        return &s_internal_supplies;
    }

    // Save current diagnostic slot configuration
    uint8_t saved_sources[4] = {};
    if (xSemaphoreTake(g_stateMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        for (int i = 0; i < 4; i++) {
            saved_sources[i] = g_deviceState.diag[i].source;
        }
        xSemaphoreGive(g_stateMutex);
    }

    // Phase 1: Configure slots via command queue (properly synchronized with ADC task)
    // Slot 0=TEMP, 1=AVDD_HI, 2=DVCC, 3=AVSS
    ESP_LOGI(TAG, "Configuring diagnostic slots...");
    send_diag_config(0, DIAG_SRC_TEMP);
    send_diag_config(1, DIAG_SRC_AVDD_HI);
    send_diag_config(2, DIAG_SRC_DVCC);
    send_diag_config(3, DIAG_SRC_AVSS);

    // Wait for commands to be processed + ADC to restart + skipReads to expire
    // + at least one fresh diagnostic read.
    // skipReads=2, diag poll every ~1s → need ~3.5s minimum after commands are processed.
    ESP_LOGI(TAG, "Waiting for fresh diagnostic data (5s)...");
    delay_ms(5000);

    // Read cached values from g_deviceState (populated by the fault monitor task)
    if (xSemaphoreTake(g_stateMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        s_internal_supplies.temp_c    = g_deviceState.diag[0].value;
        s_internal_supplies.avdd_hi_v = g_deviceState.diag[1].value;
        s_internal_supplies.dvcc_v    = g_deviceState.diag[2].value;
        s_internal_supplies.avss_v    = g_deviceState.diag[3].value;
        ESP_LOGI(TAG, "Phase 1: TEMP=%.1f AVDD_HI=%.1f DVCC=%.2f AVSS=%.1f",
                 s_internal_supplies.temp_c, s_internal_supplies.avdd_hi_v,
                 s_internal_supplies.dvcc_v, s_internal_supplies.avss_v);
        xSemaphoreGive(g_stateMutex);
    }

    // Phase 2: Measure AVCC using slot 1
    ESP_LOGI(TAG, "Switching slot 1 to AVCC...");
    send_diag_config(1, DIAG_SRC_AVCC);
    delay_ms(5000);  // same wait for fresh data

    if (xSemaphoreTake(g_stateMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        s_internal_supplies.avcc_v = g_deviceState.diag[1].value;
        ESP_LOGI(TAG, "Phase 2: AVCC=%.2f", s_internal_supplies.avcc_v);
        xSemaphoreGive(g_stateMutex);
    }

    s_internal_supplies.valid = true;

    // Check supplies against expected ranges
#if BREADBOARD_MODE
    s_internal_supplies.supplies_ok =
        (s_internal_supplies.avdd_hi_v > 18.0f && s_internal_supplies.avdd_hi_v < 25.0f) &&
        (s_internal_supplies.dvcc_v    > 4.5f  && s_internal_supplies.dvcc_v    < 5.5f)  &&
        (s_internal_supplies.avcc_v    > 4.5f  && s_internal_supplies.avcc_v    < 5.5f)  &&
        (s_internal_supplies.avss_v    < -13.0f && s_internal_supplies.avss_v   > -20.0f);
#else
    s_internal_supplies.supplies_ok =
        (s_internal_supplies.avdd_hi_v > 13.5f && s_internal_supplies.avdd_hi_v < 16.5f) &&
        (s_internal_supplies.dvcc_v    > 3.0f  && s_internal_supplies.dvcc_v    < 3.6f)  &&
        (s_internal_supplies.avcc_v    > 4.5f  && s_internal_supplies.avcc_v    < 5.5f)  &&
        (s_internal_supplies.avss_v    < -13.5f && s_internal_supplies.avss_v   > -16.5f);
#endif

    ESP_LOGI(TAG, "Internal supplies: AVDD_HI=%.1fV DVCC=%.2fV AVCC=%.2fV AVSS=%.1fV Temp=%.1fC %s",
             s_internal_supplies.avdd_hi_v, s_internal_supplies.dvcc_v,
             s_internal_supplies.avcc_v, s_internal_supplies.avss_v,
             s_internal_supplies.temp_c,
             s_internal_supplies.supplies_ok ? "OK" : "OUT OF RANGE");

    // Restore original diagnostic slot configuration
    for (int i = 0; i < 4; i++) {
        send_diag_config(i, saved_sources[i]);
    }

    return &s_internal_supplies;
}

// Note: selftest_measure_internal_supplies() defined below, shared by both modes

#if !ADGS_HAS_SELFTEST
// Stubs for breadboard mode (no U23)

void selftest_init(void)
{
    ESP_LOGW("selftest", "Self-test not available (breadboard mode)");
}

const SelftestBootResult* selftest_boot_check(void) {
    static SelftestBootResult dummy = { false, false, -1, -1, -1 };
    return &dummy;
}

float selftest_measure_supply(uint8_t rail)         { return -1.0f; }
float selftest_measure_efuse_current(uint8_t efuse)  { return -1.0f; }

const SelftestEfuseCurrents* selftest_get_efuse_currents(void) {
    static SelftestEfuseCurrents dummy = {};
    for (int i = 0; i < SELFTEST_EFUSE_COUNT; i++) dummy.current_a[i] = -1.0f;
    dummy.available = false;
    return &dummy;
}

const SelftestSupplyVoltages* selftest_get_supply_voltages(void) {
    static SelftestSupplyVoltages dummy = {};
    for (int i = 0; i < SELFTEST_RAIL_COUNT; i++) dummy.voltage[i] = -1.0f;
    dummy.available = false;
    return &dummy;
}
void selftest_monitor_step(void) {}
bool selftest_start_auto_calibrate(uint8_t ch) { return false; }

const SelftestCalResult* selftest_get_cal_result(void) {
    static SelftestCalResult dummy = {};
    return &dummy;
}

bool selftest_is_busy(void) { return false; }

#endif // !ADGS_HAS_SELFTEST
