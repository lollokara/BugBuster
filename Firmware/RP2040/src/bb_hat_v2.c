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
#include "pico/error.h"
#include "pico/flash.h"
#include "FreeRTOS.h"
#include "task.h"
#include "semphr.h"
#include "bb_la_usb.h"
#include "bb_ws2812.pio.h"
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include <stdio.h>
#include <stddef.h>

// DS4424 Registers & Constants
#define DS4424_REG_OUT0     0xF8
#define DS4424_REG_OUT1     0xF9
#define DS4424_REG_OUT2     0xFA
#define DS4424_I2C_ADDR     0x10
#define DS4424_CAL_MAX_POINTS 169

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
#define FLASH_CAL_LEGACY_OFFSET   (2048 * 1024 - 4096)
#define FLASH_CAL_SLOT_A_OFFSET   (2048 * 1024 - 8192)
#define FLASH_CAL_SLOT_B_OFFSET   (2048 * 1024 - 4096)
#define CAL_MAGIC                 0xCA1B0002
#define CAL_JOURNAL_MAGIC         0xCA1B0003
#define CAL_JOURNAL_VERSION       3

typedef struct {
    uint32_t magic;
    uint32_t crc;
    uint8_t version;
    uint8_t padding[3];
    DS4424CalData cal[3];
} FlashCalSector;

typedef struct {
    uint32_t magic;
    uint32_t crc;
    uint32_t sequence;
    uint16_t payload_len;
    uint8_t version;
    uint8_t reserved;
    FlashCalSector payload;
} FlashCalJournalSector;

typedef enum {
    HAT_PERSIST_CLEAN = 0,
    HAT_PERSIST_PENDING = 1,
    HAT_PERSIST_SAVING = 2,
    HAT_PERSIST_FAILED = 3,
} HatPersistState;

// Helper function prototypes from bb_main.c
extern void send_response(uint8_t rsp_cmd, const uint8_t *payload, uint8_t len);
extern void send_ok(const uint8_t *payload, uint8_t len);
extern void send_error(uint8_t error_code);
extern void bb_la_log(const char *fmt, ...);
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
static SemaphoreHandle_t s_persist_mutex = NULL;
static TaskHandle_t s_persist_task_handle = NULL;
static FlashCalSector s_persist_snapshot = {0};
static volatile HatPersistState s_persist_state = HAT_PERSIST_CLEAN;
static volatile bool s_persist_pending = false;
static uint32_t s_persist_request_id = 0;
static uint32_t s_flash_sequence = 0;
// The journal sector must fit in one flash sector.  Verified here at compile
// time so any future struct growth is caught immediately.
_Static_assert(sizeof(FlashCalJournalSector) <= FLASH_SECTOR_SIZE,
               "FlashCalJournalSector exceeds FLASH_SECTOR_SIZE — reduce DS4424_CAL_MAX_POINTS");

// Write buffer must hold the journal record.  Size it explicitly so GCC
// -Warray-bounds does not emit a false positive when sizeof(record) > 4096
// (which cannot happen given the assert above, but the compiler doesn't know).
#define FLASH_WRITE_BUF_SIZE  \
    ((sizeof(FlashCalJournalSector) > FLASH_SECTOR_SIZE) \
        ? sizeof(FlashCalJournalSector) : FLASH_SECTOR_SIZE)
static uint8_t s_flash_write_buf[FLASH_WRITE_BUF_SIZE];

// Calibration Background Task State
static volatile uint8_t s_cal_state = 0; // 0=idle, 1=running, 2=success, 3=failed
static volatile uint8_t s_cal_progress = 0;
static volatile uint8_t s_cal_rail_id = 0;
static volatile uint8_t s_cal_last_error = 0;
static volatile uint8_t s_cal_stage = 0;
static volatile uint8_t s_cal_point = 0;
static volatile int8_t  s_cal_code = 0;
static volatile int32_t s_cal_measured_mv = -1;
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
    // pio_sm_put_blocking() returns once the FIFO accepts the word, NOT when
    // the PIO finishes transmitting it.  With an 8-entry joined TX FIFO and
    // 8 pixels, up to ~210µs of bits may still be in flight when we return.
    // Drain the FIFO first so the PIO is down to its last pixel, then
    // wait 350µs (≈30µs last-pixel tail + 300µs WS2812B reset threshold).
    // busy_wait_us_32 is a pure spin loop — no WFE, safe inside FreeRTOS tasks.
    if (s_ws2812_sm >= 0) {
        while (!pio_sm_is_tx_fifo_empty(s_ws2812_pio, (uint)s_ws2812_sm))
            tight_loop_contents();
    }
    busy_wait_us_32(350);
}

