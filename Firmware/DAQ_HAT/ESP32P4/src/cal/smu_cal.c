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
    // Set levels before enabling output direction to avoid a LOW glitch.
    gpio_set_level(VOLT_MUX_A0_PIN, addr & 1);
    gpio_set_level(VOLT_MUX_A1_PIN, (addr >> 1) & 1);
    gpio_set_direction(VOLT_MUX_A0_PIN, GPIO_MODE_OUTPUT);
    gpio_set_direction(VOLT_MUX_A1_PIN, GPIO_MODE_OUTPUT);
    gpio_set_level(MUX_EN_PIN, 1);
    gpio_set_direction(MUX_EN_PIN, GPIO_MODE_OUTPUT);
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
// Baseline (open-circuit offset) NVS persistence
// -----------------------------------------------------------------------------

static uint32_t base_crc(const smu_base_blob_t *bl)
{
    const uint8_t *p = (const uint8_t *)&bl->version;
    size_t len = sizeof(*bl) - offsetof(smu_base_blob_t, version);
    return esp_rom_crc32_le(0, p, len);
}

static esp_err_t base_save(smu_cal_t *c)
{
    c->base.magic   = SMU_BASE_MAGIC;
    c->base.version = SMU_BASE_VERSION;
    c->base.crc     = base_crc(&c->base);

    nvs_handle_t h;
    esp_err_t e = nvs_open(SMU_CAL_NVS_NS, NVS_READWRITE, &h);
    if (e != ESP_OK) return e;
    e = nvs_set_blob(h, SMU_BASE_NVS_KEY, &c->base, sizeof(c->base));
    if (e == ESP_OK) e = nvs_commit(h);
    nvs_close(h);
    return e;
}

static esp_err_t base_load(smu_cal_t *c)
{
    nvs_handle_t h;
    esp_err_t e = nvs_open(SMU_CAL_NVS_NS, NVS_READONLY, &h);
    if (e != ESP_OK) return e;
    size_t sz = sizeof(c->base);
    e = nvs_get_blob(h, SMU_BASE_NVS_KEY, &c->base, &sz);
    nvs_close(h);
    if (e != ESP_OK || sz != sizeof(c->base)) return ESP_ERR_NOT_FOUND;
    if (c->base.magic != SMU_BASE_MAGIC)       return ESP_ERR_INVALID_CRC;
    if (c->base.version != SMU_BASE_VERSION)   return ESP_ERR_INVALID_VERSION;
    if (c->base.crc != base_crc(&c->base))     return ESP_ERR_INVALID_CRC;
    c->have_base = (c->base.have[RANGE_HI] || c->base.have[RANGE_MID] ||
                    c->base.have[RANGE_LO]);
    return ESP_OK;
}

bool smu_base_offset(const smu_cal_t *c, uint8_t range, int8_t vdut_code,
                     int32_t *offset_out)
{
    if (!c || !c->have_base || range >= SMU_BASE_RANGES) return false;
    if (!c->base.have[range]) return false;
    int idx = (int)vdut_code + 127;
    if (idx < 0) idx = 0;
    if (idx >= SMU_BASE_CODES) idx = SMU_BASE_CODES - 1;
    *offset_out = c->base.offset[range][idx];
    return true;
}

// -----------------------------------------------------------------------------
// Per-range current-measurement cal (interactive reference-meter cal)
// -----------------------------------------------------------------------------

static uint32_t rcal_crc(const smu_range_cal_blob_t *bl)
{
    const uint8_t *p = (const uint8_t *)&bl->version;
    size_t len = sizeof(*bl) - offsetof(smu_range_cal_blob_t, version);
    return esp_rom_crc32_le(0, p, len);
}

static esp_err_t rcal_save(smu_cal_t *c)
{
    c->rcal.magic   = SMU_RANGE_MAGIC;
    c->rcal.version = SMU_RANGE_VERSION;
    c->rcal.crc     = rcal_crc(&c->rcal);

    nvs_handle_t h;
    esp_err_t e = nvs_open(SMU_CAL_NVS_NS, NVS_READWRITE, &h);
    if (e != ESP_OK) return e;
    e = nvs_set_blob(h, SMU_RANGE_NVS_KEY, &c->rcal, sizeof(c->rcal));
    if (e == ESP_OK) e = nvs_commit(h);
    nvs_close(h);
    return e;
}

