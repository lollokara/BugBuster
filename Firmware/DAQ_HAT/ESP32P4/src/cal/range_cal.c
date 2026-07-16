// =============================================================================
// range_cal.c — per-board autorange threshold calibration.
// See range_cal.h for the full description of the two-pass procedure.
// =============================================================================

#include "range_cal.h"
#include "range_manager.h"
#include "daq_board.h"
#include "adaq7769.h"
#include "smu.h"

#include <string.h>
#include <math.h>
#include "esp_log.h"
#include "esp_rom_crc.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "config.h"

static const char *TAG = "range_cal";

// ---------------------------------------------------------------------------
// NVS helpers
// ---------------------------------------------------------------------------

static uint32_t cal_crc(const range_threshold_cal_t *c)
{
    // CRC32 over all bytes except the trailing crc field itself.
    size_t body = offsetof(range_threshold_cal_t, crc);
    return esp_rom_crc32_le(0, (const uint8_t *)c, body);
}

esp_err_t range_cal_load(range_threshold_cal_t *out_cal)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(RANGE_CAL_NVS_NS, NVS_READONLY, &h);
    if (err != ESP_OK) {
        ESP_LOGI(TAG, "no NVS namespace — using defaults");
        return ESP_OK;
    }
    size_t sz = sizeof(*out_cal);
    err = nvs_get_blob(h, RANGE_CAL_NVS_KEY, out_cal, &sz);
    nvs_close(h);
    if (err != ESP_OK || sz != sizeof(*out_cal)) {
        ESP_LOGI(TAG, "no stored range calibration — using defaults");
        return ESP_OK;
    }
    if (out_cal->magic != RANGE_CAL_MAGIC || out_cal->crc != cal_crc(out_cal)) {
        ESP_LOGW(TAG, "stored calibration CRC/magic mismatch — ignoring");
        memset(out_cal, 0, sizeof(*out_cal));
        return ESP_OK;
    }
    ESP_LOGI(TAG, "range cal loaded: SET_HI=%.3fV RST_HI=%.3fV "
                  "SET_MID=%.3fV RST_MID=%.3fV  trust HI=%.3fmA MID=%.1fmA",
             (double)out_cal->v_set_hi, (double)out_cal->v_reset_hi,
             (double)out_cal->v_set_mid, (double)out_cal->v_reset_mid,
             (double)(out_cal->i_trust_hi * 1e3f),
             (double)(out_cal->i_trust_mid * 1e3f));
    return ESP_OK;
}

