// =============================================================================
// ds4424.cpp - DS4424 4-Channel I2C IDAC Driver
//
// Voltage formula: V_OUT = V_FB * (1 + R_INT/R_FB) + I_DAC * R_INT
//   - V_FB: internal feedback reference of the regulator
//   - R_INT: series resistor in FB network (249kΩ typical)
//   - R_FB: parallel resistor to GND (sets midpoint)
//   - I_DAC: signed current from DS4424 (sink raises V, source lowers V)
//            I_DAC = (|code|/127) * I_FS
//            I_FS determined by R_FS: R_FS = (V_RFS * 127) / (16 * I_FS)
//            where V_RFS = 0.976V
//
// DS4424 register format:
//   Bit 7: Sign (0 = sink current, 1 = source current)
//   Bits 6-0: Magnitude (0-127)
//
// Note on polarity: Sink current flows INTO the DS4424 pin, pulling the
// FB node voltage DOWN, which makes the regulator RAISE its output.
// Source current flows OUT, pushing FB UP, making the regulator LOWER output.
//
// VCC Safety: DS4424 OUT pin absolute max is VCC + 0.5V (= 3.8V with 3.3V VCC).
// The feedback divider network ensures the FB node stays at ~0.8V (midpoint),
// well below 3.8V across the entire DAC code range. No runtime clamp needed.
// =============================================================================

#include "ds4424.h"
#include "i2c_bus.h"
#include "config.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"

#include <string.h>
#include <math.h>

static const char *TAG = "ds4424";

// V_RFS constant from DS4424 datasheet
#define DS4424_VRFS  0.976f

// NVS namespace for calibration persistence
#define DS4424_NVS_NAMESPACE  "ds4424_cal"
#define DS4424_CAL_CHANNELS   3  // Only channels 0-2 have calibration

static DS4424State s_state = {};

// Forward declaration
static bool read_dac(uint8_t ch, int8_t *code);

// Default channel configurations
// I_FS = 50µA for all channels (as specified by user)
static void init_channel_configs(void)
{
    // IDAC0: Level shifter voltage (LTM8078 Out2)
    // V_FB=0.8V, R_INT=249kΩ, midpoint=3.3V
    // R_FB = R_INT / (V_mid/V_FB - 1) = 249 / (3.3/0.8 - 1) = 79.68kΩ
    s_state.config[0] = (DS4424ChanConfig){
        .v_fb = 0.8f,
        .r_int_kohm = 249.0f,
        .r_fb_kohm = 0.0f,  // computed below
        .midpoint_v = 3.3f,
        .ifs_ua = 50.0f,
        .rfs_kohm = 0.0f,   // computed below
        .v_min = 1.7f,
        .v_max = 5.2f,
    };

    // IDAC1: V_ADJ1 (LTM8063 #1, feeds P1+P2)
    // V_FB=0.774V, R_INT=249kΩ, midpoint=5.0V
    s_state.config[1] = (DS4424ChanConfig){
        .v_fb = 0.774f,
        .r_int_kohm = 249.0f,
        .r_fb_kohm = 0.0f,
        .midpoint_v = 5.0f,
        .ifs_ua = 50.0f,
        .rfs_kohm = 0.0f,
        .v_min = 3.0f,
        .v_max = 15.0f,
    };

    // IDAC2: V_ADJ2 (LTM8063 #2, feeds P3+P4)
    // Same topology as IDAC1
    s_state.config[2] = (DS4424ChanConfig){
        .v_fb = 0.774f,
        .r_int_kohm = 249.0f,
        .r_fb_kohm = 0.0f,
        .midpoint_v = 5.0f,
        .ifs_ua = 50.0f,
        .rfs_kohm = 0.0f,
        .v_min = 3.0f,
        .v_max = 15.0f,
    };

    // IDAC3: Not connected, but configure anyway
    s_state.config[3] = (DS4424ChanConfig){
        .v_fb = 0.8f,
        .r_int_kohm = 249.0f,
        .r_fb_kohm = 249.0f,
        .midpoint_v = 1.6f,
        .ifs_ua = 50.0f,
        .rfs_kohm = 0.0f,
        .v_min = 0.8f,
        .v_max = 3.3f,
    };

    // Compute derived values
    for (int i = 0; i < DS4424_NUM_CHANNELS; i++) {
        DS4424ChanConfig *c = &s_state.config[i];

        // R_FB = R_INT / (V_mid/V_FB - 1)
        float ratio = c->midpoint_v / c->v_fb;
        if (ratio > 1.0f) {
            c->r_fb_kohm = c->r_int_kohm / (ratio - 1.0f);
        } else {
            c->r_fb_kohm = c->r_int_kohm; // fallback
        }

        // R_FS = (V_RFS * 127) / (16 * I_FS)
        c->rfs_kohm = (DS4424_VRFS * 127.0f) / (16.0f * c->ifs_ua * 1e-6f) / 1000.0f;
    }
}