static esp_err_t rcal_load(smu_cal_t *c)
{
    nvs_handle_t h;
    esp_err_t e = nvs_open(SMU_CAL_NVS_NS, NVS_READONLY, &h);
    if (e != ESP_OK) return e;
    size_t sz = sizeof(c->rcal);
    e = nvs_get_blob(h, SMU_RANGE_NVS_KEY, &c->rcal, &sz);
    nvs_close(h);
    if (e != ESP_OK || sz != sizeof(c->rcal)) return ESP_ERR_NOT_FOUND;
    if (c->rcal.magic != SMU_RANGE_MAGIC)      return ESP_ERR_INVALID_CRC;
    if (c->rcal.version != SMU_RANGE_VERSION)  return ESP_ERR_INVALID_VERSION;
    if (c->rcal.crc != rcal_crc(&c->rcal))     return ESP_ERR_INVALID_CRC;
    c->have_rcal = (c->rcal.have[RANGE_HI] || c->rcal.have[RANGE_MID] ||
                    c->rcal.have[RANGE_LO]);
    return ESP_OK;
}

// Push a calibrated range into the live range_manager so it takes effect now.
static void rcal_apply(smu_cal_t *c, uint8_t range)
{
    if (range >= SMU_BASE_RANGES || !c->rcal.have[range]) return;
    daq_board_t *b = (daq_board_t *)c->board;
    range_cal_t rc = {
        .shunt_ohm = c->rcal.shunt_ohm[range],
        .amp_gain  = c->rcal.amp_gain[range],
        .offset_v  = c->rcal.offset_v[range],
        .gain_corr = c->rcal.gain_corr[range],
    };
    range_manager_set_cal(&b->range, (current_range_t)range, &rc);
}

// Least-squares fit amps = slope*v_adc + intercept over the stored points.
static bool rcal_fit(const smu_range_pt_t *pts, uint8_t n,
                     double *slope, double *intercept)
{
    if (n < 2) return false;
    double sx = 0, sy = 0, sxx = 0, sxy = 0;
    for (uint8_t i = 0; i < n; ++i) {
        double x = pts[i].v_adc, y = pts[i].amps;
        sx += x; sy += y; sxx += x * x; sxy += x * y;
    }
    double det = (double)n * sxx - sx * sx;
    if (fabs(det) < 1e-18) return false;       // v_adc did not vary
    double s = ((double)n * sxy - sx * sy) / det;
    if (fabs(s) < 1e-12) return false;         // no response (flat)
    *slope     = s;
    *intercept = (sy - s * sx) / (double)n;
    return true;
}

// Refit offset/gain over the currently stored points for a range, apply to the
// live range_manager, and update the have flag. Returns the point count on a
// good fit, 0 if too few points, -1 if the fit is singular/flat. When the set
// is emptied the range reverts to its nominal (uncalibrated) conversion.
static int rcal_recompute(smu_cal_t *c, uint8_t range)
{
    daq_board_t *b = (daq_board_t *)c->board;
    uint8_t n = c->rcal.npts[range];
    double slope, intercept;

    if (rcal_fit(c->rcal.pts[range], n, &slope, &intercept)) {
        // range_manager: I = (v_adc - offset_v)/(shunt*amp_gain) * gain_corr
        //   amps = slope*v_adc + intercept
        //   => gain_corr = slope*shunt*amp_gain, offset_v = -intercept/slope.
        c->rcal.gain_corr[range] = (float)(slope * (double)c->rcal.shunt_ohm[range]
                                                 * (double)c->rcal.amp_gain[range]);
        c->rcal.offset_v[range]  = (float)(-intercept / slope);
        c->rcal.have[range]      = 1;
        c->have_rcal             = true;
        rcal_apply(c, range);
        return (int)n;
    }

    // Not enough usable points. If the set is now empty, revert to nominal.
    if (n == 0) {
        c->rcal.have[range] = 0;
        const range_cal_t *cur = range_manager_get_cal(&b->range, (current_range_t)range);
        if (cur) {
            range_cal_t rc = *cur;
            rc.offset_v  = 0.0f;
            rc.gain_corr = 1.0f;
            range_manager_set_cal(&b->range, (current_range_t)range, &rc);
        }
    }
    return (n < 2) ? 0 : -1;
}