static void ws2812_set_pixel(uint8_t index, uint8_t r, uint8_t g, uint8_t b)
{
    if (index < BB_WS2812_COUNT) {
        s_ws2812_buffer[index] = ((uint32_t)g << 16) | ((uint32_t)r << 8) | b;
    }
}

static void ws2812_init(void)
{
    // Idempotent: if SM2 was already claimed and initialized by a previous
    // call, do not re-claim or re-add the program.  Without this guard a
    // second call would see pio_sm_is_claimed()==true and set s_ws2812_sm=-1,
    // permanently disabling the driver for the lifetime of this boot.
    if (s_ws2812_sm >= 0) return;

    // PIO0 is owned by debugprobe (SWD/SWCLK). PIO1 SM0 and SM1 are reserved
    // for the LA capture and trigger programs. SM2 is guaranteed free.
    // Bypass pio_claim_unused_sm to avoid depending on LA's claim state.
    s_ws2812_pio = pio1;
    if (pio_sm_is_claimed(pio1, 2)) {
        // SM2 claimed by something else — driver stays disabled.
        return; // s_ws2812_sm remains -1
    }
    pio_sm_claim(pio1, 2);
    if (!pio_can_add_program(pio1, &ws2812_program)) {
        pio_sm_unclaim(pio1, 2);
        return; // s_ws2812_sm remains -1
    }
    uint offset = pio_add_program(pio1, &ws2812_program);
    ws2812_program_init(pio1, 2, offset, BB_WS2812_PIN, 800000.0f, false);
    s_ws2812_sm = 2; // mark as live only after full init succeeds
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

static void flash_prepare_payload(FlashCalSector *sector)
{
    sector->magic = CAL_MAGIC;
    sector->version = 2;
    uint8_t *p = (uint8_t *)&sector->version;
    size_t len = sizeof(FlashCalSector) - offsetof(FlashCalSector, version);
    sector->crc = calculate_crc(p, len);
}

static bool flash_payload_valid(const FlashCalSector *sector)
{
    if (sector->magic != CAL_MAGIC) return false;
    const uint8_t *p = (const uint8_t *)&sector->version;
    size_t len = sizeof(FlashCalSector) - offsetof(FlashCalSector, version);
    return calculate_crc(p, len) == sector->crc;
}

static bool flash_journal_valid(const FlashCalJournalSector *slot)
{
    if (slot->magic != CAL_JOURNAL_MAGIC) return false;
    if (slot->version != CAL_JOURNAL_VERSION) return false;
    if (slot->payload_len != sizeof(FlashCalSector)) return false;

    const uint8_t *p = (const uint8_t *)&slot->sequence;
    size_t len = offsetof(FlashCalJournalSector, payload) - offsetof(FlashCalJournalSector, sequence);
    len += slot->payload_len;
    if (calculate_crc(p, len) != slot->crc) return false;

    return flash_payload_valid(&slot->payload);
}

typedef struct {
    uint32_t offset;
    const uint8_t *data;
} FlashWriteOp;

static void flash_write_callback(void *param)
{
    FlashWriteOp *op = (FlashWriteOp *)param;
    flash_range_erase(op->offset, FLASH_SECTOR_SIZE);
    flash_range_program(op->offset, op->data, FLASH_SECTOR_SIZE);
}

static void persist_task_func(void *param)
{
    (void)param;

    for (;;) {
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(250));

        if (!s_persist_pending) {
            continue;
        }

        if (bb_la_usb_is_streaming() || bb_la_usb_has_pending_data()) {
            s_persist_state = HAT_PERSIST_PENDING;
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        static FlashCalSector s_write_snapshot;
        uint32_t request_id = 0;
        if (s_persist_mutex && xSemaphoreTake(s_persist_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
            memcpy(&s_write_snapshot, &s_persist_snapshot, sizeof(s_write_snapshot));
            request_id = s_persist_request_id;
            xSemaphoreGive(s_persist_mutex);
        } else {
            s_persist_state = HAT_PERSIST_FAILED;
            continue;
        }

        s_persist_state = HAT_PERSIST_SAVING;
        uint32_t sequence = s_flash_sequence + 1;
        uint32_t offset = (sequence & 1u) ? FLASH_CAL_SLOT_A_OFFSET : FLASH_CAL_SLOT_B_OFFSET;

        FlashCalJournalSector *record = (FlashCalJournalSector *)s_flash_write_buf;
        memset(s_flash_write_buf, 0xFF, sizeof(s_flash_write_buf));
        record->magic = CAL_JOURNAL_MAGIC;
        record->sequence = sequence;
        record->payload_len = sizeof(FlashCalSector);
        record->version = CAL_JOURNAL_VERSION;
        record->reserved = 0xFF;
        memcpy(&record->payload, &s_write_snapshot, sizeof(s_write_snapshot));

        const uint8_t *crc_start = (const uint8_t *)&record->sequence;
        size_t crc_len = offsetof(FlashCalJournalSector, payload) - offsetof(FlashCalJournalSector, sequence);
        crc_len += record->payload_len;
        record->crc = calculate_crc(crc_start, crc_len);

        FlashWriteOp op = {
            .offset = offset,
            .data = s_flash_write_buf,
        };

        int rc = flash_safe_execute(flash_write_callback, &op, 1000);
        if (rc == PICO_OK) {
            s_flash_sequence = sequence;
            bool more_pending = false;
            if (s_persist_mutex && xSemaphoreTake(s_persist_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
                more_pending = s_persist_pending && s_persist_request_id != request_id;
                if (!more_pending) {
                    s_persist_pending = false;
                }
                xSemaphoreGive(s_persist_mutex);
            }
            s_persist_state = more_pending ? HAT_PERSIST_PENDING : HAT_PERSIST_CLEAN;
            bb_la_log("[CAL] flash save complete seq=%lu slot=%c\n",
                      (unsigned long)sequence, (offset == FLASH_CAL_SLOT_A_OFFSET) ? 'A' : 'B');
            if (more_pending) {
                xTaskNotifyGive(s_persist_task_handle);
            }
        } else {
            bool more_pending = false;
            if (s_persist_mutex && xSemaphoreTake(s_persist_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
                more_pending = s_persist_pending && s_persist_request_id != request_id;
                if (!more_pending) {
                    s_persist_pending = false;
                }
                xSemaphoreGive(s_persist_mutex);
            }
            s_persist_state = more_pending ? HAT_PERSIST_PENDING : HAT_PERSIST_FAILED;
            bb_la_log("[CAL] flash save failed rc=%d\n", rc);
            if (more_pending) {
                xTaskNotifyGive(s_persist_task_handle);
            }
        }
    }
}

static bool flash_save(void)
{
    flash_prepare_payload(&s_flash_cal);

    if (!s_persist_mutex || !s_persist_task_handle) {
        s_persist_state = HAT_PERSIST_FAILED;
        return false;
    }

    if (xSemaphoreTake(s_persist_mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        s_persist_state = HAT_PERSIST_FAILED;
        return false;
    }
    memcpy(&s_persist_snapshot, &s_flash_cal, sizeof(s_persist_snapshot));
    s_persist_request_id++;
    s_persist_pending = true;
    s_persist_state = HAT_PERSIST_PENDING;
    xSemaphoreGive(s_persist_mutex);

    xTaskNotifyGive(s_persist_task_handle);
    return true;
}

static void flash_load(void)
{
    const FlashCalJournalSector *slot_a =
        (const FlashCalJournalSector *)(XIP_BASE + FLASH_CAL_SLOT_A_OFFSET);
    const FlashCalJournalSector *slot_b =
        (const FlashCalJournalSector *)(XIP_BASE + FLASH_CAL_SLOT_B_OFFSET);

    bool a_valid = flash_journal_valid(slot_a);
    bool b_valid = flash_journal_valid(slot_b);

    if (a_valid || b_valid) {
        const FlashCalJournalSector *chosen = slot_a;
        if (b_valid && (!a_valid || slot_b->sequence > slot_a->sequence)) {
            chosen = slot_b;
        }
        memcpy(&s_flash_cal, &chosen->payload, sizeof(s_flash_cal));
        s_flash_sequence = chosen->sequence;
        return;
    }

    const FlashCalSector *legacy = (const FlashCalSector *)(XIP_BASE + FLASH_CAL_LEGACY_OFFSET);
    if (flash_payload_valid(legacy)) {
        memcpy(&s_flash_cal, legacy, sizeof(s_flash_cal));
        s_flash_sequence = 0;
        return;
    }

    memset(&s_flash_cal, 0, sizeof(s_flash_cal));
    s_flash_sequence = 0;
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
        // Compact single-line summary to avoid log truncation
        bb_la_log("[DEBUG] vtc ch=%d tgt=%.3f cnt=%d lo=%.3f(c%d) hi=%.3f(c%d)\n",
                  ch, volts, (int)cal->count,
                  cal->points[cal->count - 1].measured_v, cal->points[cal->count - 1].dac_code,
                  cal->points[0].measured_v, cal->points[0].dac_code);

        // Search for the two calibration points whose measured voltages best
        // bracket the target on the voltage axis.  The table is sorted by
        // dac_code, NOT by voltage, so adjacent dac_code entries can have
        // non-monotonic voltages near the sink/source sweep join (~18 V).
        // Scanning by voltage avoids picking the wrong bracket.
        int best_lo = -1;   // index of highest measured_v <= volts
        int best_hi = -1;   // index of lowest  measured_v >= volts
        float best_lo_v = -1e9f;
        float best_hi_v =  1e9f;

        for (int i = 0; i < (int)cal->count; i++) {
            float v = cal->points[i].measured_v;
            if (v <= volts && v > best_lo_v) { best_lo_v = v; best_lo = i; }
            if (v >= volts && v < best_hi_v) { best_hi_v = v; best_hi = i; }
        }

        if (best_lo >= 0 && best_hi >= 0 && best_lo_v != best_hi_v) {
            int8_t c0 = cal->points[best_lo].dac_code;
            int8_t c1 = cal->points[best_hi].dac_code;
            float t = (volts - best_lo_v) / (best_hi_v - best_lo_v);
            float code_f = (float)c0 + t * (float)(c1 - c0);
            int code_i = (int)roundf(code_f);
            if (code_i >  127) code_i =  127;
            if (code_i < -127) code_i = -127;
            bb_la_log("[DEBUG] vtc->interp lo=%.3f(c%d) hi=%.3f(c%d) t=%.3f code=%d\n",
                      best_lo_v, c0, best_hi_v, c1, t, code_i);
            return (int8_t)code_i;
        }

        // Target is outside the calibrated range — clamp to nearest endpoint
        int best_idx = 0;
        float best_err = fabsf(cal->points[0].measured_v - volts);
        for (int i = 1; i < (int)cal->count; i++) {
            float err = fabsf(cal->points[i].measured_v - volts);
            if (err < best_err) { best_err = err; best_idx = i; }
        }
        bb_la_log("[DEBUG] vtc->nearest idx=%d code=%d v=%.3f err=%.3f\n",
                  best_idx, cal->points[best_idx].dac_code,
                  cal->points[best_idx].measured_v, best_err);
        return cal->points[best_idx].dac_code;
    }

    float midpoint = (ch == 0) ? 3.3f : 18.0f;
    float r_int = 249.0f;
    float ifs = 50.0f;
    float step_v = (ifs * 1e-6f * r_int * 1000.0f) / 127.0f;
    float dv = volts - midpoint;
    float code_f = -dv / step_v;
    int code_i = (int)roundf(code_f);
    if (code_i >  127) code_i =  127;
    if (code_i < -127) code_i = -127;
    bb_la_log("[DEBUG] vtc->theory ch=%d tgt=%.3f code=%d\n", ch, volts, code_i);
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

static float measure_supply_stable_for_cal(uint adc_ch)
{
    float samples[5] = {0};
    int sample_idx = 0;
    int samples_collected = 0;

    for (int step = 0; step < 100; step++) {
        uint16_t adc_val = adc_read_oversampled(adc_ch);
        float v = (float)adc_val * 3.3f / 4095.0f * 12.0f;

        s_cal_measured_mv = (int32_t)(v * 1000.0f);

        samples[sample_idx] = v;
        sample_idx = (sample_idx + 1) % 5;
        if (samples_collected < 5) {
            samples_collected++;
        }

        if (samples_collected >= 5 && step >= 5) {
            float min_v = samples[0];
            float max_v = samples[0];
            for (int i = 1; i < 5; i++) {
                if (samples[i] < min_v) min_v = samples[i];
                if (samples[i] > max_v) max_v = samples[i];
            }
            float dev = max_v - min_v;
            if (dev < 0.030f) {
                float sum = 0.0f;
                for (int i = 0; i < 5; i++) sum += samples[i];
                return sum / 5.0f;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(15));
    }

    float sum = 0.0f;
    for (int i = 0; i < 5; i++) sum += samples[i];
    return sum / 5.0f;
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
    s_cal_stage = 1;
    s_cal_point = 0;
    s_cal_code = 0;
    s_cal_measured_mv = -1;

    uint8_t dac_ch = (rail_id == HAT_RAIL_VADJ3) ? 1 : 2;
    uint8_t adc_ch = (rail_id == HAT_RAIL_VADJ3) ? 2 : 3;
    uint8_t en_pin = (rail_id == HAT_RAIL_VADJ3) ? BB_VADJ3_EN_PIN : BB_VADJ4_EN_PIN;
    const char *rail_name = (rail_id == HAT_RAIL_VADJ3) ? "VADJ3" : "VADJ4";

    bb_la_log("[CAL] start %s\n", rail_name);

    // Refuse calibration with active shifted outputs — could damage a connected DUT
    if (gpio_get(BB_LEVEL_SHIFT_OE_PIN)) {
        s_cal_state = 3;
        s_cal_last_error = 3;
        s_cal_task_handle = NULL;
        bb_la_log("[CAL] refused %s: level shifter OE active\n", rail_name);
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
    s_cal_stage = 3;
    float v0 = measure_supply_stable_for_cal(adc_ch);
    cal->points[0].dac_code = 0;
    cal->points[0].measured_v = v0;
    cal->count = 1;
    s_cal_progress = 1;
    s_cal_point = 0;
    s_cal_code = 0;
    s_cal_measured_mv = (int32_t)(v0 * 1000.0f);
    s_cal_stage = 4;
    bb_la_log("[CAL] %s code=0 measured=%dmV\n", rail_name, (int)(v0 * 1000.0f));

    float max_volt = 35.0f;
    // Sweep sink direction (raising voltage)
    for (int code = -1; code >= -127; code -= 1) {
        int8_t code_i8 = (int8_t)code;
        s_cal_stage = 2;
        s_cal_point = cal->count;
        s_cal_code = code_i8;
        s_cal_measured_mv = -1;
        if (!ds4424_set_code(dac_ch, code_i8)) {
            s_cal_state = 3;
            s_cal_last_error = 1;
            s_cal_task_handle = NULL;
            bb_la_log("[CAL] failed %s: DS4424 write code=%d\n", rail_name, code_i8);
            vTaskDelete(NULL);
            return;
        }
        s_cal_stage = 3;
        float v = measure_supply_stable_for_cal(adc_ch);

        if (cal->count < DS4424_CAL_MAX_POINTS) {
            cal->points[cal->count].dac_code = code_i8;
            cal->points[cal->count].measured_v = v;
            cal->count++;
        } else {
            bb_la_log("[CAL] warning %s: points array full\n", rail_name);
            break;
        }
        s_cal_progress = (uint8_t)((cal->count * 100) / 128);
        if (s_cal_progress > 99) s_cal_progress = 99;
        s_cal_stage = 4;
        s_cal_point = cal->count - 1;
        s_cal_code = code_i8;
        s_cal_measured_mv = (int32_t)(v * 1000.0f);
        bb_la_log("[CAL] %s progress=%u%% code=%d measured=%dmV\n",
                  rail_name, s_cal_progress, code_i8, (int)(v * 1000.0f));

        if (v >= max_volt) {
            break;
        }
    }

    // Settle back to 0
    ds4424_set_code(dac_ch, 0);
    vTaskDelay(pdMS_TO_TICKS(100));

    // Sweep source direction (lowering voltage)
    float min_volt = 0.5f;
    for (int code = 2; code <= 127; code += 2) {
        int8_t code_i8 = (int8_t)code;
        s_cal_stage = 2;
        s_cal_point = cal->count;
        s_cal_code = code_i8;
        s_cal_measured_mv = -1;
        if (!ds4424_set_code(dac_ch, code_i8)) {
            s_cal_state = 3;
            s_cal_last_error = 1;
            s_cal_task_handle = NULL;
            bb_la_log("[CAL] failed %s: DS4424 write code=%d\n", rail_name, code_i8);
            vTaskDelete(NULL);
            return;
        }
        s_cal_stage = 3;
        float v = measure_supply_stable_for_cal(adc_ch);

        if (cal->count < DS4424_CAL_MAX_POINTS) {
            cal->points[cal->count].dac_code = code_i8;
            cal->points[cal->count].measured_v = v;
            cal->count++;
        } else {
            bb_la_log("[CAL] warning %s: points array full\n", rail_name);
            break;
        }
        s_cal_progress = (uint8_t)((cal->count * 100) / 128);
        if (s_cal_progress > 99) s_cal_progress = 99;
        s_cal_stage = 4;
        s_cal_point = cal->count - 1;
        s_cal_code = code_i8;
        s_cal_measured_mv = (int32_t)(v * 1000.0f);
        bb_la_log("[CAL] %s progress=%u%% code=%d measured=%dmV\n",
                  rail_name, s_cal_progress, code_i8, (int)(v * 1000.0f));

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
        bool queued = flash_save();
        s_cal_state = 2;
        s_cal_progress = 100;
        s_cal_stage = 5;
        bb_la_log("[CAL] done %s: %u points, persistence=%s\n",
                  rail_name, cal->count, queued ? "queued" : "queue-failed");
    } else {
        s_cal_state = 3;
        s_cal_last_error = 2;
        s_cal_stage = 8;
        bb_la_log("[CAL] failed %s: insufficient points\n", rail_name);
    }

    s_cal_task_handle = NULL;
    vTaskDelete(NULL);
}

// -----------------------------------------------------------------------------
// Color Code mapping helper
// -----------------------------------------------------------------------------
static void ws2812_set_pixel_brightness(uint8_t index, uint8_t r, uint8_t g, uint8_t b, uint8_t brightness)
{
    uint8_t br = (uint16_t)r * brightness / 255;
    uint8_t bg = (uint16_t)g * brightness / 255;
    uint8_t bb = (uint16_t)b * brightness / 255;
    ws2812_set_pixel(index, br, bg, bb);
}

static void ws2812_fade_single(uint8_t index, uint8_t r, uint8_t g, uint8_t b, uint16_t duration_ms)
{
    const int steps = 25;
    int step_delay = (duration_ms / 2) / steps;
    if (step_delay < 10) step_delay = 10;

    // Fade in
    for (int i = 0; i <= steps; i++) {
        ws2812_set_pixel_brightness(index, r, g, b, (i * 255) / steps);
        ws2812_update();
        vTaskDelay(pdMS_TO_TICKS(step_delay));
    }
    // Fade out
    for (int i = steps; i >= 0; i--) {
        ws2812_set_pixel_brightness(index, r, g, b, (i * 255) / steps);
        ws2812_update();
        vTaskDelay(pdMS_TO_TICKS(step_delay));
    }
}

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
    if (s_persist_mutex == NULL) {
        s_persist_mutex = xSemaphoreCreateMutex();
    }
    if (s_persist_task_handle == NULL) {
        BaseType_t ok = xTaskCreate(persist_task_func, "cal_save", 2048, NULL, 1, &s_persist_task_handle);
        if (ok != pdPASS) {
            s_persist_task_handle = NULL;
            s_persist_state = HAT_PERSIST_FAILED;
            bb_la_log("[CAL] flash save worker create failed\n");
        }
    }

    // Boot animation: Green sweep 1->8 (10x slower with smooth fades).
    for (int i = 0; i < BB_WS2812_COUNT; i++) {
        ws2812_fade_single(i, 0, 255, 0, 500);
    }

    // Pulse all green
    const int pulse_steps = 40;
    for (int s = 0; s <= pulse_steps; s++) {
        uint8_t br = (s * 255) / pulse_steps;
        for (int i = 0; i < BB_WS2812_COUNT; i++) {
            ws2812_set_pixel_brightness(i, 0, 255, 0, br);
        }
        ws2812_update();
        vTaskDelay(pdMS_TO_TICKS(15));
    }
    vTaskDelay(pdMS_TO_TICKS(200));
    for (int s = pulse_steps; s >= 0; s--) {
        uint8_t br = (s * 255) / pulse_steps;
        for (int i = 0; i < BB_WS2812_COUNT; i++) {
            ws2812_set_pixel_brightness(i, 0, 255, 0, br);
        }
        ws2812_update();
        vTaskDelay(pdMS_TO_TICKS(15));
    }

    // Set LED 1 health OK (Green)
    ws2812_set_pixel(0, 0, 255, 0);
    ws2812_update();
}

void bb_hat_v2_handle_reset(void)
{
    memset(s_led_states, 0, sizeof(s_led_states));
    s_la_route = HAT_LA_ROUTE_LOW_SPEED;
    s_io_voltage_mv = 3300;
    s_cal_state = 0;
    s_cal_progress = 0;
    s_cal_rail_id = 0;
    s_cal_last_error = 0;
    s_cal_stage = 0;
    s_cal_point = 0;
    s_cal_code = 0;
    s_cal_measured_mv = -1;

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
                       bb_power_get_3v3_adj_enabled(),
                       bb_hat_v2_get_io_voltage(), 0,
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
    uint8_t status = s_cal_state;
    send_ok(&status, 1);

    if (xTaskCreate(cal_task_func, "cal_task", 1536, (void*)(uintptr_t)rail_id, 1, &s_cal_task_handle) != pdPASS) {
        s_cal_state = 3;
        s_cal_last_error = HAT_ERR_BUSY;
        s_cal_task_handle = NULL;
        bb_la_log("[CAL] failed rail=%u: task create failed\n", rail_id);
    }
}

void handle_calibrate_status(void)
{
    uint8_t rsp[12];
    int32_t measured_mv = s_cal_measured_mv;
    int8_t code = s_cal_code;
    uint8_t stage = s_cal_stage;
    rsp[0] = s_cal_state;
    rsp[1] = s_cal_progress;
    rsp[2] = s_cal_rail_id;
    rsp[3] = s_cal_last_error;
    rsp[4] = (uint8_t)s_persist_state;
    rsp[5] = stage;
    rsp[6] = s_cal_point;
    rsp[7] = (uint8_t)code;
    memcpy(&rsp[8], &measured_mv, sizeof(measured_mv));
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

    // Check if the calibration matches what is already saved to avoid flash wear
    bool different = false;
    if (!cal->valid || cal->count != count) {
        different = true;
    } else {
        size_t pos = 2;
        for (int i = 0; i < count; i++) {
            int8_t code = (int8_t)payload[pos++];
            float val;
            memcpy(&val, &payload[pos], 4);
            pos += 4;
            if (cal->points[i].dac_code != code || cal->points[i].measured_v != val) {
                different = true;
                break;
            }
        }
    }

    if (!different) {
        send_ok(NULL, 0);
        return;
    }

    memset(cal, 0, sizeof(DS4424CalData));
    size_t pos = 2;
    for (int i = 0; i < count; i++) {
        cal->points[i].dac_code = (int8_t)payload[pos++];
        memcpy(&cal->points[i].measured_v, &payload[pos], 4);
        pos += 4;
    }
    cal->count = count;
    cal->valid = (count >= 2);

    send_ok(NULL, 0);
    bool queued = flash_save();
    bb_la_log("[CAL] import rail=%u points=%u persistence=%s\n",
              rail_id, count, queued ? "queued" : "queue-failed");
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

void handle_set_rail_voltage(const uint8_t *payload, uint8_t len)
{
    if (len < 3) { send_error(HAT_ERR_FRAME); return; }
    uint8_t rail_id = payload[0];
    uint16_t mv = (uint16_t)payload[1] | ((uint16_t)payload[2] << 8);

    if (!bb_hat_v2_set_rail_voltage(rail_id, mv)) {
        send_error(HAT_ERR_INVALID_FUNC);
        return;
    }

    handle_get_rail_status();
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

bool bb_hat_v2_set_rail_voltage(uint8_t rail_id, uint16_t mv)
{
    if (!s_ds4424_present) return false;
    if (rail_id == HAT_RAIL_3V3_ADJ) {
        return bb_hat_v2_set_io_voltage(mv);
    }
    if (rail_id != HAT_RAIL_VADJ3 && rail_id != HAT_RAIL_VADJ4) return false;

    uint8_t dac_ch = (rail_id == HAT_RAIL_VADJ3) ? 1 : 2;
    float volts = (float)mv / 1000.0f;
    int8_t code = ds4424_voltage_to_code(dac_ch, volts);
    return ds4424_set_code(dac_ch, code);
}

uint16_t bb_hat_v2_get_io_voltage(void)
{
    return s_io_voltage_mv;
}