// Write a DAC code to a DS4424 channel register with read-back verification.
static bool write_dac(uint8_t ch, int8_t code)
{
    if (ch >= DS4424_NUM_CHANNELS) return false;

    uint8_t reg_addr = DS4424_REG_OUT0 + ch;
    uint8_t reg_val;

    if (code == 0) {
        reg_val = 0x00;
    } else if (code > 0) {
        // Source current: bit7=1, bits6-0 = magnitude
        reg_val = 0x80 | (uint8_t)code;
    } else {
        // Sink current: bit7=0, bits6-0 = magnitude
        reg_val = (uint8_t)(-code);
    }

    bool ok = false;
    for (int attempt = 0; attempt < 3; attempt++) {
        uint8_t buf[2] = { reg_addr, reg_val };
        if (!i2c_bus_write(DS4424_I2C_ADDR, buf, 2, 50)) {
            ESP_LOGW(TAG, "IDAC%d write failed (attempt %d)", ch, attempt + 1);
            delay_ms(1);
            continue;
        }

        // Read-back verify
        int8_t readback = 0;
        if (!read_dac(ch, &readback)) {
            ESP_LOGW(TAG, "IDAC%d readback failed (attempt %d)", ch, attempt + 1);
            delay_ms(1);
            continue;
        }

        if (readback == code) {
            if (attempt > 0) {
                ESP_LOGW(TAG, "IDAC%d write-verify succeeded on retry %d", ch, attempt);
            }
            ok = true;
            break;
        }
        ESP_LOGW(TAG, "IDAC%d write-verify mismatch: wrote=%d read=%d (attempt %d)",
                 ch, code, readback, attempt + 1);
        delay_ms(1);
    }

    if (!ok) {
        ESP_LOGE(TAG, "IDAC%d write-verify FAILED after 3 retries", ch);
        return false;
    }

    {
        s_state.state[ch].dac_code = code;
        // Use calibrated voltage if available, otherwise formula
        const DS4424CalData *cal = &s_state.cal[ch];
        if (cal->valid && cal->count >= 2) {
            // Interpolate from calibration points (sorted by dac_code)
            // Find two points bracketing this code
            float v = ds4424_code_to_voltage(ch, code); // fallback
            for (int i = 0; i < (int)cal->count - 1; i++) {
                int8_t c0 = cal->points[i].dac_code;
                int8_t c1 = cal->points[i + 1].dac_code;
                if ((code >= c0 && code <= c1) || (code >= c1 && code <= c0)) {
                    if (c1 != c0) {
                        float t = (float)(code - c0) / (float)(c1 - c0);
                        v = cal->points[i].measured_v + t * (cal->points[i + 1].measured_v - cal->points[i].measured_v);
                    }
                    break;
                }
            }
            // Outside calibrated code window: saturate to nearest measured edge.
            if (code <= cal->points[0].dac_code) {
                v = cal->points[0].measured_v;
            } else if (code >= cal->points[cal->count - 1].dac_code) {
                v = cal->points[cal->count - 1].measured_v;
            }
            s_state.state[ch].target_v = v;
        } else {
            s_state.state[ch].target_v = ds4424_code_to_voltage(ch, code);
        }
    }
    return ok;
}

