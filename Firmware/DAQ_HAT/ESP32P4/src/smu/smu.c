// =============================================================================
// smu.c — programmable DUT supply (Source-Measure Unit) control.
// =============================================================================

#include "smu.h"
#include <string.h>
#include <math.h>
#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "config.h"

static const char *TAG = "smu";

// Ramp V_DUT one DS4424 code at a time on a direct jump rather than writing
// the target code in one shot. Confirmed on the bench (baseline-cal
// investigation): a single-shot jump to a code leaves the FINE current-sense
// path with ~200-400 uA of residual on HI range that does NOT decay over 20+
// seconds of waiting -- so it isn't a slow RC settle, it's history/path-
// dependent (self-heating or dielectric absorption in the sense path most
// likely). run_baseline_cal() (smu_cal.c) always reaches a code via a
// continuous one-step-at-a-time ramp with a settle delay, and only THAT path
// reads back near zero after baseline subtraction -- reproduced live by
// ramping vdut through intermediate codes by hand. Match that methodology
// here so every caller (CLI, settings, calibration's own stepper) gets a
// correctly-nulled baseline instead of only the calibration sweep itself.
#define SMU_VDUT_RAMP_STEP_DELAY_MS   10

// Clamp helper.
static float clampf(float x, float lo, float hi)
{
    return (x < lo) ? lo : (x > hi) ? hi : x;
}

// V_DUT = V0 - R_FB * I_DAC  ->  I_DAC = (V0 - V_DUT) / R_FB
// code  = (I_DAC / IFS) * 127 * polarity
int8_t smu_voltage_to_code(float volts)
{
    volts = clampf(volts, SMU_VDUT_MIN, SMU_VDUT_MAX);
    float i_dac_ua = ((SMU_VDUT_V0 - volts) / SMU_VDUT_RFB_OHM) * 1e6f;
    float code_f = (i_dac_ua / SMU_DS4424_IFS_UA) * 127.0f * (float)SMU_VDUT_CODE_POLARITY;
    int code = (int)lroundf(code_f);
    if (code > 127)  code = 127;
    if (code < -127) code = -127;
    return (int8_t)code;
}

// Current limit -> DS4424 ch0 code. code 0 = full-scale limit; larger sink codes
// reduce the limit. Linear placeholder pending bench calibration.
static int8_t current_limit_to_code(float amps)
{
    amps = clampf(amps, SMU_ILIMIT_MIN_A, SMU_ILIMIT_FULLSCALE_A);
    // Fraction of reduction from full scale -> magnitude of sink code.
    float reduce = (SMU_ILIMIT_FULLSCALE_A - amps) /
                   (SMU_ILIMIT_FULLSCALE_A - SMU_ILIMIT_MIN_A);
    int code = (int)lroundf(reduce * 127.0f) * SMU_ILIMIT_CODE_POLARITY;
    if (code > 127)  code = 127;
    if (code < -127) code = -127;
    return (int8_t)code;
}

esp_err_t smu_init(smu_t *s, ds4424_t *idac)
{
    memset(s, 0, sizeof(*s));
    s->idac = idac;

    // LTM8056 RUN pin: output, default OFF (V_DUT disabled at boot).
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << PWR_VDUT_RUN_PIN,
        .mode         = GPIO_MODE_OUTPUT,
    };
    gpio_config(&io);
    gpio_set_level(PWR_VDUT_RUN_PIN, 0);
    s->enabled = false;

    // ADC1 oneshot for IINMON / IOUTMON.
    adc_oneshot_unit_init_cfg_t unit_cfg = {
        .unit_id  = ADC_UNIT_1,
        .ulp_mode = ADC_ULP_MODE_DISABLE,
    };
    if (adc_oneshot_new_unit(&unit_cfg, &s->adc) == ESP_OK) {
        adc_oneshot_chan_cfg_t ch_cfg = {
            .atten    = ADC_ATTEN_DB_12,   // full input range
            .bitwidth = ADC_BITWIDTH_DEFAULT,
        };
        adc_oneshot_config_channel(s->adc, IINMON_ADC_CH, &ch_cfg);
        adc_oneshot_config_channel(s->adc, IOUTMON_ADC_CH, &ch_cfg);
        s->adc_ok = true;
    } else {
        ESP_LOGW(TAG, "ADC1 init failed; current monitors unavailable");
    }

    // Safe defaults: minimum voltage, full current limit (no write until set).
    s->vdut_set   = SMU_VDUT_MIN;
    s->ilimit_set = SMU_ILIMIT_FULLSCALE_A;
    return ESP_OK;
}

