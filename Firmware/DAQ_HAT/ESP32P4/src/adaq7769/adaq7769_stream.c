// =============================================================================
// adaq7769_stream.c — DRDY-driven sample capture into a PSRAM ring buffer
// =============================================================================

#include "adaq7769_stream.h"
#include <string.h>
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_attr.h"
#include "driver/gpio.h"

static const char *TAG = "adaq_stream";

// Round up to the next power of two (>= 8).
static size_t round_pow2(size_t v)
{
    size_t p = 8;
    while (p < v) p <<= 1;
    return p;
}

// Continuous-read payload length: 3 data bytes (+1 status) (+1 CRC).
static size_t payload_len(const adaq_stream_t *s)
{
    size_t n = 3;
    if (s->append_status) n += 1;
    if (s->append_crc)    n += 1;
    return n;
}

// -----------------------------------------------------------------------------
// DRDY ISR: post the device index to the capture task's queue.
// -----------------------------------------------------------------------------
typedef struct {
    adaq_stream_t *stream;
    uint8_t        index;
} drdy_ctx_t;

static drdy_ctx_t s_drdy_ctx[ADAQ_STREAM_MAX_DEVICES];

static void IRAM_ATTR drdy_isr(void *arg)
{
    drdy_ctx_t *ctx = (drdy_ctx_t *)arg;
    BaseType_t hp = pdFALSE;
    xQueueSendFromISR(ctx->stream->drdy_queue, &ctx->index, &hp);
    if (hp == pdTRUE) {
        portYIELD_FROM_ISR();
    }
}

// -----------------------------------------------------------------------------
// Ring buffer (SPSC). Producer = capture task, consumer = adaq_stream_read.
// -----------------------------------------------------------------------------
static inline bool ring_push(adaq_stream_t *s, const adaq_sample_t *rec)
{
    size_t head = s->head;
    size_t next = (head + 1) & (s->ring_capacity - 1);
    if (next == s->tail) {
        s->overflow_count++;
        return false;            // full; drop newest
    }
    s->ring[head] = *rec;
    s->head = next;
    s->sample_count++;
    return true;
}

// -----------------------------------------------------------------------------
// Capture task — robust path
// -----------------------------------------------------------------------------
static void capture_task(void *arg)
{
    adaq_stream_t *s = (adaq_stream_t *)arg;
    const size_t plen = payload_len(s);
    uint8_t buf[5];

    while (s->running) {
        uint8_t idx;
        if (xQueueReceive(s->drdy_queue, &idx, pdMS_TO_TICKS(100)) != pdTRUE) {
            continue;
        }
        if (idx >= s->device_count) continue;
        adaq7769_t *dev = s->devices[idx];

        esp_err_t err = adaq_ll_contread_word(&dev->ll, buf, plen);
        if (err != ESP_OK) {
            continue;
        }

        adaq_sample_t rec = {0};
        rec.device_id = idx;
        uint32_t raw = ((uint32_t)buf[0] << 16) | ((uint32_t)buf[1] << 8) | buf[2];
        rec.value = adaq_sign_extend24(raw);

        size_t off = 3;
        if (s->append_status) {
            rec.status = buf[off++];
            if (rec.status & (ADAQ_ST_MASTER_ERROR | ADAQ_ST_FILT_SATURATED |
                              ADAQ_ST_FILT_NOT_SETTLED)) {
                rec.flags |= ADAQ_SAMPLE_FLAG_STATUS_ERR;
            }
        }
        if (s->append_crc) {
            uint8_t crc_rx = buf[off];
            // Continuous-read CRC initial value is 0x03 (poly) per datasheet.
            uint8_t crc = adaq_ll_crc8(buf, off, 0x03);
            if (crc != crc_rx) {
                rec.flags |= ADAQ_SAMPLE_FLAG_CRC_ERR;
            }
        }
        rec.seq = s->seq[idx]++;
        ring_push(s, &rec);
    }
    vTaskDelete(NULL);
}