// Read back the current DAC register value
static bool read_dac(uint8_t ch, int8_t *code)
{
    if (ch >= DS4424_NUM_CHANNELS) return false;

    uint8_t reg_addr = DS4424_REG_OUT0 + ch;
    uint8_t val = 0;
    bool ok = i2c_bus_write_read(DS4424_I2C_ADDR, &reg_addr, 1, &val, 1, 50);
    if (!ok) return false;

    if (val & 0x80) {
        // Source: positive code
        *code = (int8_t)(val & 0x7F);
    } else {
        // Sink: negative code
        *code = -(int8_t)(val & 0x7F);
    }
    return true;
}

bool ds4424_init(void)
{
    memset(&s_state, 0, sizeof(s_state));
    init_channel_configs();

    if (!i2c_bus_ready()) {
        ESP_LOGW(TAG, "I2C bus not ready");
        s_state.present = false;
        return false;
    }

    // Probe device
    s_state.present = i2c_bus_probe(DS4424_I2C_ADDR);
    if (!s_state.present) {
        ESP_LOGW(TAG, "DS4424 not found at 0x%02X", DS4424_I2C_ADDR);
        return false;
    }

    ESP_LOGI(TAG, "DS4424 found at 0x%02X", DS4424_I2C_ADDR);

    // Set all channels to zero current (safe start) — write_dac includes read-back verify
    for (uint8_t ch = 0; ch < DS4424_NUM_CHANNELS; ch++) {
        write_dac(ch, 0);
        s_state.state[ch].present = true;
    }

    // Post-init verification: confirm all channels actually read zero
    bool init_ok = true;
    for (uint8_t ch = 0; ch < DS4424_NUM_CHANNELS; ch++) {
        int8_t readback = 0;
        if (read_dac(ch, &readback)) {
            if (readback != 0) {
                ESP_LOGE(TAG, "IDAC%d init verify FAIL: expected 0, read %d", ch, readback);
                init_ok = false;
            }
        } else {
            ESP_LOGE(TAG, "IDAC%d init readback failed", ch);
            init_ok = false;
        }
    }
    if (!init_ok) {
        ESP_LOGE(TAG, "DS4424 init verification failed — outputs may not be at zero!");
    }

    ESP_LOGI(TAG, "All channels set to zero (midpoint voltages)");
    for (uint8_t ch = 0; ch < 3; ch++) {
        ESP_LOGI(TAG, "  IDAC%d: midpoint=%.2fV range=[%.2f, %.2f]V step=%.2fmV",
                 ch, s_state.config[ch].midpoint_v,
                 s_state.config[ch].v_min, s_state.config[ch].v_max,
                 ds4424_step_mv(ch));
    }

    // Load saved calibration data from NVS
    ds4424_cal_load();

    return true;
}

bool ds4424_present(void)
{
    return s_state.present;
}

const DS4424State* ds4424_get_state(void)
{
    return &s_state;
}

bool ds4424_set_code(uint8_t ch, int8_t code)
{
    if (ch >= DS4424_NUM_CHANNELS) return false;
    if (!s_state.present) return false;

    // Clamp the only out-of-range int8_t case that can still occur.
    if (code < -127) code = -127;

    return write_dac(ch, code);
}

int8_t ds4424_get_code(uint8_t ch)
{
    if (ch >= DS4424_NUM_CHANNELS) return 0;
    return s_state.state[ch].dac_code;
}