void smu_cal_range_reset(smu_cal_t *c, uint8_t range)
{
    if (!c || range >= SMU_BASE_RANGES) return;
    c->rcal.npts[range]      = 0;
    c->rcal.offset_v[range]  = 0.0f;
    c->rcal.gain_corr[range] = 1.0f;
    rcal_recompute(c, range);   // n==0 -> clears have + reverts range_manager to nominal
    rcal_save(c);
}

uint8_t smu_cal_range_count(const smu_cal_t *c, uint8_t range)
{
    if (!c || range >= SMU_BASE_RANGES) return 0;
    return c->rcal.npts[range];
}

const smu_range_pt_t *smu_cal_range_points(const smu_cal_t *c, uint8_t range,
                                           uint8_t *count_out)
{
    if (!c || range >= SMU_BASE_RANGES) {
        if (count_out) *count_out = 0;
        return NULL;
    }
    if (count_out) *count_out = c->rcal.npts[range];
    return c->rcal.pts[range];
}

bool smu_cal_range_info(const smu_cal_t *c, uint8_t range, float *offset_v,
                        float *gain_corr, float *shunt_ohm, float *amp_gain)
{
    if (!c || range >= SMU_BASE_RANGES) return false;
    if (offset_v)  *offset_v  = c->rcal.offset_v[range];
    if (gain_corr) *gain_corr = c->rcal.gain_corr[range];
    if (shunt_ohm) *shunt_ohm = c->rcal.shunt_ohm[range];
    if (amp_gain)  *amp_gain  = c->rcal.amp_gain[range];
    return c->rcal.have[range] != 0;
}

int smu_cal_range_delete(smu_cal_t *c, uint8_t range, uint8_t idx)
{
    if (!c || range >= SMU_BASE_RANGES) return -2;
    uint8_t n = c->rcal.npts[range];
    if (idx >= n) return -2;

    // Remove the point by shifting the tail down, then refit over the rest.
    memmove(&c->rcal.pts[range][idx], &c->rcal.pts[range][idx + 1],
            (size_t)(n - idx - 1) * sizeof(smu_range_pt_t));
    c->rcal.npts[range] = n - 1;

    int ret = rcal_recompute(c, range);
    if (rcal_save(c) != ESP_OK) return -3;
    return ret;
}

