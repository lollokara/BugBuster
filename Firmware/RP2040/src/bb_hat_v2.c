// =============================================================================
// bb_hat_v2.c — BugBuster HAT v2 Feature Handlers
// =============================================================================

#include "bb_hat_v2.h"
#include "bb_config.h"
#include "bb_power.h"
#include "hardware/gpio.h"
#include "hardware/adc.h"
#include "hardware/i2c.h"
#include "hardware/pio.h"
#include "hardware/flash.h"
#include "hardware/sync.h"
#include "hardware/clocks.h"
#include "FreeRTOS.h"
#include "task.h"
#include "bb_ws2812.pio.h"
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include <stdio.h>

// DS4424 Registers & Constants
#define DS4424_REG_OUT0     0xF8
#define DS4424_REG_OUT1     0xF9
#define DS4424_REG_OUT2     0xFA
#define DS4424_I2C_ADDR     0x10
#define DS4424_CAL_MAX_POINTS 100

// Calibration Point definition
typedef struct {
    int8_t   dac_code;
    float    measured_v;
} DS4424CalPoint;

// Per-channel Calibration Data definition
typedef struct {
    DS4424CalPoint points[DS4424_CAL_MAX_POINTS];
    uint8_t        count;
    bool           valid;
} DS4424CalData;

// Flash Persistence layout
#define FLASH_CAL_OFFSET   (2048 * 1024 - 4096)
#define CAL_MAGIC          0xCA1B0002

typedef struct {
    uint32_t magic;
    uint32_t crc;
    uint8_t version;
    uint8_t padding[3];
    DS4424CalData cal[3];
} FlashCalSector;

// Helper function prototypes from bb_main.c
extern void send_response(uint8_t rsp_cmd, const uint8_t *payload, uint8_t len);
extern void send_ok(const uint8_t *payload, uint8_t len);
extern void send_error(uint8_t error_code);
extern uint16_t clamp_u16_from_float(float v);
extern void append_rail_status(uint8_t *rsp, size_t *p, uint8_t rail_id,
                               bool enabled, uint16_t mv, uint16_t ma,
                               uint8_t status);

// State Variables
static uint8_t s_led_states[BB_WS2812_COUNT][4]; // [r, g, b, mode]
static uint32_t s_ws2812_buffer[BB_WS2812_COUNT];
static int s_ws2812_sm = -1;
static PIO s_ws2812_pio = pio0;
static uint8_t s_la_route = HAT_LA_ROUTE_LOW_SPEED;

static bool s_ds4424_present = false;
static FlashCalSector s_flash_cal = {0};
static uint16_t s_io_voltage_mv = 3300;

// Calibration Background Task State
static volatile uint8_t s_cal_state = 0; // 0=idle, 1=running, 2=success, 3=failed
static volatile uint8_t s_cal_progress = 0;
static volatile uint8_t s_cal_rail_id = 0;
static volatile uint8_t s_cal_last_error = 0;
static TaskHandle_t s_cal_task_handle = NULL;

// -----------------------------------------------------------------------------
// WS2812B Driver
// -----------------------------------------------------------------------------
static void ws2812_put_pixel(uint32_t grb)
{
    if (s_ws2812_sm >= 0) {
        pio_sm_put_blocking(s_ws2812_pio, s_ws2812_sm, grb << 8);
    }
}

static void ws2812_update(void)
{
    for (int i = 0; i < BB_WS2812_COUNT; i++) {
        ws2812_put_pixel(s_ws2812_buffer[i]);
    }
    sleep_us(280); // Reset delay
}

static void ws2812_set_pixel(uint8_t index, uint8_t r, uint8_t g, uint8_t b)
{
    if (index < BB_WS2812_COUNT) {
        s_ws2812_buffer[index] = ((uint32_t)g << 16) | ((uint32_t)r << 8) | b;
    }
}

static void ws2812_init(void)
{
    // PIO0 is owned by debugprobe (SWD/SWCLK). PIO1 SM0 and SM1 are reserved
    // for the LA capture and trigger programs. SM2 is guaranteed free.
    // Bypass pio_claim_unused_sm to avoid depending on LA's claim state.
    s_ws2812_pio = pio1;
    if (pio_sm_is_claimed(pio1, 2)) {
        s_ws2812_sm = -1;
        return;
    }
    s_ws2812_sm = 2;
    pio_sm_claim(pio1, 2);
    if (!pio_can_add_program(pio1, &ws2812_program)) {
        pio_sm_unclaim(pio1, 2);
        s_ws2812_sm = -1;
        return;
    }
    uint offset = pio_add_program(pio1, &ws2812_program);
    ws2812_program_init(pio1, 2, offset, BB_WS2812_PIN, 800000.0f, false);
}

