// =============================================================================
// smu_cal.c — factory calibration for the DAQ HAT onboard supply.
// See smu_cal.h for the high-level description of the two routines.
// =============================================================================

#include "smu_cal.h"

#include <string.h>
#include <stdlib.h>
#include <stddef.h>
#include <math.h>

#include "esp_log.h"
#include "esp_rom_crc.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "config.h"
#include "daq_board.h"
#include "smu.h"
#include "range_manager.h"
#include "adaq7769.h"
#include "ds4424_p4.h"

static const char *TAG = "smu_cal";

// -----------------------------------------------------------------------------
// Small helpers
// -----------------------------------------------------------------------------

static int cmp_float(const void *a, const void *b)
{
    float fa = *(const float *)a, fb = *(const float *)b;
    return (fa > fb) - (fa < fb);
}

// Drive the U25 voltage mux to a 2-bit channel address (and keep it enabled).
static void volt_mux_select(uint8_t addr)
{
    gpio_set_direction(VOLT_MUX_A0_PIN, GPIO_MODE_OUTPUT);
    gpio_set_direction(VOLT_MUX_A1_PIN, GPIO_MODE_OUTPUT);
    gpio_set_level(VOLT_MUX_A0_PIN, addr & 1);
    gpio_set_level(VOLT_MUX_A1_PIN, (addr >> 1) & 1);
    gpio_set_direction(MUX_EN_PIN, GPIO_MODE_OUTPUT);
    gpio_set_level(MUX_EN_PIN, 1);
}

// DS4424 code that programs the minimum DUT current limit. The current-limit
// helper in smu.c maps reduce-fraction 1.0 -> magnitude 127 with the configured
// polarity; we mirror that so cal and runtime agree.
// TODO(bench): confirm the sign — it selects whether the DAC sources or sinks
// into the CTL (I_FB_DCDC) node. Flip SMU_ILIMIT_CODE_POLARITY if reversed.
static int8_t ilimit_min_code(void)
{
    int code = 127 * SMU_ILIMIT_CODE_POLARITY;
    if (code > 127)  code = 127;
    if (code < -127) code = -127;
    return (int8_t)code;
}

// -----------------------------------------------------------------------------
// Per-point sampling
// -----------------------------------------------------------------------------

typedef esp_err_t (*sample_fn)(smu_cal_t *c, float *out);

// Read one V_DUT sample off the VOLTAGE ADAQ (U23 via U25 S4).
static esp_err_t sample_vdut(smu_cal_t *c, float *out)
{
    daq_board_t *b = c->board;
    if (!b->adaq_ok[ADAQ_ROLE_VOLTAGE]) return ESP_ERR_INVALID_STATE;
    int32_t raw = 0;
    esp_err_t e = adaq7769_read_sample(&b->adaq[ADAQ_ROLE_VOLTAGE], &raw);
    if (e != ESP_OK) return e;
    *out = adaq7769_code_to_volts(&b->adaq[ADAQ_ROLE_VOLTAGE], raw) * V_DUT_SENSE_SCALE;
    return ESP_OK;
}

// Read one output-current sample off the COARSE ADAQ (U22, 50 mohm, LO range).
static esp_err_t sample_iout(smu_cal_t *c, float *out)
{
    daq_board_t *b = c->board;
    if (!b->adaq_ok[ADAQ_ROLE_COARSE]) return ESP_ERR_INVALID_STATE;
    int32_t raw = 0;
    esp_err_t e = adaq7769_read_sample(&b->adaq[ADAQ_ROLE_COARSE], &raw);
    if (e != ESP_OK) return e;
    float v = adaq7769_code_to_volts(&b->adaq[ADAQ_ROLE_COARSE], raw);
    *out = range_manager_volts_to_amps(&b->range, RANGE_LO, v);
    return ESP_OK;
}