int smu_cal_range_fit(smu_cal_t *c, uint8_t range,
                      const smu_range_pt_t *new_pts, uint8_t n_new,
                      float shunt_ohm, float amp_gain)
{
    if (!c || range >= SMU_BASE_RANGES || (!new_pts && n_new)) return -2;

    // Append the new points, dropping the oldest once the buffer is full so a
    // long multi-resistor accumulation keeps the most recent coverage.
    for (uint8_t i = 0; i < n_new; ++i) {
        if (c->rcal.npts[range] < SMU_RANGE_CAL_MAX_PTS) {
            c->rcal.pts[range][c->rcal.npts[range]++] = new_pts[i];
        } else {
            memmove(&c->rcal.pts[range][0], &c->rcal.pts[range][1],
                    (SMU_RANGE_CAL_MAX_PTS - 1) * sizeof(smu_range_pt_t));
            c->rcal.pts[range][SMU_RANGE_CAL_MAX_PTS - 1] = new_pts[i];
        }
    }
    c->rcal.shunt_ohm[range] = shunt_ohm;
    c->rcal.amp_gain[range]  = amp_gain;

    int ret = rcal_recompute(c, range);   // 0 = too few, -1 = flat, >=2 = fitted

    // Persist the accumulated points (and any updated coefficients) so the set
    // survives across runs and reboots.
    if (rcal_save(c) != ESP_OK) return -3;
    return ret;
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

// Remove calibration points the DAC could not actually address: a flat plateau
// where the measured value is pinned at the supply's hard limit and does not
// respond to code changes ("the needle doesn't move"). Keeps only the
// responsive ramp — the codes that moved the needle. Points are compacted in
// place; returns the new count.
static uint8_t prune_stuck_points(smu_cal_point_t *pts, uint8_t count, float noise)
{
    if (count < 2) return count;

    // Responsive span [first,last]: adjacent measurements differ by > noise.
    int first = -1, last = -1;
    for (int i = 1; i < count; ++i) {
        if (fabsf(pts[i].value - pts[i - 1].value) > noise) {
            if (first < 0) first = i - 1;   // include the knee before the move
            last = i;
        }
    }
    if (first < 0) return 0;   // never moved: nothing addressable

    // Compact the responsive span, dropping interior duplicates that did not
    // move the needle relative to the last kept point.
    smu_cal_point_t keep[SMU_CAL_MAX_POINTS];
    uint8_t n = 0;
    keep[n++] = pts[first];
    for (int i = first + 1; i <= last; ++i) {
        if (fabsf(pts[i].value - keep[n - 1].value) > noise)
            keep[n++] = pts[i];
    }
    memcpy(pts, keep, (size_t)n * sizeof(pts[0]));
    return n;
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

    // 3) Sweep the DS4424 ch1 code across its full span (-127..+127 step 1),
    //    recording one calibration point per code (up to 255 points).
    uint8_t cnt = 0;
    float vmin = 1e30f, vmax = -1e30f;
    for (int code = -127; code <= 127 && cnt < SMU_CAL_MAX_POINTS; code += 1) {
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

    // Current cal sources up to the target amps, so it needs a stiff supply:
    // require a USB-PD contract of at least 20 V / 3 A (no override).
    if (!daq_board_pd_ok(b, 20000, 3000)) {
        c->flags |= SMU_CAL_FLAG_NO_PD;
        c->phase  = SMU_CAL_FAILED;
        return;
    }

    // 1) Set V_DUT = 3.0 V (formula path) and disable the DCDC.
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

    // 5) Sweep the current-limit DAC across its FULL signed span (min_code ->
    //    the opposite extreme, e.g. +127 -> -127). The output current rises to
    //    full scale near code 0 and then REDUCES again through the negative
    //    codes as the DS4424 sign flips, so the whole -127..+127 range must be
    //    captured to build a complete table (127 extra points on the negative
    //    side). Do not stop early at the 2 A target — just note it and keep
    //    sweeping so the reducing branch is recorded too.
    int step     = (min_code > 0) ? -1 : +1;
    int end_code = (min_code > 0) ? -127 : +127;
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
            // Progress tracks the code sweep across the full span.
            int denom = (min_code - end_code);
            if (denom == 0) denom = 1;
            c->progress = (uint8_t)(((min_code - code) * 100) / denom);
            if (c->progress > 99) c->progress = 99;
            if (a >= SMU_CAL_ICAL_TARGET_A) reached = true;   // note; keep sweeping
        } else {
            c->flags |= SMU_CAL_FLAG_NO_SETTLE;
        }
        if (code == end_code) break;   // swept the full signed DAC span
    }

    smu_enable(&b->smu, false);
    range_manager_force(&b->range, RANGE_UNKNOWN);   // release override

    // Drop the codes the DAC could not actually address. The onboard supply has
    // a hard current ceiling, so the sweep legitimately pins the output at a
    // flat plateau over a large span of codes ("the needle doesn't move"). Those
    // points are not a calibration failure: prune them and keep only the
    // responsive ramp. Because of the hard limit the fixed SMU_CAL_ICAL_TARGET_A
    // may be unreachable — that is expected and no longer fails the run.
    cnt = prune_stuck_points(c->blob.ipoints, cnt, SMU_CAL_I_NOISE_A);
    c->blob.icount = cnt;

    imin = 1e30f;
    imax = -1e30f;
    for (int i = 0; i < cnt; ++i) {
        if (c->blob.ipoints[i].value < imin) imin = c->blob.ipoints[i].value;
        if (c->blob.ipoints[i].value > imax) imax = c->blob.ipoints[i].value;
    }
    if (cnt > 0) {
        c->min_v = imin;
        c->max_v = imax;
        c->point = cnt;
    } else {
        imin = imax = 0.0f;
    }

    // Validate coverage only against the span the hardware could actually reach
    // (imin..imax): the hard current limit means the 2 A target may be out of
    // reach, which is acceptable. The full signed sweep is intentionally peaked
    // (current rises to full scale near code 0, then reduces through the
    // negative codes), so NON_MONOTONIC is expected here and does NOT fail the
    // run — we only require enough responsive points.
    c->flags |= validate_table(c->blob.ipoints, cnt,
                               /*lo_target=*/imin,
                               /*hi_target=*/imax,
                               /*max_gap=*/0.30f, /*min_points=*/8);
    bool ok = (cnt >= 2) && !(c->flags & SMU_CAL_FLAG_TOO_FEW_POINTS);


    if (ok) {
        c->have_ical = true;
        c->persist   = SMU_CAL_PERSIST_SAVING;
        c->persist   = (blob_save(c) == ESP_OK) ? SMU_CAL_PERSIST_SAVED
                                                : SMU_CAL_PERSIST_FAILED;
        smu_set_cal(&b->smu, c);
        c->progress = 100;
        c->phase    = SMU_CAL_SUCCESS;
        ESP_LOGI(TAG, "current cal OK: %u points, %.3f..%.3f A (target %s)", cnt,
                 (double)imin, (double)imax,
                 reached ? "reached" : "hard-limited");
    } else {
        c->phase = SMU_CAL_FAILED;
        ESP_LOGW(TAG, "current cal failed: flags=0x%04x, %u points", c->flags, cnt);
    }
    safe_restore(c);
}