// -----------------------------------------------------------------------------
// DS4424 IDAC Driver & Calibration Persistence
// -----------------------------------------------------------------------------
static uint32_t calculate_crc(const uint8_t *data, size_t len)
{
    uint32_t crc = 0xFFFFFFFF;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int j = 0; j < 8; j++) {
            if (crc & 1) {
                crc = (crc >> 1) ^ 0xEDB88320;
            } else {
                crc >>= 1;
            }
        }
    }
    return ~crc;
}

static void flash_save(void)
{
    s_flash_cal.magic = CAL_MAGIC;
    s_flash_cal.version = 2;
    uint8_t *p = (uint8_t *)&s_flash_cal.version;
    size_t len = sizeof(FlashCalSector) - offsetof(FlashCalSector, version);
    s_flash_cal.crc = calculate_crc(p, len);

    uint32_t ints = save_and_disable_interrupts();
    flash_range_erase(FLASH_CAL_OFFSET, FLASH_SECTOR_SIZE);
    flash_range_program(FLASH_CAL_OFFSET, (const uint8_t *)&s_flash_cal, sizeof(FlashCalSector));
    restore_interrupts(ints);
}

static void flash_load(void)
{
    const FlashCalSector *flash_ptr = (const FlashCalSector *)(XIP_BASE + FLASH_CAL_OFFSET);
    if (flash_ptr->magic == CAL_MAGIC) {
        uint8_t *p = (uint8_t *)&flash_ptr->version;
        size_t len = sizeof(FlashCalSector) - offsetof(FlashCalSector, version);
        uint32_t crc = calculate_crc(p, len);
        if (crc == flash_ptr->crc) {
            memcpy(&s_flash_cal, flash_ptr, sizeof(FlashCalSector));
            return;
        }
    }
    memset(&s_flash_cal, 0, sizeof(FlashCalSector));
}

static int ds4424_write_raw(uint8_t reg, uint8_t value)
{
    uint8_t frame[2] = { reg, value };
    return i2c_write_timeout_us(
        BB_HVPAK_I2C, DS4424_I2C_ADDR, frame, sizeof(frame), false, BB_HVPAK_I2C_TIMEOUT_US
    );
}

static int ds4424_read_raw(uint8_t reg, uint8_t *value)
{
    int rc = i2c_write_timeout_us(
        BB_HVPAK_I2C, DS4424_I2C_ADDR, &reg, 1, true, BB_HVPAK_I2C_TIMEOUT_US
    );
    if (rc != 1) return rc;
    return i2c_read_timeout_us(
        BB_HVPAK_I2C, DS4424_I2C_ADDR, value, 1, false, BB_HVPAK_I2C_TIMEOUT_US
    );
}

static bool ds4424_set_code(uint8_t ch, int8_t code)
{
    if (ch >= 3) return false;
    uint8_t reg_addr = DS4424_REG_OUT0 + ch;
    uint8_t reg_val;
    if (code == 0) {
        reg_val = 0x00;
    } else if (code > 0) {
        reg_val = 0x80 | (uint8_t)code;
    } else {
        reg_val = (uint8_t)(-code);
    }

    for (int attempt = 0; attempt < 3; attempt++) {
        int rc = ds4424_write_raw(reg_addr, reg_val);
        if (rc != 2) {
            sleep_ms(1);
            continue;
        }

        uint8_t val = 0;
        rc = ds4424_read_raw(reg_addr, &val);
        if (rc != 1) {
            sleep_ms(1);
            continue;
        }

        int8_t readback;
        if (val & 0x80) {
            readback = (int8_t)(val & 0x7F);
        } else {
            readback = -(int8_t)(val & 0x7F);
        }

        if (readback == code) {
            return true;
        }
        sleep_ms(1);
    }
    return false;
}