static esp_err_t cal_save(const range_threshold_cal_t *c)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(RANGE_CAL_NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_set_blob(h, RANGE_CAL_NVS_KEY, c, sizeof(*c));
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

// ---------------------------------------------------------------------------
// SMU voltage ramp helpers
// ---------------------------------------------------------------------------

// Number of DAC steps across the full SMU range (DS4424 has 256 levels per
// channel polarity; we map 0..255 to V_min..V_max linearly).
#define CAL_VDAC_STEPS     200     // DAC steps for the full SMU range
#define CAL_STEP_DELAY_MS    30    // ms per step (~6 s total sweep per pass)
#define CAL_START_V        2.00f   // safe pre-prompt voltage requested by operator flow
#define CAL_V_MAX          19.50f  // stay below absolute max for safety
#define CAL_FF_STABLE_SAMPLES 2    // consecutive asserted/unasserted reads to accept edge
#define CAL_INRUSH_IGNORE_MS 250   // ignore enable-time latch state / inrush transient
#define SMU_CAL_RESTORE_CURRENT_A 0.05f

// A genuine SET crossing needs enough current to raise the CSA output to
// AR_V_SET; a physically real trip clears this floor several times over
// (measured against a conservative fraction of V_SET, ignoring offset, so it
// stays valid regardless of the per-unit ~1.7-2.0V offset spread). Anything
// asserting the FF pin with current well below this is a transient (operator
// connecting the cal resistor, DAC-step charge current, etc.), not a real
// threshold crossing — even one that persists past the GPIO debounce window.
#define CAL_MIN_PLAUSIBLE_I_HI_A   ((AR_V_SET * 0.4f) / (SHUNT_HI_OHM  * ISENSE_AMP_GAIN))
#define CAL_MIN_PLAUSIBLE_I_MID_A  ((AR_V_SET * 0.4f) / (SHUNT_MID_OHM * ISENSE_AMP_GAIN))

// Set V_DUT to the given integer step (0..CAL_VDAC_STEPS).
static void ramp_voltage(daq_board_t *b, float v)
{
    if (v < CAL_START_V) v = CAL_START_V;
    if (v > CAL_V_MAX)   v = CAL_V_MAX;
    smu_set_voltage(&b->smu, v);
}

static void ramp_step(daq_board_t *b, int step)
{
    float frac = (float)step / (float)CAL_VDAC_STEPS;
    float v = CAL_START_V + frac * (CAL_V_MAX - CAL_START_V);
    ramp_voltage(b, v);
}

static bool ff_stable_level(gpio_num_t pin, int expected)
{
    for (int i = 0; i < CAL_FF_STABLE_SAMPLES; ++i) {
        if (gpio_get_level(pin) != expected) return false;
        vTaskDelay(pdMS_TO_TICKS(CAL_STEP_DELAY_MS));
    }
    return true;
}

// Each DAC step recharges the output capacitor a bit more, and that charge
// current alone can trip the FF latch even though the *steady-state* current
// at this step is well below threshold — indistinguishable from a real
// crossing by reading the pin right after stepping. A real crossing stays
// asserted once the step's transient has settled; a step-charge artifact
// clears on its own within that window. Debounce by waiting the transient
// out and rechecking — do NOT touch the bypass here: switching it is itself
// a transient (reconnecting the shunt) that would just re-trigger the very
// thing we're trying to debounce, oscillating forever instead of settling.
#define CAL_STEP_INRUSH_SETTLE_MS  120

static bool ff_confirmed_after_step(gpio_num_t pin)
{
    if (!gpio_get_level(pin)) return false;
    vTaskDelay(pdMS_TO_TICKS(CAL_STEP_INRUSH_SETTLE_MS));
    return ff_stable_level(pin, 1);
}

// Final physical sanity gate: even after the GPIO debounce above, confirm the
// *measured* current is actually in the ballpark needed to cross AR_V_SET.
// This is what actually distinguishes a real threshold crossing from a
// transient that just happens to outlast the debounce window (e.g. the
// operator still settling the cal-resistor connection) — timing alone can't
// tell them apart, physics can.
static bool current_plausible_for_set(daq_board_t *b, float min_amps)
{
    float amps = 0.0f;
    current_range_t r;
    if (daq_board_read_current(b, &amps, &r) != ESP_OK) return true;  // can't verify; don't block
    return fabsf(amps) >= min_amps;
}

// Returns true once the pin has settled low (either it was never asserted, or
// the enable-time inrush transient cleared within the settle window).
//
// The SR latches only get relieved by an actual bypass-switch transition (the
// same thing normal, non-overridden range_manager_step() does the instant it
// sees the edge). During calibration range_manager_force() holds
// override_active, so range_manager_step() never calls apply_range() on our
// behalf — a latch tripped by the enable-time transient can sit asserted
// indefinitely with nothing to clear it. So if passive waiting doesn't clear
// it, actively pulse the bypass to `clear_range` and back to `base_range` —
// exactly what autorange would have done — before giving up.
static bool ignore_initial_asserted(daq_board_t *b, gpio_num_t pin,
                                    current_range_t base_range,
                                    current_range_t clear_range)
{
    vTaskDelay(pdMS_TO_TICKS(CAL_INRUSH_IGNORE_MS));
    if (!gpio_get_level(pin)) return true;

    ESP_LOGW(TAG, "ignoring enable-time autorange trigger on GPIO%u", (unsigned)pin);
    for (int i = 0; i < 20; ++i) {
        if (!gpio_get_level(pin)) return true;
        vTaskDelay(pdMS_TO_TICKS(CAL_STEP_DELAY_MS));
    }

    ESP_LOGW(TAG, "GPIO%u still asserted — pulsing bypass to clear the latch",
             (unsigned)pin);
    range_manager_force(&b->range, clear_range);
    vTaskDelay(pdMS_TO_TICKS(CAL_STEP_DELAY_MS * 2));
    range_manager_force(&b->range, base_range);
    vTaskDelay(pdMS_TO_TICKS(CAL_STEP_DELAY_MS));

    for (int i = 0; i < 20; ++i) {
        if (!gpio_get_level(pin)) return true;
        vTaskDelay(pdMS_TO_TICKS(CAL_STEP_DELAY_MS));
    }
    ESP_LOGE(TAG, "GPIO%u still asserted after bypass pulse — stuck trigger",
             (unsigned)pin);
    return false;
}

// Restore a known safe, non-stale state after calibration or abort. The normal
// SMU setters update the live SMU state fields used by status reporting, while
// releasing range override lets autorange reacquire from the low-current 2 V state.
static void range_cal_restore_safe(daq_board_t *b)
{
    smu_enable(&b->smu, false);
    smu_set_current_limit(&b->smu, SMU_CAL_RESTORE_CURRENT_A);
    ramp_voltage(b, CAL_START_V);
    range_manager_force(&b->range, RANGE_UNKNOWN);
}

// ---------------------------------------------------------------------------
// Single-sample ADAQ poll (raw volts, no offset correction)
// ---------------------------------------------------------------------------

static esp_err_t read_raw_fine(daq_board_t *b, float *out)
{
    if (!b->adaq_ok[ADAQ_ROLE_FINE]) return ESP_ERR_INVALID_STATE;
    int32_t raw = 0;
    esp_err_t e = adaq7769_read_sample(&b->adaq[ADAQ_ROLE_FINE], &raw);
    if (e != ESP_OK) return e;
    *out = adaq7769_code_to_volts(&b->adaq[ADAQ_ROLE_FINE], raw);
    return ESP_OK;
}

static esp_err_t read_raw_coarse(daq_board_t *b, float *out)
{
    if (!b->adaq_ok[ADAQ_ROLE_COARSE]) return ESP_ERR_INVALID_STATE;
    int32_t raw = 0;
    esp_err_t e = adaq7769_read_sample(&b->adaq[ADAQ_ROLE_COARSE], &raw);
    if (e != ESP_OK) return e;
    *out = adaq7769_code_to_volts(&b->adaq[ADAQ_ROLE_COARSE], raw);
    return ESP_OK;
}

// ---------------------------------------------------------------------------
// Pass A: calibrate HI↔MID boundary
//
// Ramps UP until FF_HI fires (record v_set_hi), then IMMEDIATELY reverses
// from that same step so the resistor is never exposed to higher current than
// strictly necessary. On the way down records v_reset_hi when FF_HI releases.
// ---------------------------------------------------------------------------
static esp_err_t pass_a(range_cal_engine_t *c, daq_board_t *b)
{
    ESP_LOGI(TAG, "pass A: ramp UP watching FF_HI...");
    int set_step = -1;

    if (!ignore_initial_asserted(b, AR_FF_HI_PIN, RANGE_HI, RANGE_MID)) {
        c->error_code = RANGE_CAL_ERR_FF_STUCK; return ESP_FAIL;
    }

    // Phase 1 — ramp up, stop immediately at POSEDGE
    for (int step = 0; step <= CAL_VDAC_STEPS; step++) {
        ramp_step(b, step);
        vTaskDelay(pdMS_TO_TICKS(CAL_STEP_DELAY_MS));
        c->progress = (uint8_t)((float)step / CAL_VDAC_STEPS * 25.0f);  // 0–25 %

        float fine_v = 0.0f;
        if (read_raw_fine(b, &fine_v) != ESP_OK) {
            c->error_code = RANGE_CAL_ERR_ADAQ; return ESP_FAIL;
        }
        if (ff_confirmed_after_step(AR_FF_HI_PIN)) {
            if (!current_plausible_for_set(b, CAL_MIN_PLAUSIBLE_I_HI_A)) {
                ESP_LOGW(TAG, "step %d: FF_HI asserted but measured current is "
                              "implausibly low for a real SET — treating as "
                              "transient, continuing ramp", step);
                continue;
            }
            c->v_set_hi = fine_v;
            set_step    = step;
            ESP_LOGI(TAG, "FF_HI SET  @ step %d: v_set_hi = %.4f V",
                     step, (double)fine_v);
            break;
        }
    }
    if (set_step < 0) {
        c->error_code = RANGE_CAL_ERR_NO_SET_HI; return ESP_FAIL;
    }

    // The HI (51 Ω) channel is shorted while FF_HI is set, so the RESET
    // comparator for this boundary lives on the MID (2 Ω) channel — switch
    // the bypass + FINE mux there for real (this also physically clears
    // FF_HI, which is a latch and otherwise never self-releases under
    // override) and give the mux/ADAQ filter time to settle before reading.
    range_manager_force(&b->range, RANGE_MID);
    vTaskDelay(pdMS_TO_TICKS(CAL_STEP_DELAY_MS * 3));

    // Phase 2 — ramp DOWN from the SET step, never going higher
    ESP_LOGI(TAG, "pass A: ramp DOWN from step %d watching FF_HI NEGEDGE...", set_step);
    bool found_reset = false;
    for (int step = set_step; step >= 0; step--) {
        ramp_step(b, step);
        vTaskDelay(pdMS_TO_TICKS(CAL_STEP_DELAY_MS));
        // Progress 25–50 %: map (set_step..0) -> (25..50)
        c->progress = (uint8_t)(25.0f + (float)(set_step - step) /
                                         (float)(set_step + 1) * 25.0f);

        if (ff_stable_level(AR_FF_HI_PIN, 0)) {
            // NEGEDGE: FF_HI released. Extra settle before reading.
            vTaskDelay(pdMS_TO_TICKS(CAL_STEP_DELAY_MS * 2));
            float fine_v = 0.0f;
            if (read_raw_fine(b, &fine_v) != ESP_OK) {
                c->error_code = RANGE_CAL_ERR_ADAQ; return ESP_FAIL;
            }
            c->v_reset_hi = fine_v;
            ESP_LOGI(TAG, "FF_HI RST  @ step %d: v_reset_hi = %.4f V (MID-CSA)",
                     step, (double)fine_v);
            found_reset = true;
            break;
        }
    }
    if (!found_reset) {
        c->error_code = RANGE_CAL_ERR_NO_RST_HI; return ESP_FAIL;
    }
    // Ramp back to the 2 V hold point to leave the resistor cool.
    ramp_step(b, 0);
    return ESP_OK;
}

// ---------------------------------------------------------------------------
// Pass B: calibrate MID↔LO boundary
//
// Same stop-and-reverse discipline: reverses at the MID POSEDGE step, then
// records the COARSE voltage at the NEGEDGE on the way back down.
// ---------------------------------------------------------------------------
static esp_err_t pass_b(range_cal_engine_t *c, daq_board_t *b)
{
    ESP_LOGI(TAG, "pass B: ramp UP watching FF_MID...");
    int set_step = -1;

    if (!ignore_initial_asserted(b, AR_FF_MID_PIN, RANGE_MID, RANGE_LO)) {
        c->error_code = RANGE_CAL_ERR_FF_STUCK; return ESP_FAIL;
    }

    for (int step = 0; step <= CAL_VDAC_STEPS; step++) {
        ramp_step(b, step);
        vTaskDelay(pdMS_TO_TICKS(CAL_STEP_DELAY_MS));
        c->progress = (uint8_t)(50.0f + (float)step / CAL_VDAC_STEPS * 25.0f);

        float fine_v = 0.0f;
        if (read_raw_fine(b, &fine_v) != ESP_OK) {
            c->error_code = RANGE_CAL_ERR_ADAQ; return ESP_FAIL;
        }
        if (ff_confirmed_after_step(AR_FF_MID_PIN)) {
            if (!current_plausible_for_set(b, CAL_MIN_PLAUSIBLE_I_MID_A)) {
                ESP_LOGW(TAG, "step %d: FF_MID asserted but measured current is "
                              "implausibly low for a real SET — treating as "
                              "transient, continuing ramp", step);
                continue;
            }
            c->v_set_mid = fine_v;
            set_step     = step;
            ESP_LOGI(TAG, "FF_MID SET @ step %d: v_set_mid = %.4f V",
                     step, (double)fine_v);
            break;
        }
    }
    if (set_step < 0) {
        c->error_code = RANGE_CAL_ERR_NO_SET_MID; return ESP_FAIL;
    }

    // Same reasoning as pass_a: the MID (2 Ω) channel is shorted while FF_MID
    // is set, so the RESET comparator for this boundary lives on the LO
    // (50 mΩ / COARSE) channel — switch there for real (also physically
    // clears the FF_MID latch, which never self-releases under override).
    range_manager_force(&b->range, RANGE_LO);
    vTaskDelay(pdMS_TO_TICKS(CAL_STEP_DELAY_MS * 3));

    ESP_LOGI(TAG, "pass B: ramp DOWN from step %d watching FF_MID NEGEDGE...", set_step);
    bool found_reset = false;
    for (int step = set_step; step >= 0; step--) {
        ramp_step(b, step);
        vTaskDelay(pdMS_TO_TICKS(CAL_STEP_DELAY_MS));
        c->progress = (uint8_t)(75.0f + (float)(set_step - step) /
                                         (float)(set_step + 1) * 20.0f);

        if (ff_stable_level(AR_FF_MID_PIN, 0)) {
            vTaskDelay(pdMS_TO_TICKS(CAL_STEP_DELAY_MS * 2));
            float coarse_v = 0.0f;
            if (read_raw_coarse(b, &coarse_v) != ESP_OK) {
                c->error_code = RANGE_CAL_ERR_ADAQ; return ESP_FAIL;
            }
            c->v_reset_mid = coarse_v;
            ESP_LOGI(TAG, "FF_MID RST @ step %d: v_reset_mid = %.4f V (LO-CSA)",
                     step, (double)coarse_v);
            found_reset = true;
            break;
        }
    }
    if (!found_reset) {
        c->error_code = RANGE_CAL_ERR_NO_RST_MID; return ESP_FAIL;
    }
    ramp_step(b, 0);
    return ESP_OK;
}

// ---------------------------------------------------------------------------
// Sanity check & derived quantities
// ---------------------------------------------------------------------------

// Accept the calibration if the measured thresholds are plausible:
//   * SET voltages must be between 2.5 V and 4.8 V (the ADCMP600 reference
//     is close to 3.9 V but allows for some tolerance + CSA output variation)
//   * RESET voltages must be between 0.20 V and 1.50 V
//   * v_set > v_reset (positive hardware hysteresis in CSA voltage domain)
static bool sanity_check(const range_cal_engine_t *c)
{
    const float V_SET_MIN = 2.5f, V_SET_MAX = 4.8f;
    const float V_RST_MIN = 0.20f, V_RST_MAX = 1.50f;

    bool ok = (c->v_set_hi  >= V_SET_MIN && c->v_set_hi  <= V_SET_MAX) &&
              (c->v_set_mid >= V_SET_MIN && c->v_set_mid <= V_SET_MAX) &&
              (c->v_reset_hi  >= V_RST_MIN && c->v_reset_hi  <= V_RST_MAX) &&
              (c->v_reset_mid >= V_RST_MIN && c->v_reset_mid <= V_RST_MAX) &&
              (c->v_set_hi  > c->v_reset_hi) &&
              (c->v_set_mid > c->v_reset_mid);
    if (!ok) {
        ESP_LOGE(TAG, "calibration results out of expected range — implausible");
    }
    return ok;
}

// Compute fine_trust_max values from calibrated SET thresholds.
// I_safe = RANGE_CAL_SAFE_FRAC × (V_SET − offset) / (shunt × gain)
// offset for HI = cal[HI].offset_v; for MID = cal[MID].offset_v.
static void derive_trust(range_cal_engine_t *c, daq_board_t *b,
                         float *i_trust_hi, float *i_trust_mid)
{
    const range_manager_t *rm = &b->range;

    // HI range trust max (in amps).
    float denom_hi = rm->cal[RANGE_HI].shunt_ohm * rm->cal[RANGE_HI].amp_gain;
    if (denom_hi > 0.0f) {
        *i_trust_hi = RANGE_CAL_SAFE_FRAC
                      * (c->v_set_hi - rm->cal[RANGE_HI].offset_v) / denom_hi;
        if (*i_trust_hi < 1e-6f) *i_trust_hi = 1e-6f;
    } else {
        *i_trust_hi = 1.40e-3f;
    }

    // MID range trust max (in amps).
    float denom_mid = rm->cal[RANGE_MID].shunt_ohm * rm->cal[RANGE_MID].amp_gain;
    if (denom_mid > 0.0f) {
        *i_trust_mid = RANGE_CAL_SAFE_FRAC
                       * (c->v_set_mid - rm->cal[RANGE_MID].offset_v) / denom_mid;
        if (*i_trust_mid < 1e-4f) *i_trust_mid = 1e-4f;
    } else {
        *i_trust_mid = 3.60e-2f;
    }

    ESP_LOGI(TAG, "derived: i_trust_hi=%.3f mA  i_trust_mid=%.1f mA",
             (double)(*i_trust_hi * 1e3f), (double)(*i_trust_mid * 1e3f));
}

// ---------------------------------------------------------------------------
// Background calibration task
// ---------------------------------------------------------------------------

static void cal_task(void *arg)
{
    range_cal_engine_t  *c = (range_cal_engine_t *)arg;
    daq_board_t  *b = c->board;

    // ---- Pre-conditions ------------------------------------------------
    // Calibration must run with fast acquisition stopped.
    daq_board_stop_fast(b);

    // Set current limit high enough so we don't starve the ramp.
    smu_set_current_limit(&b->smu, 2.5f);

    // Start the operator flow with the supply already enabled at 2 V. This lets
    // the user plug in the resistor under the requested low-voltage condition.
    smu_enable(&b->smu, false);
    ramp_voltage(b, CAL_START_V);
    smu_enable(&b->smu, true);

    // Force range to HI for pass A (fine mux → 51 Ω CSA).
    range_manager_force(&b->range, RANGE_HI);

    // ---- Wait for operator to connect R_CAL_A -------------------------
    c->phase = RANGE_CAL_PROMPT_A;
    ESP_LOGI(TAG, "waiting for ACK: connect R_CAL_A = %.0f Ω", (double)c->r_cal_a_ohm);

    // Block until ack or abort.
    while (c->phase == RANGE_CAL_PROMPT_A) {
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    if (c->phase == RANGE_CAL_IDLE) {
        goto cleanup;  // aborted
    }

    // ---- Pass A --------------------------------------------------------
    c->phase = RANGE_CAL_RUNNING_A;

    if (pass_a(c, b) != ESP_OK) goto fail;

    // pass_a() already returned V_DUT to the 2 V hold point. Force MID so pass B
    // starts from a clean state for the MID↔LO boundary.
    range_manager_force(&b->range, RANGE_MID);

    ramp_voltage(b, CAL_START_V);
    c->progress = 50;

    // ---- Wait for operator to connect R_CAL_B -------------------------
    c->phase = RANGE_CAL_PROMPT_B;
    ESP_LOGI(TAG, "waiting for ACK: connect R_CAL_B = %.0f Ω", (double)c->r_cal_b_ohm);
    while (c->phase == RANGE_CAL_PROMPT_B) {
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    if (c->phase == RANGE_CAL_IDLE) {
        goto cleanup;
    }

    // ---- Pass B --------------------------------------------------------
    c->phase = RANGE_CAL_RUNNING_B;
    range_manager_force(&b->range, RANGE_MID);
    ramp_voltage(b, CAL_START_V);
    smu_enable(&b->smu, true);

    if (pass_b(c, b) != ESP_OK) goto fail;

    range_cal_restore_safe(b);

    // ---- Sanity check --------------------------------------------------
    if (!sanity_check(c)) {
        c->error_code = RANGE_CAL_ERR_IMPLAUSIBLE;
        goto fail;
    }

    // ---- Build and persist calibration struct --------------------------
    {
        float i_trust_hi = 0.0f, i_trust_mid = 0.0f;
        derive_trust(c, b, &i_trust_hi, &i_trust_mid);

        range_threshold_cal_t stored = {
            .magic       = RANGE_CAL_MAGIC,
            .v_set_hi    = c->v_set_hi,
            .v_set_mid   = c->v_set_mid,
            .v_reset_hi  = c->v_reset_hi,
            .v_reset_mid = c->v_reset_mid,
            .r_cal_a_ohm = c->r_cal_a_ohm,
            .r_cal_b_ohm = c->r_cal_b_ohm,
            .i_trust_hi  = i_trust_hi,
            .i_trust_mid = i_trust_mid,
        };
        stored.crc = cal_crc(&stored);

        if (cal_save(&stored) != ESP_OK) {
            ESP_LOGE(TAG, "NVS write failed");
            c->error_code = RANGE_CAL_ERR_NVS;
            goto fail;
        }

        // Apply live: update current_fusion trust windows so the new calibration
        // takes effect immediately without requiring a reboot.
        current_fusion_set_trust(&b->fusion, i_trust_hi, i_trust_mid);
        ESP_LOGI(TAG, "calibration complete and applied live");
    }

    c->progress = 100;
    c->phase    = RANGE_CAL_SUCCESS;
    goto cleanup;

fail:
    range_cal_restore_safe(b);
    c->phase = RANGE_CAL_FAILED;

cleanup:
    range_cal_restore_safe(b);
    c->task = NULL;
    vTaskDelete(NULL);
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

esp_err_t range_cal_start(range_cal_engine_t *c, struct daq_board *b)
{
    if (c->phase == RANGE_CAL_RUNNING_A || c->phase == RANGE_CAL_RUNNING_B ||
        c->phase == RANGE_CAL_PROMPT_A  || c->phase == RANGE_CAL_PROMPT_B) {
        return ESP_ERR_INVALID_STATE;
    }
    // Save caller-supplied R_cal values before memset wipes them.
    float r_a = c->r_cal_a_ohm;
    float r_b = c->r_cal_b_ohm;
    memset(c, 0, sizeof(*c));
    c->board       = b;
    c->phase       = RANGE_CAL_IDLE;
    c->r_cal_a_ohm = (r_a > 0.0f) ? r_a : 5600.0f;
    c->r_cal_b_ohm = (r_b > 0.0f) ? r_b : 56.0f;

    BaseType_t ok = xTaskCreatePinnedToCore(cal_task, "range_cal", 4096, c,
                                            /*prio=*/5, &c->task, /*core=*/0);
    if (ok != pdPASS) return ESP_ERR_NO_MEM;

    // Wait up to 2 s for the background task to set the first PROMPT phase
    // so callers can immediately poll phase without hitting the IDLE race.
    for (int i = 0; i < 40 && c->phase == RANGE_CAL_IDLE; i++)
        vTaskDelay(pdMS_TO_TICKS(50));
    return ESP_OK;
}

void range_cal_ack(range_cal_engine_t *c)
{
    // Advance past PROMPT to RUNNING; cal_task is polling the phase field.
    if (c->phase == RANGE_CAL_PROMPT_A) {
        c->phase = RANGE_CAL_RUNNING_A;
    } else if (c->phase == RANGE_CAL_PROMPT_B) {
        c->phase = RANGE_CAL_RUNNING_B;
    }
}

void range_cal_abort(range_cal_engine_t *c)
{
    // Signal the task to stop; it checks for IDLE during prompt waits.
    c->phase = RANGE_CAL_IDLE;
}

void range_cal_get_status(const range_cal_engine_t *c, range_cal_phase_t *phase,
                          uint8_t *progress, uint8_t *error_code)
{
    if (phase)      *phase      = c->phase;
    if (progress)   *progress   = c->progress;
    if (error_code) *error_code = c->error_code;
}