// -----------------------------------------------------------------------------
// Public API
// -----------------------------------------------------------------------------
esp_err_t adaq_stream_init(adaq_stream_t *s,
                           adaq7769_t *const *devices, uint8_t count,
                           size_t ring_capacity,
                           bool append_status, bool append_crc)
{
    if (count == 0 || count > ADAQ_STREAM_MAX_DEVICES) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(s, 0, sizeof(*s));
    s->device_count   = count;
    s->append_status  = append_status;
    s->append_crc     = append_crc;
    for (uint8_t i = 0; i < count; ++i) {
        s->devices[i] = devices[i];
    }

    s->ring_capacity = round_pow2(ring_capacity);
    s->ring = (adaq_sample_t *)heap_caps_malloc(
        s->ring_capacity * sizeof(adaq_sample_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s->ring) {
        // Fall back to internal RAM if PSRAM is unavailable.
        ESP_LOGW(TAG, "PSRAM alloc failed, using internal RAM");
        s->ring = (adaq_sample_t *)heap_caps_malloc(
            s->ring_capacity * sizeof(adaq_sample_t), MALLOC_CAP_8BIT);
    }
    if (!s->ring) {
        return ESP_ERR_NO_MEM;
    }

    s->drdy_queue = xQueueCreate(256, sizeof(uint8_t));
    if (!s->drdy_queue) {
        heap_caps_free(s->ring);
        s->ring = NULL;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void adaq_stream_deinit(adaq_stream_t *s)
{
    if (s->running) {
        adaq_stream_stop(s);
    }
    if (s->drdy_queue) {
        vQueueDelete(s->drdy_queue);
        s->drdy_queue = NULL;
    }
    if (s->ring) {
        heap_caps_free(s->ring);
        s->ring = NULL;
    }
}

esp_err_t adaq_stream_start(adaq_stream_t *s, int task_core, int task_prio)
{
    if (s->running) return ESP_OK;

    // Put every device into continuous conversion + continuous read.
    for (uint8_t i = 0; i < s->device_count; ++i) {
        adaq7769_t *dev = s->devices[i];
        esp_err_t err = adaq7769_set_conv_mode(dev, ADAQ_CONVMODE_CONTINUOUS);
        if (err != ESP_OK) return err;
        err = adaq7769_set_read_format(dev, /*continuous=*/true,
                                       s->append_status, s->append_crc,
                                       /*crc_xor=*/false, dev->cfg.conv16);
        if (err != ESP_OK) return err;
    }

    s->head = s->tail = 0;
    s->overflow_count = s->sample_count = 0;
    memset((void *)s->seq, 0, sizeof(s->seq));
    s->running = true;

    BaseType_t ok = xTaskCreatePinnedToCore(capture_task, "adaq_cap", 4096, s,
                                            task_prio, &s->capture_task, task_core);
    if (ok != pdPASS) {
        s->running = false;
        return ESP_ERR_NO_MEM;
    }

    // Install per-device DRDY interrupts (rising edge = new data ready).
    gpio_install_isr_service(0);   // harmless if already installed
    for (uint8_t i = 0; i < s->device_count; ++i) {
        gpio_num_t pin = s->devices[i]->drdy_pin;
        if (pin == GPIO_NUM_NC) continue;
        s_drdy_ctx[i].stream = s;
        s_drdy_ctx[i].index  = i;
        gpio_set_intr_type(pin, GPIO_INTR_POSEDGE);
        gpio_isr_handler_add(pin, drdy_isr, &s_drdy_ctx[i]);
    }
    return ESP_OK;
}

esp_err_t adaq_stream_stop(adaq_stream_t *s)
{
    if (!s->running) return ESP_OK;

    for (uint8_t i = 0; i < s->device_count; ++i) {
        gpio_num_t pin = s->devices[i]->drdy_pin;
        if (pin != GPIO_NUM_NC) {
            gpio_isr_handler_remove(pin);
        }
    }

    s->running = false;
    // Let the capture task observe running=false and self-delete.
    vTaskDelay(pdMS_TO_TICKS(150));
    s->capture_task = NULL;

    // Exit continuous-read mode so registers are accessible again.
    for (uint8_t i = 0; i < s->device_count; ++i) {
        adaq7769_t *dev = s->devices[i];
        uint8_t key[2] = { ADAQ_CONTREAD_EXIT_KEY, 0x00 };
        adaq_ll_write_raw(&dev->ll, key, sizeof(key));
        dev->cfg.cont_read = false;
        adaq_ll_set_crc(&dev->ll, false, false);
    }
    return ESP_OK;
}

size_t adaq_stream_read(adaq_stream_t *s, adaq_sample_t *out, size_t max)
{
    size_t n = 0;
    while (n < max) {
        size_t tail = s->tail;
        if (tail == s->head) break;     // empty
        out[n++] = s->ring[tail];
        s->tail = (tail + 1) & (s->ring_capacity - 1);
    }
    return n;
}

size_t adaq_stream_available(const adaq_stream_t *s)
{
    size_t head = s->head, tail = s->tail;
    return (head - tail) & (s->ring_capacity - 1);
}