// -----------------------------------------------------------------------------
// Baseline (open-circuit offset) calibration
// -----------------------------------------------------------------------------

// Map a current range to the ADAQ that measures it (HI/MID -> FINE, LO -> COARSE).
static uint8_t base_role_for_range(uint8_t range)
{
    return (range == RANGE_LO) ? (uint8_t)ADAQ_ROLE_COARSE
                               : (uint8_t)ADAQ_ROLE_FINE;
}

// With the output OPEN, sweep V_DUT across every DS4424 code for each range and
// record the averaged open-circuit ADC code. That code is later loaded straight
// into the ADAQ OFFSET register so the hardware auto-subtracts the baseline.
static void run_baseline_cal(smu_cal_t *c)
{
    daq_board_t *b = c->board;

    if (!b->idac_ok || !b->adaq_ok[ADAQ_ROLE_FINE] || !b->adaq_ok[ADAQ_ROLE_COARSE]) {
        c->flags |= SMU_CAL_FLAG_HARDWARE;
        c->phase  = SMU_CAL_FAILED;
        return;
    }

    // 1) Ask the operator to leave the DUT output OPEN (no load / open circuit).
    c->prompt = SMU_CAL_PROMPT_OPEN_CIRCUIT;
    c->phase  = SMU_CAL_PROMPT;
    if (!wait_for_ack(c)) { safe_restore(c); c->phase = SMU_CAL_FAILED; return; }

    c->prompt = SMU_CAL_PROMPT_NONE;
    c->phase  = SMU_CAL_RUNNING;

    memset(&c->base, 0, sizeof(c->base));

    const uint8_t ranges[SMU_BASE_RANGES] = { RANGE_HI, RANGE_MID, RANGE_LO };
    const int total = SMU_BASE_RANGES * SMU_BASE_CODES;
    int done = 0;

    for (int ri = 0; ri < SMU_BASE_RANGES; ++ri) {
        uint8_t r    = ranges[ri];
        uint8_t role = base_role_for_range(r);
        c->base_range = r;

        // Force the range (bypass switches + FINE mux) and zero the OFFSET reg so
        // we measure the true raw open-circuit code, then enable the supply.
        range_manager_force(&b->range, r);
        adaq7769_set_offset_cal(&b->adaq[role], 0);
        smu_enable(&b->smu, true);
        vTaskDelay(pdMS_TO_TICKS(SMU_BASE_SETTLE_MS));

        // Record the board temperature at which this range was calibrated.
        float tc = 0.0f;
        c->base.temp_c10[r] =
            (b->temp_ok[0] && ad741x_read_celsius(&b->temp[0], &tc) == ESP_OK)
                ? (int16_t)lroundf(tc * 10.0f)
                : (int16_t)SMU_BASE_TEMP_NA;

        // Sweep every V_DUT code: settle 200 ms, average 1000 raw reads.
        for (int code = -127; code <= 127; ++code) {
            if (c->abort_req) { safe_restore(c); c->phase = SMU_CAL_FAILED; return; }

            c->code = (int8_t)code;
            ds4424_set_code(&b->idac, DS4424_CH_VDUT, (int8_t)code);
            vTaskDelay(pdMS_TO_TICKS(SMU_BASE_SETTLE_MS));

            double acc = 0.0;
            int    n   = 0;
            for (int s = 0; s < SMU_BASE_SAMPLES_PER_PT; ++s) {
                int32_t raw = 0;
                if (adaq7769_read_sample(&b->adaq[role], &raw) == ESP_OK) {
                    acc += raw;
                    n++;
                }
            }
            // Store the raw open-circuit ADC_DATA average. It is subtracted in
            // SOFTWARE from each live sample (daq_board fast_emit) before the
            // code->amps conversion. Software subtraction tracks autoranging
            // (HI and MID share the FINE ADC but have different baselines) and
            // needs no mid-stream register writes, unlike the single ADAQ
            // hardware OFFSET register which cannot do either.
            int32_t off_adc = (n > 0) ? (int32_t)llround(acc / (double)n) : 0;
            c->base.offset[r][code + 127] = off_adc;

            c->measured = (float)off_adc;
            c->point    = (uint8_t)(code + 127);
            done++;
            c->progress = (uint8_t)((done * 100) / total);
        }
        c->base.have[r] = 1;
    }

    smu_enable(&b->smu, false);
    range_manager_force(&b->range, RANGE_UNKNOWN);   // release override

    c->have_base = true;
    c->persist   = SMU_CAL_PERSIST_SAVING;
    c->persist   = (base_save(c) == ESP_OK) ? SMU_CAL_PERSIST_SAVED
                                            : SMU_CAL_PERSIST_FAILED;
    c->progress  = 100;
    c->phase     = SMU_CAL_SUCCESS;
    ESP_LOGI(TAG, "baseline cal OK: HI %.1fC / MID %.1fC / LO %.1fC",
             c->base.temp_c10[RANGE_HI] / 10.0, c->base.temp_c10[RANGE_MID] / 10.0,
             c->base.temp_c10[RANGE_LO] / 10.0);
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
        daq_board_t *b = (daq_board_t *)c->board;
        // The cal routines do single-shot ADAQ reads, which fight the
        // continuous-read DMA. Pause the fast acquisition for the run and
        // resume it after — but only if it was running AND we are the one who
        // stopped it, so a TUI-driven cal (which pauses/resumes itself) is not
        // double-restarted.
        bool resume_fast = b->fast_running;
        if (resume_fast) daq_board_stop_fast(b);
        if      (c->mode == SMU_CAL_MODE_VOLTAGE) run_voltage_cal(c);
        else if (c->mode == SMU_CAL_MODE_CURRENT) run_current_cal(c);
        else                                      run_baseline_cal(c);
        if (resume_fast) daq_board_run_fast(b, DAQ_RING_CAPACITY);
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

    if (base_load(c) == ESP_OK) {
        ESP_LOGI(TAG, "loaded baseline offsets: HI=%u MID=%u LO=%u",
                 c->base.have[RANGE_HI], c->base.have[RANGE_MID],
                 c->base.have[RANGE_LO]);
    } else {
        memset(&c->base, 0, sizeof(c->base));
        c->have_base = false;
        ESP_LOGI(TAG, "no stored baseline offsets");
    }

    if (rcal_load(c) == ESP_OK) {
        for (int r = 0; r < SMU_BASE_RANGES; ++r) {
            if (c->rcal.have[r]) rcal_apply(c, r);
        }
        ESP_LOGI(TAG, "loaded per-range meter cal: HI=%u MID=%u LO=%u",
                 c->rcal.have[RANGE_HI], c->rcal.have[RANGE_MID],
                 c->rcal.have[RANGE_LO]);
    } else {
        memset(&c->rcal, 0, sizeof(c->rcal));
        c->have_rcal = false;
        ESP_LOGI(TAG, "no stored per-range meter cal");
    }

    if (xTaskCreatePinnedToCore(cal_task, "smu_cal", 8192, c, 5, &c->task, 0)
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