float ds4424_code_to_voltage(uint8_t ch, int8_t code)
{
    if (ch >= DS4424_NUM_CHANNELS) return 0.0f;
    const DS4424ChanConfig *c = &s_state.config[ch];

    // I_DAC magnitude in Amps
    float i_dac_a = (fabsf((float)code) / 127.0f) * c->ifs_ua * 1e-6f;

    // Midpoint voltage (DAC=0)
    float v_mid = c->midpoint_v;

    // Delta from IDAC: dV = I_DAC * R_INT
    float dv = i_dac_a * c->r_int_kohm * 1000.0f;  // R in ohms

    float v_out;
    if (code < 0) {
        // Sink (negative code): current flows into DS4424, pulls FB down → raises V_OUT
        v_out = v_mid + dv;
    } else if (code > 0) {
        // Source (positive code): current flows out, pushes FB up → lowers V_OUT
        v_out = v_mid - dv;
    } else {
        v_out = v_mid;
    }

    return v_out;
}

int8_t ds4424_voltage_to_code(uint8_t ch, float volts)
{
    if (ch >= DS4424_NUM_CHANNELS) return 0;

    // Check if calibration data is available and valid
    const DS4424CalData *cal = &s_state.cal[ch];
    if (cal->valid && cal->count >= 2) {
        // Use linear interpolation between calibration points
        // Points are sorted by dac_code
        // Find two points bracketing the target voltage
        for (int i = 0; i < (int)cal->count - 1; i++) {
            float v0 = cal->points[i].measured_v;
            float v1 = cal->points[i + 1].measured_v;
            int8_t c0 = cal->points[i].dac_code;
            int8_t c1 = cal->points[i + 1].dac_code;

            // Check if target is between these two points (either direction)
            bool between = (v0 <= volts && volts <= v1) || (v1 <= volts && volts <= v0);
            if (between && v0 != v1) {
                float t = (volts - v0) / (v1 - v0);
                float code_f = (float)c0 + t * (float)(c1 - c0);
                // Round towards lower voltage (safer: truncate towards zero)
                int code_i = (code_f >= 0) ? (int)floorf(code_f) : (int)ceilf(code_f);
                if (code_i > 127) code_i = 127;
                if (code_i < -127) code_i = -127;
                return (int8_t)code_i;
            }
        }
        // Outside calibrated voltage window: use nearest measured calibration point.
        int best_idx = 0;
        float best_err = fabsf(cal->points[0].measured_v - volts);
        for (int i = 1; i < (int)cal->count; i++) {
            float err = fabsf(cal->points[i].measured_v - volts);
            if (err < best_err) {
                best_err = err;
                best_idx = i;
            }
        }
        return cal->points[best_idx].dac_code;
    }

    // Fall back to formula-based calculation
    const DS4424ChanConfig *c = &s_state.config[ch];
    float v_mid = c->midpoint_v;
    float dv = volts - v_mid;

    // dv = -(code_sign) * |code|/127 * I_FS * R_INT
    // For sink (raise voltage): code < 0, dv > 0
    // For source (lower voltage): code > 0, dv < 0
    float r_int_ohm = c->r_int_kohm * 1000.0f;
    float ifs_a = c->ifs_ua * 1e-6f;
    float step_v = ifs_a * r_int_ohm / 127.0f;

    if (step_v < 1e-9f) return 0;

    // code = -dv / step_v_per_code  (negative because sink=raise, source=lower)
    float code_f = -dv / (ifs_a * r_int_ohm / 127.0f);
    int code_i = (int)roundf(code_f);
    if (code_i > 127) code_i = 127;
    if (code_i < -127) code_i = -127;

    return (int8_t)code_i;
}

bool ds4424_set_voltage(uint8_t ch, float volts)
{
    if (ch >= 3) return false;  // ch3 not connected
    if (!s_state.present) return false;

    const DS4424ChanConfig *c = &s_state.config[ch];

    // Safety: clamp to allowed range
    if (volts < c->v_min) volts = c->v_min;
    if (volts > c->v_max) volts = c->v_max;

    int8_t code = ds4424_voltage_to_code(ch, volts);

    // Verify the computed code won't exceed hardware limits
    float computed_v = ds4424_code_to_voltage(ch, code);
    if (computed_v < c->v_min || computed_v > c->v_max) {
        ESP_LOGW(TAG, "IDAC%d: computed voltage %.3fV out of range [%.1f, %.1f]",
                 ch, computed_v, c->v_min, c->v_max);
    }

    return write_dac(ch, code);
}