static float ds4424_code_to_voltage(uint8_t ch, int8_t code)
{
    if (ch >= 3) return 0.0f;
    // ch 0 = 3V3_ADJ (TPS/LDO midpoint 3.3V)
    // ch 1/2 = VADJ3/VADJ4 (LTM8083 midpoint 18V, range 0–36V)
    float midpoint = (ch == 0) ? 3.3f : 18.0f;
    float r_int = 249.0f;
    float ifs = 50.0f;

    float i_dac = (float)abs(code) / 127.0f * ifs * 1e-6f;
    float dv = i_dac * r_int * 1000.0f;

    if (code < 0) {
        return midpoint + dv;
    } else {
        return midpoint - dv;
    }
}

static float ds4424_get_calibrated_voltage(uint8_t ch, int8_t code)
{
    DS4424CalData *cal = &s_flash_cal.cal[ch];
    if (cal->valid && cal->count >= 2) {
        if (code <= cal->points[0].dac_code) {
            return cal->points[0].measured_v;
        }
        if (code >= cal->points[cal->count - 1].dac_code) {
            return cal->points[cal->count - 1].measured_v;
        }
        for (int i = 0; i < (int)cal->count - 1; i++) {
            int8_t c0 = cal->points[i].dac_code;
            int8_t c1 = cal->points[i+1].dac_code;
            if ((code >= c0 && code <= c1) || (code >= c1 && code <= c0)) {
                if (c1 != c0) {
                    float t = (float)(code - c0) / (float)(c1 - c0);
                    return cal->points[i].measured_v + t * (cal->points[i+1].measured_v - cal->points[i].measured_v);
                }
            }
        }
    }
    return ds4424_code_to_voltage(ch, code);
}