// Collect a stable measurement: median-filter SMU_CAL_SAMPLES_PER_PT reads, then
// wait for a SMU_CAL_SETTLE_WINDOW sliding window to fall below `noise`. Returns
// false on abort or settle timeout.
static bool measure_stable(smu_cal_t *c, sample_fn fn, float noise, float *result)
{
    float window[SMU_CAL_SETTLE_WINDOW];
    int   wn = 0;

    for (int it = 0; it < SMU_CAL_SETTLE_ITERS_MAX; ++it) {
        if (c->abort_req) return false;

        float buf[SMU_CAL_SAMPLES_PER_PT];
        int n = 0;
        for (int s = 0; s < SMU_CAL_SAMPLES_PER_PT; ++s) {
            float v;
            if (fn(c, &v) == ESP_OK) buf[n++] = v;
        }
        if (n == 0) {
            vTaskDelay(pdMS_TO_TICKS(SMU_CAL_SETTLE_MS));
            continue;
        }

        qsort(buf, n, sizeof(float), cmp_float);
        int lo = (n > SMU_CAL_MEDIAN_WINDOW) ? (n - SMU_CAL_MEDIAN_WINDOW) / 2 : 0;
        int hi = (n > SMU_CAL_MEDIAN_WINDOW) ? lo + SMU_CAL_MEDIAN_WINDOW : n;
        double acc = 0.0;
        for (int i = lo; i < hi; ++i) acc += buf[i];
        float meas = (float)(acc / (double)(hi - lo));
        c->measured = meas;

        if (wn < SMU_CAL_SETTLE_WINDOW) {
            window[wn++] = meas;
        } else {
            memmove(window, window + 1, (SMU_CAL_SETTLE_WINDOW - 1) * sizeof(float));
            window[SMU_CAL_SETTLE_WINDOW - 1] = meas;
        }

        if (wn >= SMU_CAL_SETTLE_WINDOW) {
            float wmin = window[0], wmax = window[0];
            for (int i = 1; i < wn; ++i) {
                if (window[i] < wmin) wmin = window[i];
                if (window[i] > wmax) wmax = window[i];
            }
            if ((wmax - wmin) < noise) {
                double a = 0.0;
                for (int i = 0; i < wn; ++i) a += window[i];
                *result = (float)(a / (double)wn);
                return true;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(SMU_CAL_SETTLE_MS));
    }
    return false;   // never settled
}

// -----------------------------------------------------------------------------
// NVS persistence
// -----------------------------------------------------------------------------

static uint32_t blob_crc(const smu_cal_blob_t *bl)
{
    const uint8_t *p = (const uint8_t *)&bl->version;
    size_t len = sizeof(*bl) - offsetof(smu_cal_blob_t, version);
    return esp_rom_crc32_le(0, p, len);
}

static esp_err_t blob_save(smu_cal_t *c)
{
    c->blob.magic   = SMU_CAL_MAGIC;
    c->blob.version = SMU_CAL_VERSION;
    c->blob.crc     = blob_crc(&c->blob);

    nvs_handle_t h;
    esp_err_t e = nvs_open(SMU_CAL_NVS_NS, NVS_READWRITE, &h);
    if (e != ESP_OK) return e;
    e = nvs_set_blob(h, SMU_CAL_NVS_KEY, &c->blob, sizeof(c->blob));
    if (e == ESP_OK) e = nvs_commit(h);
    nvs_close(h);
    return e;
}

static esp_err_t blob_load(smu_cal_t *c)
{
    nvs_handle_t h;
    esp_err_t e = nvs_open(SMU_CAL_NVS_NS, NVS_READONLY, &h);
    if (e != ESP_OK) return e;
    size_t sz = sizeof(c->blob);
    e = nvs_get_blob(h, SMU_CAL_NVS_KEY, &c->blob, &sz);
    nvs_close(h);
    if (e != ESP_OK || sz != sizeof(c->blob)) return ESP_ERR_NOT_FOUND;
    if (c->blob.magic != SMU_CAL_MAGIC)        return ESP_ERR_INVALID_CRC;
    if (c->blob.crc != blob_crc(&c->blob))     return ESP_ERR_INVALID_CRC;
    c->have_vcal = (c->blob.vcount > 1);
    c->have_ical = (c->blob.icount > 1);
    return ESP_OK;
}

// -----------------------------------------------------------------------------
// Validation
// -----------------------------------------------------------------------------

// Validate a freshly-swept table: enough points, full coverage, monotonic,
// no large gaps. `lo_target`/`hi_target` bound the expected measurement span.
static uint16_t validate_table(const smu_cal_point_t *pts, uint8_t count,
                               float lo_target, float hi_target,
                               float max_gap, uint8_t min_points)
{
    uint16_t flags = 0;
    if (count < min_points) flags |= SMU_CAL_FLAG_TOO_FEW_POINTS;
    if (count < 2) return flags | SMU_CAL_FLAG_TOO_FEW_POINTS;

    // Sort the measured values to assess coverage + gaps independent of code
    // direction.
    float vals[SMU_CAL_MAX_POINTS];
    for (int i = 0; i < count; ++i) vals[i] = pts[i].value;
    qsort(vals, count, sizeof(float), cmp_float);

    if (vals[0] > lo_target)          flags |= SMU_CAL_FLAG_LOW_COVERAGE;
    if (vals[count - 1] < hi_target)  flags |= SMU_CAL_FLAG_HIGH_COVERAGE;

    for (int i = 1; i < count; ++i) {
        if ((vals[i] - vals[i - 1]) > max_gap) {
            flags |= SMU_CAL_FLAG_GAP_TOO_LARGE;
            break;
        }
    }

    // Monotonic vs code: value must move one consistent direction as code steps.
    int up = 0, down = 0;
    for (int i = 1; i < count; ++i) {
        if (pts[i].value > pts[i - 1].value) up++;
        else if (pts[i].value < pts[i - 1].value) down++;
    }
    if (up > 0 && down > 0) {
        // Allow a tiny number of noise reversals; flag if both directions common.
        int minor = (up < down) ? up : down;
        if (minor > count / 10) flags |= SMU_CAL_FLAG_NON_MONOTONIC;
    }
    return flags;
}

// -----------------------------------------------------------------------------
// Table lookup (inverse interpolation: measured value -> DS4424 code)
// -----------------------------------------------------------------------------

static bool table_to_code(const smu_cal_point_t *pts, uint8_t count,
                          float target, int8_t *code_out)
{
    if (count < 2) return false;

    // Points are stored in code order; values are (near) monotonic. Find the
    // bracketing pair and linearly interpolate the code.
    for (int i = 1; i < count; ++i) {
        float v0 = pts[i - 1].value, v1 = pts[i].value;
        float lo = (v0 < v1) ? v0 : v1;
        float hi = (v0 < v1) ? v1 : v0;
        if (target >= lo && target <= hi) {
            float span = v1 - v0;
            float t = (fabsf(span) < 1e-9f) ? 0.0f : (target - v0) / span;
            float code_f = (float)pts[i - 1].code +
                           t * (float)(pts[i].code - pts[i - 1].code);
            int code = (int)lroundf(code_f);
            if (code > 127)  code = 127;
            if (code < -127) code = -127;
            *code_out = (int8_t)code;
            return true;
        }
    }
    // Outside the table: clamp to the nearer endpoint.
    float dist_first = fabsf(target - pts[0].value);
    float dist_last  = fabsf(target - pts[count - 1].value);
    *code_out = (dist_first <= dist_last) ? pts[0].code : pts[count - 1].code;
    return true;
}

bool smu_cal_voltage_to_code(const smu_cal_t *c, float volts, int8_t *code)
{
    if (!c || !c->have_vcal) return false;
    return table_to_code(c->blob.vpoints, c->blob.vcount, volts, code);
}

bool smu_cal_current_to_code(const smu_cal_t *c, float amps, int8_t *code)
{
    if (!c || !c->have_ical) return false;
    return table_to_code(c->blob.ipoints, c->blob.icount, amps, code);
}

// -----------------------------------------------------------------------------
// Run helpers
// -----------------------------------------------------------------------------

// Block until the operator acknowledges the current prompt (or aborts).
static bool wait_for_ack(smu_cal_t *c)
{
    while (!c->ack) {
        if (c->abort_req) return false;
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    c->ack = false;
    return true;
}

// Restore a safe SMU state: output off, range override released, DAC neutral.
static void safe_restore(smu_cal_t *c)
{
    daq_board_t *b = c->board;
    smu_enable(&b->smu, false);
    range_manager_force(&b->range, RANGE_UNKNOWN);   // release override
    // Voltage DAC back to minimum V_DUT; current DAC back to full-scale limit.
    ds4424_set_code(&b->idac, DS4424_CH_VDUT, smu_voltage_to_code(SMU_VDUT_MIN));
    ds4424_set_code(&b->idac, DS4424_CH_ILIMIT, 0);
}

static void reset_run_state(smu_cal_t *c, smu_cal_mode_t mode)
{
    c->mode      = mode;
    c->prompt    = SMU_CAL_PROMPT_NONE;
    c->ack       = false;
    c->abort_req = false;
    c->progress  = 0;
    c->point     = 0;
    c->code      = 0;
    c->measured  = 0.0f;
    c->min_v     = 0.0f;
    c->max_v     = 0.0f;
    c->flags     = 0;
    c->persist   = SMU_CAL_PERSIST_RAM;
}

// -----------------------------------------------------------------------------
// Voltage calibration
// -----------------------------------------------------------------------------

static void run_voltage_cal(smu_cal_t *c)
{
    daq_board_t *b = c->board;

    if (!b->idac_ok || !b->adaq_ok[ADAQ_ROLE_VOLTAGE]) {
        c->flags |= SMU_CAL_FLAG_HARDWARE;
        c->phase  = SMU_CAL_FAILED;
        return;
    }

    // 1) Ask the operator to disconnect the DUT load.
    c->prompt = SMU_CAL_PROMPT_DISCONNECT_LOAD;
    c->phase  = SMU_CAL_PROMPT;
    if (!wait_for_ack(c)) { safe_restore(c); c->phase = SMU_CAL_FAILED; return; }

    c->prompt = SMU_CAL_PROMPT_NONE;
    c->phase  = SMU_CAL_RUNNING;

    // 2) Route U25 to the S4 node (V_DUT direct off the DCDC) and enable output.
    volt_mux_select(VOLT_MUX_ADDR_VDUT);
    ds4424_set_code(&b->idac, DS4424_CH_VDUT, smu_voltage_to_code(SMU_VDUT_MIN));
    smu_enable(&b->smu, true);
    vTaskDelay(pdMS_TO_TICKS(200));

    // 3) Sweep the DS4424 ch1 code across its span (-127..+127 step 2).
    uint8_t cnt = 0;
    float vmin = 1e30f, vmax = -1e30f;
    for (int code = -127; code <= 127 && cnt < SMU_CAL_MAX_POINTS; code += 2) {
        if (c->abort_req) { safe_restore(c); c->phase = SMU_CAL_FAILED; return; }

        c->code = (int8_t)code;
        ds4424_set_code(&b->idac, DS4424_CH_VDUT, (int8_t)code);

        float v = 0.0f;
        if (!measure_stable(c, sample_vdut, SMU_CAL_V_NOISE_V, &v)) {
            c->flags |= SMU_CAL_FLAG_NO_SETTLE;
            continue;   // skip unstable point, keep sweeping
        }
        c->blob.vpoints[cnt].code  = (int8_t)code;
        c->blob.vpoints[cnt].value = v;
        cnt++;
        if (v < vmin) vmin = v;
        if (v > vmax) vmax = v;
        c->point    = cnt;
        c->min_v    = vmin;
        c->max_v    = vmax;
        c->progress = (uint8_t)(((code + 127) * 100) / 254);
    }

    smu_enable(&b->smu, false);
    c->blob.vcount = cnt;

    // 4) Validate: expect coverage close to [VDUT_MIN, VDUT_MAX].
    c->flags |= validate_table(c->blob.vpoints, cnt,
                               SMU_VDUT_MIN + 0.20f, SMU_VDUT_MAX - 0.25f,
                               /*max_gap=*/1.0f, /*min_points=*/96);
    bool ok = !(c->flags & (SMU_CAL_FLAG_TOO_FEW_POINTS |
                            SMU_CAL_FLAG_GAP_TOO_LARGE |
                            SMU_CAL_FLAG_NON_MONOTONIC));

    if (ok) {
        c->have_vcal = true;
        c->persist   = SMU_CAL_PERSIST_SAVING;
        c->persist   = (blob_save(c) == ESP_OK) ? SMU_CAL_PERSIST_SAVED
                                                : SMU_CAL_PERSIST_FAILED;
        smu_set_cal(&b->smu, c);
        c->progress = 100;
        c->phase    = SMU_CAL_SUCCESS;
        ESP_LOGI(TAG, "voltage cal OK: %u points, %.2f..%.2f V", cnt,
                 (double)vmin, (double)vmax);
    } else {
        c->phase = SMU_CAL_FAILED;
        ESP_LOGW(TAG, "voltage cal failed: flags=0x%04x, %u points", c->flags, cnt);
    }
    safe_restore(c);
}

// -----------------------------------------------------------------------------
// Current-limit calibration
// -----------------------------------------------------------------------------

static void run_current_cal(smu_cal_t *c)
{
    daq_board_t *b = c->board;

    if (!b->idac_ok || !b->adaq_ok[ADAQ_ROLE_COARSE]) {
        c->flags |= SMU_CAL_FLAG_HARDWARE;
        c->phase  = SMU_CAL_FAILED;
        return;
    }

    // 1) Set V_DUT = 1.8 V (formula path) and disable the DCDC.
    ds4424_set_code(&b->idac, DS4424_CH_VDUT,
                    smu_voltage_to_code(SMU_CAL_ICAL_VSET_V));
    smu_enable(&b->smu, false);

    // 2) Ask the operator to short the output.
    c->prompt = SMU_CAL_PROMPT_SHORT_OUTPUT;
    c->phase  = SMU_CAL_PROMPT;
    if (!wait_for_ack(c)) { safe_restore(c); c->phase = SMU_CAL_FAILED; return; }

    c->prompt = SMU_CAL_PROMPT_NONE;
    c->phase  = SMU_CAL_RUNNING;

    // 3) Override autorange: close BOTH bypass switches -> 50 mohm (LO) shunt.
    range_manager_force(&b->range, RANGE_LO);

    // 4) Set the DAC to the minimum-current code, wait, then enable the output.
    int8_t min_code = ilimit_min_code();
    ds4424_set_code(&b->idac, DS4424_CH_ILIMIT, min_code);
    vTaskDelay(pdMS_TO_TICKS(SMU_CAL_ICAL_ENABLE_MS));
    smu_enable(&b->smu, true);
    vTaskDelay(pdMS_TO_TICKS(SMU_CAL_ICAL_ENABLE_MS));

    // 5) Step the current limit up (min_code -> 0) until the output reaches the
    //    target. min_code has the same sign as the polarity; sweep toward 0.
    int step = (min_code > 0) ? -1 : +1;
    uint8_t cnt = 0;
    float imin = 1e30f, imax = -1e30f;
    bool reached = false;

    for (int code = min_code; cnt < SMU_CAL_MAX_POINTS; code += step) {
        if (c->abort_req) { safe_restore(c); c->phase = SMU_CAL_FAILED; return; }

        c->code = (int8_t)code;
        ds4424_set_code(&b->idac, DS4424_CH_ILIMIT, (int8_t)code);

        float a = 0.0f;
        if (measure_stable(c, sample_iout, SMU_CAL_I_NOISE_A, &a)) {
            c->blob.ipoints[cnt].code  = (int8_t)code;
            c->blob.ipoints[cnt].value = a;
            cnt++;
            if (a < imin) imin = a;
            if (a > imax) imax = a;
            c->point = cnt;
            c->min_v = imin;
            c->max_v = imax;
            c->progress = (uint8_t)((imax / SMU_CAL_ICAL_TARGET_A) * 100.0f);
            if (c->progress > 99) c->progress = 99;
            if (a >= SMU_CAL_ICAL_TARGET_A) { reached = true; break; }
        } else {
            c->flags |= SMU_CAL_FLAG_NO_SETTLE;
        }
        if (code == 0) break;   // reached full-scale limit
    }

    smu_enable(&b->smu, false);
    range_manager_force(&b->range, RANGE_UNKNOWN);   // release override
    c->blob.icount = cnt;

    if (!reached) c->flags |= SMU_CAL_FLAG_TARGET_UNREACHED;
    c->flags |= validate_table(c->blob.ipoints, cnt,
                               /*lo_target=*/0.10f,
                               /*hi_target=*/SMU_CAL_ICAL_TARGET_A * 0.95f,
                               /*max_gap=*/0.30f, /*min_points=*/8);
    bool ok = !(c->flags & (SMU_CAL_FLAG_TOO_FEW_POINTS |
                            SMU_CAL_FLAG_TARGET_UNREACHED |
                            SMU_CAL_FLAG_NON_MONOTONIC));

    if (ok) {
        c->have_ical = true;
        c->persist   = SMU_CAL_PERSIST_SAVING;
        c->persist   = (blob_save(c) == ESP_OK) ? SMU_CAL_PERSIST_SAVED
                                                : SMU_CAL_PERSIST_FAILED;
        smu_set_cal(&b->smu, c);
        c->progress = 100;
        c->phase    = SMU_CAL_SUCCESS;
        ESP_LOGI(TAG, "current cal OK: %u points, %.3f..%.3f A", cnt,
                 (double)imin, (double)imax);
    } else {
        c->phase = SMU_CAL_FAILED;
        ESP_LOGW(TAG, "current cal failed: flags=0x%04x, %u points", c->flags, cnt);
    }
    safe_restore(c);
}

// -----------------------------------------------------------------------------
// Worker task + public API
// -----------------------------------------------------------------------------

static void cal_task(void *arg)
{
    smu_cal_t *c = (smu_cal_t *)arg;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        if (c->abort_req) { c->abort_req = false; continue; }
        if (c->mode == SMU_CAL_MODE_VOLTAGE) run_voltage_cal(c);
        else                                  run_current_cal(c);
    }
}

esp_err_t smu_cal_init(smu_cal_t *c, struct daq_board *board)
{
    memset(c, 0, sizeof(*c));
    c->board = board;
    c->phase = SMU_CAL_IDLE;

    // NVS must be up before we read the cal blob (app_update may have done it).
    esp_err_t e = nvs_flash_init();
    if (e == ESP_ERR_NVS_NO_FREE_PAGES || e == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }

    if (blob_load(c) == ESP_OK) {
        ESP_LOGI(TAG, "loaded cal: %u V-points, %u I-points",
                 c->blob.vcount, c->blob.icount);
        smu_set_cal(&((daq_board_t *)board)->smu, c);
    } else {
        memset(&c->blob, 0, sizeof(c->blob));
        ESP_LOGI(TAG, "no stored cal; using formula defaults");
    }

    if (xTaskCreatePinnedToCore(cal_task, "smu_cal", 6144, c, 5, &c->task, 0)
        != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t smu_cal_start(smu_cal_t *c, smu_cal_mode_t mode)
{
    if (c->phase == SMU_CAL_PROMPT || c->phase == SMU_CAL_RUNNING) {
        return ESP_ERR_INVALID_STATE;   // already running
    }
    reset_run_state(c, mode);
    c->phase = SMU_CAL_RUNNING;
    xTaskNotifyGive(c->task);
    return ESP_OK;
}

void smu_cal_ack(smu_cal_t *c)
{
    c->ack = true;
}

void smu_cal_abort(smu_cal_t *c)
{
    c->abort_req = true;
}

void smu_cal_get_status(const smu_cal_t *c, smu_cal_status_t *out)
{
    memset(out, 0, sizeof(*out));
    out->phase    = (uint8_t)c->phase;
    out->prompt   = (uint8_t)c->prompt;
    out->mode     = (uint8_t)c->mode;
    out->progress = c->progress;
    out->point    = c->point;
    out->code     = c->code;
    out->persist  = (uint8_t)c->persist;
    out->measured = c->measured;
    out->min_v    = c->min_v;
    out->max_v    = c->max_v;
    out->flags    = c->flags;
    out->vcount   = c->blob.vcount;
    out->icount   = c->blob.icount;
}