void ds4424_cal_add_point(uint8_t ch, int8_t dac_code, float measured_v)
{
    if (ch >= DS4424_NUM_CHANNELS) return;
    DS4424CalData *cal = &s_state.cal[ch];

    if (cal->count >= DS4424_CAL_MAX_POINTS) return;

    // Insert sorted by dac_code
    int insert_pos = cal->count;
    for (int i = 0; i < (int)cal->count; i++) {
        if (cal->points[i].dac_code == dac_code) {
            // Update existing point
            cal->points[i].measured_v = measured_v;
            if (cal->count >= 2) cal->valid = true;
            return;
        }
        if (cal->points[i].dac_code > dac_code) {
            insert_pos = i;
            break;
        }
    }

    // Shift elements to make room
    for (int i = (int)cal->count; i > insert_pos; i--) {
        cal->points[i] = cal->points[i - 1];
    }
    cal->points[insert_pos] = (DS4424CalPoint){ .dac_code = dac_code, .measured_v = measured_v };
    cal->count++;
    if (cal->count >= 2) cal->valid = true;
}

void ds4424_cal_clear(uint8_t ch)
{
    if (ch >= DS4424_NUM_CHANNELS) return;
    memset(&s_state.cal[ch], 0, sizeof(DS4424CalData));
}

int ds4424_cal_auto(uint8_t ch, float (*read_adc)(uint8_t ch), uint8_t step_size, uint32_t settle_ms)
{
    if (ch >= 3 || !read_adc || !s_state.present) return 0;
    if (step_size < 1) step_size = 8;
    if (step_size > 127) step_size = 127;

    const DS4424ChanConfig *c = &s_state.config[ch];
    ds4424_cal_clear(ch);
    const int step = (int)step_size;

    ESP_LOGI(TAG, "IDAC%d: Starting auto-calibration (step=%d, settle=%lums)",
             ch, step_size, (unsigned long)settle_ms);

    // Always start at 0 (midpoint)
    write_dac(ch, 0);
    vTaskDelay(pdMS_TO_TICKS(settle_ms * 2));  // Extra settle for first point
    float v0 = read_adc(ch);
    ds4424_cal_add_point(ch, 0, v0);
    ESP_LOGI(TAG, "  DAC=0 → %.4fV (midpoint)", v0);

    int point_count = 1;

    // Sweep sink direction (negative codes → raise voltage)
    // This goes UP in voltage. Stop if we hit v_max or 12V ADC limit
    float cal_max = (c->v_max <= 12.0f) ? c->v_max : 12.0f;
    for (int code = -step; code >= -127; code -= step) {
        int8_t code_i8 = (int8_t)code;
        write_dac(ch, code_i8);
        vTaskDelay(pdMS_TO_TICKS(settle_ms));
        float v = read_adc(ch);
        ds4424_cal_add_point(ch, code_i8, v);
        point_count++;
        ESP_LOGI(TAG, "  DAC=%d → %.4fV", code_i8, v);

        if (v >= cal_max) {
            ESP_LOGI(TAG, "  Reached cal max %.2fV at code=%d", cal_max, code_i8);
            break;
        }
    }

    // Return to 0 before sweeping source direction
    write_dac(ch, 0);
    vTaskDelay(pdMS_TO_TICKS(settle_ms));

    // Sweep source direction (positive codes → lower voltage)
    for (int code = step; code <= 127; code += step) {
        int8_t code_i8 = (int8_t)code;
        write_dac(ch, code_i8);
        vTaskDelay(pdMS_TO_TICKS(settle_ms));
        float v = read_adc(ch);
        ds4424_cal_add_point(ch, code_i8, v);
        point_count++;
        ESP_LOGI(TAG, "  DAC=%d → %.4fV", code_i8, v);

        if (v <= c->v_min) {
            ESP_LOGI(TAG, "  Reached v_min %.2fV at code=%d", c->v_min, code_i8);
            break;
        }
    }

    // Return to 0 (safe state)
    write_dac(ch, 0);
    ESP_LOGI(TAG, "IDAC%d: Calibration complete, %d points", ch, point_count);

    return point_count;
}