static int8_t ds4424_voltage_to_code(uint8_t ch, float volts)
{
    DS4424CalData *cal = &s_flash_cal.cal[ch];
    if (cal->valid && cal->count >= 2) {
        for (int i = 0; i < (int)cal->count - 1; i++) {
            float v0 = cal->points[i].measured_v;
            float v1 = cal->points[i+1].measured_v;
            int8_t c0 = cal->points[i].dac_code;
            int8_t c1 = cal->points[i+1].dac_code;

            bool between = (v0 <= volts && volts <= v1) || (v1 <= volts && volts <= v0);
            if (between && v0 != v1) {
                float t = (volts - v0) / (v1 - v0);
                float code_f = (float)c0 + t * (float)(c1 - c0);
                int code_i = (code_f >= 0) ? (int)floorf(code_f) : (int)ceilf(code_f);
                if (code_i > 127) code_i = 127;
                if (code_i < -127) code_i = -127;
                return (int8_t)code_i;
            }
        }
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

    float midpoint = (ch == 0) ? 3.3f : 5.0f;
    float r_int = 249.0f;
    float ifs = 50.0f;
    float step_v = (ifs * 1e-6f * r_int * 1000.0f) / 127.0f;
    float dv = volts - midpoint;
    float code_f = -dv / step_v;
    int code_i = (int)roundf(code_f);
    if (code_i > 127) code_i = 127;
    if (code_i < -127) code_i = -127;
    return (int8_t)code_i;
}

static void ds4424_init(void)
{
    i2c_init(BB_HVPAK_I2C, BB_HVPAK_I2C_FREQ);
    gpio_set_function(BB_HVPAK_SDA_PIN, GPIO_FUNC_I2C);
    gpio_set_function(BB_HVPAK_SCL_PIN, GPIO_FUNC_I2C);
    gpio_pull_up(BB_HVPAK_SDA_PIN);
    gpio_pull_up(BB_HVPAK_SCL_PIN);

    s_ds4424_present = ds4424_set_code(0, 0);
    if (s_ds4424_present) {
        ds4424_set_code(1, 0);
        ds4424_set_code(2, 0);
    }
}

// -----------------------------------------------------------------------------
// ADC Oversampling
// -----------------------------------------------------------------------------
static int compare_u16(const void *a, const void *b)
{
    return (*(const uint16_t*)a - *(const uint16_t*)b);
}

static uint16_t adc_read_oversampled(uint channel)
{
    adc_select_input(channel);
    uint16_t samples[64];
    for (int i = 0; i < 64; i++) {
        samples[i] = adc_read();
    }
    qsort(samples, 64, sizeof(uint16_t), compare_u16);
    uint32_t sum = 0;
    for (int i = 24; i < 40; i++) {
        sum += samples[i];
    }
    return (uint16_t)(sum / 16);
}

// -----------------------------------------------------------------------------
// Auto-Calibration Sweep Task
// -----------------------------------------------------------------------------
static void cal_task_func(void *param)
{
    uint8_t rail_id = (uintptr_t)param;
    s_cal_rail_id = rail_id;
    s_cal_progress = 0;
    s_cal_last_error = 0;

    uint8_t dac_ch = (rail_id == HAT_RAIL_VADJ3) ? 1 : 2;
    uint8_t adc_ch = (rail_id == HAT_RAIL_VADJ3) ? 2 : 3;
    uint8_t en_pin = (rail_id == HAT_RAIL_VADJ3) ? BB_VADJ3_EN_PIN : BB_VADJ4_EN_PIN;

    // Refuse calibration with active shifted outputs — could damage a connected DUT
    if (gpio_get(BB_LEVEL_SHIFT_OE_PIN)) {
        s_cal_state = 3;
        s_cal_last_error = 3;
        s_cal_task_handle = NULL;
        vTaskDelete(NULL);
        return;
    }

    bool was_enabled = gpio_get(en_pin);
    gpio_put(en_pin, 1);
    vTaskDelay(pdMS_TO_TICKS(500)); // wait for rail to settle before sweep

    DS4424CalData *cal = &s_flash_cal.cal[rail_id];
    memset(cal, 0, sizeof(DS4424CalData));

    // Settle first at code 0
    ds4424_set_code(dac_ch, 0);
    vTaskDelay(pdMS_TO_TICKS(200));
    uint16_t adc_val = adc_read_oversampled(adc_ch);
    float v0 = (float)adc_val * 3.3f / 4095.0f * 12.0f;
    cal->points[0].dac_code = 0;
    cal->points[0].measured_v = v0;
    cal->count = 1;

    float max_volt = 35.0f;
    // Sweep sink direction (raising voltage)
    for (int code = -8; code >= -127; code -= 8) {
        int8_t code_i8 = (int8_t)code;
        if (!ds4424_set_code(dac_ch, code_i8)) {
            s_cal_state = 3;
            s_cal_last_error = 1;
            s_cal_task_handle = NULL;
            vTaskDelete(NULL);
            return;
        }
        vTaskDelay(pdMS_TO_TICKS(100));
        adc_val = adc_read_oversampled(adc_ch);
        float v = (float)adc_val * 3.3f / 4095.0f * 12.0f;

        cal->points[cal->count].dac_code = code_i8;
        cal->points[cal->count].measured_v = v;
        cal->count++;
        s_cal_progress = (uint8_t)(cal->count * 100 / 33);

        if (v >= max_volt) {
            break;
        }
    }

    // Settle back to 0
    ds4424_set_code(dac_ch, 0);
    vTaskDelay(pdMS_TO_TICKS(100));

    // Sweep source direction (lowering voltage)
    float min_volt = 0.5f;
    for (int code = 8; code <= 127; code += 8) {
        int8_t code_i8 = (int8_t)code;
        if (!ds4424_set_code(dac_ch, code_i8)) {
            s_cal_state = 3;
            s_cal_last_error = 1;
            s_cal_task_handle = NULL;
            vTaskDelete(NULL);
            return;
        }
        vTaskDelay(pdMS_TO_TICKS(100));
        adc_val = adc_read_oversampled(adc_ch);
        float v = (float)adc_val * 3.3f / 4095.0f * 12.0f;

        cal->points[cal->count].dac_code = code_i8;
        cal->points[cal->count].measured_v = v;
        cal->count++;
        s_cal_progress = (uint8_t)(cal->count * 100 / 33);

        if (v <= min_volt) {
            break;
        }
    }

    // Sort by dac_code
    for (int i = 0; i < cal->count - 1; i++) {
        for (int j = 0; j < cal->count - i - 1; j++) {
            if (cal->points[j].dac_code > cal->points[j+1].dac_code) {
                DS4424CalPoint temp = cal->points[j];
                cal->points[j] = cal->points[j+1];
                cal->points[j+1] = temp;
            }
        }
    }

    ds4424_set_code(dac_ch, 0);
    gpio_put(en_pin, was_enabled ? 1 : 0);
    cal->valid = (cal->count >= 2);

    if (cal->valid) {
        flash_save();
        s_cal_state = 2;
        s_cal_progress = 100;
    } else {
        s_cal_state = 3;
        s_cal_last_error = 2;
    }

    s_cal_task_handle = NULL;
    vTaskDelete(NULL);
}

// -----------------------------------------------------------------------------
// Color Code mapping helper
// -----------------------------------------------------------------------------
static void get_rgb_from_color_code(uint8_t color_code, uint8_t *r, uint8_t *g, uint8_t *b)
{
    switch (color_code) {
        case 0: *r = 0;   *g = 0;   *b = 0;   break; // Off
        case 1: *r = 255; *g = 0;   *b = 0;   break; // Red
        case 2: *r = 0;   *g = 255; *b = 0;   break; // Green
        case 3: *r = 0;   *g = 0;   *b = 255; break; // Blue
        case 4: *r = 255; *g = 255; *b = 0;   break; // Yellow
        case 5: *r = 0;   *g = 255; *b = 255; break; // Cyan
        case 6: *r = 255; *g = 0;   *b = 255; break; // Magenta
        case 7: *r = 255; *g = 255; *b = 255; break; // White
        default: *r = 255; *g = 127; *b = 0;   break; // Orange alert
    }
}

// -----------------------------------------------------------------------------
// Public APIs
// -----------------------------------------------------------------------------
void bb_hat_v2_init(void)
{
    memset(s_led_states, 0, sizeof(s_led_states));
    s_la_route = HAT_LA_ROUTE_LOW_SPEED;

    ws2812_init();
    ds4424_init();
    flash_load();

    // Boot animation: Green sweep 1->8
    for (int i = 0; i < BB_WS2812_COUNT; i++) {
        ws2812_set_pixel(i, 0, 255, 0);
        ws2812_update();
        sleep_ms(50);
        ws2812_set_pixel(i, 0, 0, 0);
        ws2812_update();
    }

    // Pulse all green
    for (int i = 0; i < BB_WS2812_COUNT; i++) {
        ws2812_set_pixel(i, 0, 255, 0);
    }
    ws2812_update();
    sleep_ms(200);

    for (int i = 0; i < BB_WS2812_COUNT; i++) {
        ws2812_set_pixel(i, 0, 0, 0);
    }
    ws2812_update();

    // Set LED 1 health OK (Green)
    ws2812_set_pixel(0, 0, 255, 0);
    ws2812_update();
}

void bb_hat_v2_handle_reset(void)
{
    memset(s_led_states, 0, sizeof(s_led_states));
    s_la_route = HAT_LA_ROUTE_LOW_SPEED;
    s_io_voltage_mv = 3300;

    // Turn off all rails & logic shifters
    gpio_put(BB_LEVEL_SHIFT_OE_PIN, 0);
    gpio_put(BB_LEVEL_SHIFT_DIR_PIN, 0);
    if (s_ds4424_present) {
        ds4424_set_code(0, 0);
        ds4424_set_code(1, 0);
        ds4424_set_code(2, 0);
    }

    // Clear pixel buffer, LED 1 green
    memset(s_ws2812_buffer, 0, sizeof(s_ws2812_buffer));
    ws2812_set_pixel(0, 0, 255, 0);
    ws2812_update();
}

uint8_t bb_hat_v2_get_la_route(void)
{
    return s_la_route;
}

void handle_get_caps(void)
{
    uint32_t flags = HAT_CAP_RAILS |
                     HAT_CAP_LEDS |
                     HAT_CAP_LA_LOW_SPEED |
                     HAT_CAP_SHIFTED_IO |
                     HAT_CAP_HVPAK_UNSUPPORTED;
    uint8_t rsp[12];
    size_t p = 0;

    rsp[p++] = 2; // HW Revision
    memcpy(&rsp[p], &flags, sizeof(flags)); p += sizeof(flags);
    rsp[p++] = HAT_RAIL_COUNT;
    rsp[p++] = BB_WS2812_COUNT;
    rsp[p++] = BB_NUM_HS_IO_PINS;
    rsp[p++] = (1u << HAT_LA_ROUTE_LOW_SPEED) | (1u << HAT_LA_ROUTE_HIGH_SPEED);
    rsp[p++] = BB_HAT_FW_MAJOR;
    rsp[p++] = BB_HAT_FW_MINOR;
    rsp[p++] = BB_HVPAK_PRESENT ? 1 : 0;

    send_response(HAT_RSP_CAPS, rsp, (uint8_t)p);
}

void handle_get_rail_status(void)
{
    // Read oversampled ADC channels
    uint16_t adc2 = adc_read_oversampled(2); // VADJ3 Sense
    uint16_t adc3 = adc_read_oversampled(3); // VADJ4 Sense
    uint16_t adc0 = adc_read_oversampled(0); // VADJ3 Current
    uint16_t adc1 = adc_read_oversampled(1); // VADJ4 Current

    float v3 = adc2 * 3.3f / 4095.0f * 12.0f * 1000.0f;
    float v4 = adc3 * 3.3f / 4095.0f * 12.0f * 1000.0f;

    float vismon3 = adc0 * 3.3f / 4095.0f * 1000.0f;
    float vismon4 = adc1 * 3.3f / 4095.0f * 1000.0f;

    float i3 = (vismon3 - 250.0f) / 0.5f;
    float i4 = (vismon4 - 250.0f) / 0.5f;
    if (i3 < 0.0f) i3 = 0.0f;
    if (i4 < 0.0f) i4 = 0.0f;

    uint8_t rsp[1 + HAT_RAIL_COUNT * 7];
    size_t p = 0;
    rsp[p++] = HAT_RAIL_COUNT;

    append_rail_status(rsp, &p, HAT_RAIL_3V3_ADJ,
                       bb_power_get_3v3_adj_enabled(), 0, 0,
                       s_flash_cal.cal[0].valid ? 1 : 0);

    append_rail_status(rsp, &p, HAT_RAIL_VADJ3,
                       bb_power_get_enabled(0),
                       clamp_u16_from_float(v3),
                       clamp_u16_from_float(i3),
                       s_flash_cal.cal[1].valid ? 1 : 0);

    append_rail_status(rsp, &p, HAT_RAIL_VADJ4,
                       bb_power_get_enabled(1),
                       clamp_u16_from_float(v4),
                       clamp_u16_from_float(i4),
                       s_flash_cal.cal[2].valid ? 1 : 0);

    send_response(HAT_RSP_RAIL_STATUS, rsp, (uint8_t)p);
}

void handle_set_rail_enable(const uint8_t *payload, uint8_t len)
{
    if (len < 2) { send_error(HAT_ERR_FRAME); return; }
    uint8_t rail = payload[0];
    bool enable = payload[1] != 0;

    switch (rail) {
    case HAT_RAIL_3V3_ADJ:
        bb_power_set_3v3_adj(enable);
        break;
    case HAT_RAIL_VADJ3:
        bb_power_set(0, enable);
        break;
    case HAT_RAIL_VADJ4:
        bb_power_set(1, enable);
        break;
    default:
        send_error(HAT_ERR_INVALID_PIN);
        return;
    }

    handle_get_rail_status();
}

void handle_set_led_state(const uint8_t *payload, uint8_t len)
{
    if (len < 2) { send_error(HAT_ERR_FRAME); return; }
    uint8_t led = payload[0];
    if (led < 1 || led > BB_WS2812_COUNT) {
        send_error(HAT_ERR_INVALID_PIN);
        return;
    }

    uint8_t r = 0, g = 0, b = 0, mode = 0;
    if (len >= 5) {
        // RGB+Mode payload: [led_id, r, g, b, mode]
        r = payload[1];
        g = payload[2];
        b = payload[3];
        mode = payload[4];
        s_led_states[led - 1][0] = r;
        s_led_states[led - 1][1] = g;
        s_led_states[led - 1][2] = b;
        s_led_states[led - 1][3] = mode;
    } else {
        // Color code payload: [led_id, color_code]
        uint8_t color_code = payload[1];
        get_rgb_from_color_code(color_code, &r, &g, &b);
        s_led_states[led - 1][0] = r;
        s_led_states[led - 1][1] = g;
        s_led_states[led - 1][2] = b;
        s_led_states[led - 1][3] = color_code;
    }

    ws2812_set_pixel(led - 1, r, g, b);
    ws2812_update();

    uint8_t rsp[2] = { led, payload[1] };
    send_ok(rsp, sizeof(rsp));
}

void handle_la_set_route(const uint8_t *payload, uint8_t len)
{
    if (len < 1) { send_error(HAT_ERR_FRAME); return; }
    uint8_t route = payload[0];

    if (route == HAT_LA_ROUTE_LOW_SPEED || route == HAT_LA_ROUTE_HIGH_SPEED) {
        s_la_route = route;
        send_ok(&s_la_route, 1);
        return;
    }
    send_error(HAT_ERR_INVALID_FUNC);
}

void handle_calibrate_start(const uint8_t *payload, uint8_t len)
{
    if (len < 1) { send_error(HAT_ERR_FRAME); return; }
    uint8_t rail_id = payload[0];
    if (rail_id != HAT_RAIL_VADJ3 && rail_id != HAT_RAIL_VADJ4) {
        send_error(HAT_ERR_INVALID_PIN);
        return;
    }

    if (s_cal_state == 1) {
        send_error(HAT_ERR_BUSY);
        return;
    }

    if (gpio_get(BB_LEVEL_SHIFT_OE_PIN)) {
        send_error(HAT_ERR_POWER_FAULT);
        return;
    }

    s_cal_state = 1;
    xTaskCreate(cal_task_func, "cal_task", 1024, (void*)(uintptr_t)rail_id, 1, &s_cal_task_handle);

    uint8_t status = s_cal_state;
    send_ok(&status, 1);
}

void handle_calibrate_status(void)
{
    uint8_t rsp[4];
    rsp[0] = s_cal_state;
    rsp[1] = s_cal_progress;
    rsp[2] = s_cal_rail_id;
    rsp[3] = s_cal_last_error;
    send_response(HAT_RSP_CALIBRATE_STATUS, rsp, sizeof(rsp));
}

void handle_calibrate_import(const uint8_t *payload, uint8_t len)
{
    if (len < 2) { send_error(HAT_ERR_FRAME); return; }
    uint8_t rail_id = payload[0];
    uint8_t count = payload[1];
    if (rail_id >= 3) { send_error(HAT_ERR_INVALID_PIN); return; }
    if (count > DS4424_CAL_MAX_POINTS) { send_error(HAT_ERR_INVALID_FUNC); return; }
    if (len < 2 + count * 5) { send_error(HAT_ERR_FRAME); return; }

    DS4424CalData *cal = &s_flash_cal.cal[rail_id];
    memset(cal, 0, sizeof(DS4424CalData));

    size_t pos = 2;
    for (int i = 0; i < count; i++) {
        cal->points[i].dac_code = (int8_t)payload[pos++];
        memcpy(&cal->points[i].measured_v, &payload[pos], 4);
        pos += 4;
    }
    cal->count = count;
    cal->valid = (count >= 2);

    flash_save();
    send_ok(NULL, 0);
}

void handle_set_io_bank(const uint8_t *payload, uint8_t len)
{
    if (len < 3) { send_error(HAT_ERR_FRAME); return; }
    uint8_t dirs = payload[0];
    uint8_t ups = payload[1];
    uint8_t dns = payload[2];

    const uint8_t pins[8] = { 10, 11, 12, 13, 14, 15, 20, 21 };
    for (int i = 0; i < 8; i++) {
        uint pin = pins[i];
        gpio_init(pin);
        if (dirs & (1 << i)) {
            gpio_set_dir(pin, GPIO_OUT);
        } else {
            gpio_set_dir(pin, GPIO_IN);
        }
        gpio_set_pulls(pin, (ups & (1 << i)) != 0, (dns & (1 << i)) != 0);
    }
    send_ok(NULL, 0);
}

void handle_set_level_shift(const uint8_t *payload, uint8_t len)
{
    if (len < 2) { send_error(HAT_ERR_FRAME); return; }
    bool oe = payload[0] != 0;
    bool dir = payload[1] != 0;

    if (oe && !bb_power_get_3v3_adj_enabled()) {
        send_error(HAT_ERR_POWER_FAULT);
        return;
    }

    gpio_put(BB_LEVEL_SHIFT_OE_PIN, oe ? 1 : 0);
    gpio_put(BB_LEVEL_SHIFT_DIR_PIN, dir ? 1 : 0);

    uint8_t rsp[2] = { oe ? 1 : 0, dir ? 1 : 0 };
    send_ok(rsp, sizeof(rsp));
}

bool bb_hat_v2_set_io_voltage(uint16_t mv)
{
    if (!s_ds4424_present) return false;
    float volts = (float)mv / 1000.0f;
    int8_t code = ds4424_voltage_to_code(0, volts);
    if (ds4424_set_code(0, code)) {
        s_io_voltage_mv = mv;
        return true;
    }
    return false;
}

uint16_t bb_hat_v2_get_io_voltage(void)
{
    return s_io_voltage_mv;
}