esp_err_t smu_enable(smu_t *s, bool on)
{
    bool was_on = s->enabled;

    // Coming from OFF: RUN gates the regulator entirely, so ramping the DAC
    // code while disabled (as smu_set_voltage() does) accomplishes nothing --
    // the regulator snaps straight to whatever code is already latched the
    // instant RUN asserts. Confirmed on the bench: enabling directly at the
    // target code leaves a large (~200-400 uA on HI range), non-decaying
    // residual after baseline subtraction; enabling then moving to the SAME
    // target via even a tiny (~2-code) live change reads back near zero.
    // A large absolute jump (e.g. to code 0) isn't needed and isn't safe --
    // code 0 corresponds to ~SMU_VDUT_V0 (10.85 V), an overshoot the DUT may
    // not tolerate. A small nudge a few codes off target and back, done
    // entirely after RUN asserts, is enough and stays close to vdut_set.
    if (on && !was_on && s->idac && s->idac->present) {
        int8_t target = s->v_code;
        int nudge = (target <= 0) ? 3 : -3;
        int8_t start = (int8_t)clampf((float)(target + nudge), -127.0f, 127.0f);
        ds4424_set_code(s->idac, DS4424_CH_VDUT, start);
        s->v_code = start;
    }

    gpio_set_level(PWR_VDUT_RUN_PIN, on ? 1 : 0);
    s->enabled = on;
    ESP_LOGI(TAG, "DUT supply %s", on ? "ON" : "OFF");

    if (on && !was_on) {
        vTaskDelay(pdMS_TO_TICKS(SMU_VDUT_RAMP_STEP_DELAY_MS));
        smu_set_voltage(s, s->vdut_set);   // ramp from the nudge back to target
    }
    return ESP_OK;
}

esp_err_t smu_set_voltage(smu_t *s, float volts)
{
    if (!s->idac || !s->idac->present) {
        return ESP_ERR_INVALID_STATE;
    }
    int8_t code;
    if (!(s->cal && smu_cal_voltage_to_code(s->cal, volts, &code))) {
        code = smu_voltage_to_code(volts);
    }

    // Ramp one code at a time toward the target instead of jumping directly
    // -- see SMU_VDUT_RAMP_STEP_DELAY_MS above.
    int8_t cur = s->v_code;
    esp_err_t err = ESP_OK;
    if (cur == code) {
        // Still write once even when the code is unchanged (e.g. right after
        // smu_enable()), matching the previous single-write behavior.
        err = ds4424_set_code(s->idac, DS4424_CH_VDUT, code);
    } else {
        int step = (code > cur) ? 1 : -1;
        while (cur != code) {
            cur = (int8_t)(cur + step);
            err = ds4424_set_code(s->idac, DS4424_CH_VDUT, cur);
            if (err != ESP_OK) break;
            s->v_code = cur;
            vTaskDelay(pdMS_TO_TICKS(SMU_VDUT_RAMP_STEP_DELAY_MS));
        }
    }
    if (err == ESP_OK) {
        s->v_code   = code;
        s->vdut_set = clampf(volts, SMU_VDUT_MIN, SMU_VDUT_MAX);
    }
    return err;
}

esp_err_t smu_set_current_limit(smu_t *s, float amps)
{
    if (!s->idac || !s->idac->present) {
        return ESP_ERR_INVALID_STATE;
    }
    int8_t code;
    if (!(s->cal && smu_cal_current_to_code(s->cal, amps, &code))) {
        code = current_limit_to_code(amps);
    }
    esp_err_t err = ds4424_set_code(s->idac, DS4424_CH_ILIMIT, code);
    if (err == ESP_OK) {
        s->i_code     = code;
        s->ilimit_set = clampf(amps, SMU_ILIMIT_MIN_A, SMU_ILIMIT_FULLSCALE_A);
    }
    return err;
}

void smu_set_cal(smu_t *s, const smu_cal_t *cal)
{
    s->cal = cal;
}

static esp_err_t read_mon(smu_t *s, adc_channel_t ch, float fs_v, float fs_a,
                          float *amps)
{
    if (!s->adc_ok) {
        return ESP_ERR_INVALID_STATE;
    }
    int raw = 0;
    esp_err_t err = adc_oneshot_read(s->adc, ch, &raw);
    if (err != ESP_OK) {
        return err;
    }
    // Approximate volts: 12-bit, 12 dB atten ~ 0..3.1 V full scale.
    float v = ((float)raw / 4095.0f) * 3.1f;
    *amps = (v / fs_v) * fs_a;
    return ESP_OK;
}

esp_err_t smu_read_input_current(smu_t *s, float *amps)
{
    return read_mon(s, IINMON_ADC_CH, SMU_IINMON_FS_V, SMU_IINMON_FS_A, amps);
}

esp_err_t smu_read_output_current(smu_t *s, float *amps)
{
    return read_mon(s, IOUTMON_ADC_CH, SMU_IOUTMON_FS_V, SMU_IOUTMON_FS_A, amps);
}