bool ds4424_cal_save(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(DS4424_NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS open failed: %s", esp_err_to_name(err));
        return false;
    }

    bool all_ok = true;
    for (int ch = 0; ch < DS4424_CAL_CHANNELS; ch++) {
        char key[12];
        snprintf(key, sizeof(key), "cal_ch%d", ch);

        const DS4424CalData *cal = &s_state.cal[ch];
        err = nvs_set_blob(h, key, cal, sizeof(DS4424CalData));
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "NVS write ch%d failed: %s", ch, esp_err_to_name(err));
            all_ok = false;
        }
    }

    err = nvs_commit(h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS commit failed: %s", esp_err_to_name(err));
        all_ok = false;
    }

    nvs_close(h);

    if (all_ok) {
        ESP_LOGI(TAG, "Calibration data saved to NVS");
    }
    return all_ok;
}

bool ds4424_cal_load(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(DS4424_NVS_NAMESPACE, NVS_READONLY, &h);
    if (err != ESP_OK) {
        ESP_LOGD(TAG, "NVS open for read failed: %s (no saved calibration)", esp_err_to_name(err));
        return false;
    }

    bool any_loaded = false;
    for (int ch = 0; ch < DS4424_CAL_CHANNELS; ch++) {
        char key[12];
        snprintf(key, sizeof(key), "cal_ch%d", ch);

        size_t len = sizeof(DS4424CalData);
        err = nvs_get_blob(h, key, &s_state.cal[ch], &len);
        if (err == ESP_OK && len == sizeof(DS4424CalData)) {
            if (s_state.cal[ch].valid && s_state.cal[ch].count >= 2) {
                ESP_LOGI(TAG, "IDAC%d: Loaded %d calibration points from NVS",
                         ch, s_state.cal[ch].count);
                any_loaded = true;
            } else {
                // Data was read but is not valid/usable, clear it
                memset(&s_state.cal[ch], 0, sizeof(DS4424CalData));
            }
        } else {
            ESP_LOGD(TAG, "IDAC%d: No saved calibration", ch);
        }
    }

    nvs_close(h);
    return any_loaded;
}

const DS4424ChanConfig* ds4424_get_config(uint8_t ch)
{
    if (ch >= DS4424_NUM_CHANNELS) return NULL;
    return &s_state.config[ch];
}

float ds4424_step_mv(uint8_t ch)
{
    if (ch >= DS4424_NUM_CHANNELS) return 0.0f;
    const DS4424ChanConfig *c = &s_state.config[ch];
    // Step = I_FS/127 * R_INT
    return (c->ifs_ua * 1e-6f / 127.0f) * c->r_int_kohm * 1e6f;  // result in mV
}

void ds4424_get_range(uint8_t ch, float *v_min, float *v_max)
{
    if (ch >= DS4424_NUM_CHANNELS) return;
    const DS4424ChanConfig *c = &s_state.config[ch];

    float ifs_a = c->ifs_ua * 1e-6f;
    float r_int_ohm = c->r_int_kohm * 1000.0f;
    float delta = ifs_a * r_int_ohm;

    float low = c->midpoint_v - delta;
    float high = c->midpoint_v + delta;

    // Clamp to regulator limits
    if (v_min) *v_min = (low < c->v_min) ? c->v_min : low;
    if (v_max) *v_max = (high > c->v_max) ? c->v_max : high;
}
