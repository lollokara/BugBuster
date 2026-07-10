// =============================================================================
// adaq7769_stream.c — DRDY-driven sample capture into a PSRAM ring buffer
// =============================================================================

#include "adaq7769_stream.h"
#include <string.h>
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_attr.h"
#include "esp_timer.h"
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
// DRDY ISR: set this device's bit in the capture task's notification value.
// This is far lighter than a FreeRTOS queue (no queue structure, no per-item
// critical section) and coalesces cleanly: if the task falls behind, repeated
// DRDYs on a device collapse to one bit (one read of the current sample) rather
// than piling up stale re-reads.
// -----------------------------------------------------------------------------
static void IRAM_ATTR drdy_isr(void *arg)
{
    adaq_drdy_ctx_t *ctx = (adaq_drdy_ctx_t *)arg;
    adaq_stream_t   *s   = (adaq_stream_t *)ctx->stream;
    s->isr_count++;               // raw trigger count (flood detector)
    BaseType_t hp = pdFALSE;
    xTaskNotifyFromISR(s->capture_task, (1u << ctx->index), eSetBits, &hp);
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
// Read one device's continuous-read word and push a record. Shared by the
// bitmask loop.
static inline void capture_one(adaq_stream_t *s, uint8_t idx, size_t plen, uint8_t *buf)
{
    adaq7769_t *dev = s->devices[idx];
    // Direct SPI-FIFO fast read (~2.5 us vs ~10.5 us for the driver path). The
    // bus is held + primed + fifo_setup() done once by the capture task, so
    // this is just trigger + poll + FIFO drain. CS is driven manually here (the
    // FIFO path drives none): assert this device low, read, deassert. On a
    // shared bus this also selects only the device being read.
    adaq_ll_cs_assert(&dev->ll);
    esp_err_t err = adaq_ll_fifo_read(&dev->ll, buf, plen);
    adaq_ll_cs_deassert(&dev->ll);
    if (err != ESP_OK) {
        return;
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

static void capture_task(void *arg)
{
    adaq_stream_t *s = (adaq_stream_t *)arg;
    const size_t plen = payload_len(s);
    uint8_t buf[5];

    // Store our own handle BEFORE installing the ISRs so the very first DRDY
    // notification has a valid target.
    s->capture_task = xTaskGetCurrentTaskHandle();

    // Install the DRDY interrupts from THIS task so they are serviced on the
    // core the task is pinned to (the acquisition core), keeping the per-sample
    // interrupt storm off core 0 (USB/DSP/links). gpio_install_isr_service is
    // idempotent across the two per-bus capture tasks (2nd call returns
    // INVALID_STATE). Must NOT be done via esp_ipc — that runs on the tiny
    // ipc task stack and overflows it (stack-protection fault).
    gpio_install_isr_service(0);
    for (uint8_t i = 0; i < s->device_count; ++i) {
        gpio_num_t pin = s->devices[i]->drdy_pin;
        if (pin == GPIO_NUM_NC) continue;
        s->drdy_ctx[i].stream = s;
        s->drdy_ctx[i].index  = i;
        gpio_set_intr_type(pin, GPIO_INTR_POSEDGE);
        gpio_isr_handler_add(pin, drdy_isr, &s->drdy_ctx[i]);
    }

    // Hold the SPI bus for the whole session on a single-device bus so each
    // contread skips the per-call bus-lock acquire. A shared bus (2 devices)
    // can't hold (the 2nd device could never transact).
    bool bus_held = (s->device_count == 1) &&
                    (adaq_ll_bus_acquire(&s->devices[0]->ll) == ESP_OK);

    // Blocks on a lightweight task notification when idle (so CPU1 IDLE runs);
    // under a flood it runs at 100% on CPU1 (its IDLE-task WDT is disabled).
    while (s->running) {
        uint32_t bits = 0;
        if (xTaskNotifyWait(0, 0xFFFFFFFFu, &bits, pdMS_TO_TICKS(100)) != pdTRUE) {
            continue;
        }
        for (uint8_t idx = 0; idx < s->device_count; ++idx) {
            if (bits & (1u << idx)) {
                capture_one(s, idx, plen, buf);
            }
        }
    }
    if (bus_held) {
        adaq_ll_bus_release(&s->devices[0]->ll);
    }
    // Remove the DRDY handlers we installed above before exiting.
    for (uint8_t i = 0; i < s->device_count; ++i) {
        gpio_num_t pin = s->devices[i]->drdy_pin;
        if (pin != GPIO_NUM_NC) gpio_isr_handler_remove(pin);
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
    return ESP_OK;
}

void adaq_stream_deinit(adaq_stream_t *s)
{
    if (s->running) {
        adaq_stream_stop(s);
    }
    if (s->ring) {
        heap_caps_free(s->ring);
        s->ring = NULL;
    }
}

// Put a stream's devices into continuous conversion + continuous read and reset
// its counters. Shared by the per-stream and combined capture starts.
static esp_err_t stream_arm(adaq_stream_t *s)
{
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
    s->isr_count = 0;
    memset((void *)s->seq, 0, sizeof(s->seq));
    s->running = true;
    return ESP_OK;
}

esp_err_t adaq_stream_start(adaq_stream_t *s, int task_core, int task_prio)
{
    if (s->running) return ESP_OK;

    esp_err_t err = stream_arm(s);
    if (err != ESP_OK) return err;

    // The capture task installs the GPIO ISR service + DRDY handlers itself so
    // they are serviced on its (acquisition) core. Nothing to do here.
    BaseType_t ok = xTaskCreatePinnedToCore(capture_task, "adaq_cap", 4096, s,
                                            task_prio, &s->capture_task, task_core);
    if (ok != pdPASS) {
        s->running = false;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t adaq_stream_stop(adaq_stream_t *s)
{
    if (!s->running) return ESP_OK;

    s->running = false;
    // The capture task observes running=false, removes its own DRDY handlers,
    // and self-deletes.
    vTaskDelay(pdMS_TO_TICKS(150));
    s->capture_task = NULL;

    // Exit continuous-read mode so registers are accessible again.
    for (uint8_t i = 0; i < s->device_count; ++i) {
        adaq7769_t *dev = s->devices[i];
        uint8_t key[2] = { ADAQ_CONTREAD_EXIT_KEY, 0x00 };
        adaq_ll_write_raw(&dev->ll, key, sizeof(key));
        dev->cfg.cont_read = false;
        // Restore register-access CRC (EN_SPI_CRC) — see adaq_stream_comb_stop.
        adaq_ll_set_crc(&dev->ll, false, false);
        dev->cfg.crc_append = true;
        adaq7769_set_read_format(dev, /*continuous=*/false, /*status=*/false,
                                 /*crc=*/true, /*crc_xor=*/false, dev->cfg.conv16);
    }
    return ESP_OK;
}

// -----------------------------------------------------------------------------
// Combined capture — one task, all buses.
// -----------------------------------------------------------------------------
static void IRAM_ATTR comb_drdy_isr(void *arg)
{
    struct adaq_comb_dev *d  = (struct adaq_comb_dev *)arg;
    adaq_stream_comb_t   *c  = (adaq_stream_comb_t *)d->comb;
    d->stream->isr_count++;
    // Set this device's ready bit. Only wake the task on the idle->busy edge
    // (mask was empty): while it's actively draining, no notification is sent,
    // so the per-sample interrupt cost collapses to a single atomic OR.
    uint32_t prev = __atomic_fetch_or(&c->pending, (1u << d->bit), __ATOMIC_RELAXED);
    if (prev == 0) {
        BaseType_t hp = pdFALSE;
        vTaskNotifyGiveFromISR(c->task, &hp);
        if (hp == pdTRUE) {
            portYIELD_FROM_ISR();
        }
    }
}

static void capture_task_comb(void *arg)
{
    adaq_stream_comb_t *c = (adaq_stream_comb_t *)arg;
    uint8_t buf[5];

    // Store our own handle BEFORE installing ISRs (first DRDY needs a target).
    c->task = xTaskGetCurrentTaskHandle();

    gpio_install_isr_service(0);   // idempotent
    for (uint8_t i = 0; i < c->n_dev; ++i) {
        gpio_num_t pin = c->dev[i].stream->devices[c->dev[i].local]->drdy_pin;
        if (pin == GPIO_NUM_NC) continue;
        gpio_set_intr_type(pin, GPIO_INTR_POSEDGE);
        gpio_isr_handler_add(pin, comb_drdy_isr, &c->dev[i]);
    }

    // Hold each host's bus and prime it (one driver read sets clock/mode), then
    // configure the direct SPI-FIFO fast read. All devices on a host share the
    // same 20 MHz Mode-3 config, so priming/setup on the first device of each
    // stream serves the rest; the other devices' fifo_read()s poke the FIFO
    // directly (no bus lock needed). Held for the whole session.
    for (uint8_t si = 0; si < c->n_streams; ++si) {
        adaq7769_t *d0 = c->streams[si]->devices[0];
        uint8_t tmp[5];
        adaq_ll_bus_acquire(&d0->ll);
        adaq_ll_contread_word(&d0->ll, tmp, payload_len(c->streams[si]));
        adaq_ll_fifo_setup(&d0->ll, payload_len(c->streams[si]));
    }

    // Take every device's CS pin away from the SPI peripheral and drive it
    // manually for the session (the FIFO fast-read path asserts no CS on its
    // own). Done per device so a shared bus's two devices each get their own CS.
    for (uint8_t i = 0; i < c->n_dev; ++i) {
        adaq7769_t *dev = c->dev[i].stream->devices[c->dev[i].local];
        adaq_ll_cs_manual_begin(&dev->ll);
    }

    while (c->running) {
        // Grab and clear the ready mask in one atomic op. If empty, block on the
        // notification (so CPU1 IDLE runs); otherwise drain every ready device.
        uint32_t bits = __atomic_exchange_n(&c->pending, 0u, __ATOMIC_RELAXED);
        if (bits == 0) {
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(100));
            continue;
        }
        for (uint8_t i = 0; i < c->n_dev; ++i) {
            if (bits & (1u << c->dev[i].bit)) {
                adaq_stream_t *s = c->dev[i].stream;
                capture_one(s, c->dev[i].local, payload_len(s), buf);
            }
        }
    }
    for (uint8_t i = 0; i < c->n_dev; ++i) {
        gpio_num_t pin = c->dev[i].stream->devices[c->dev[i].local]->drdy_pin;
        if (pin != GPIO_NUM_NC) gpio_isr_handler_remove(pin);
    }
    // Release the held buses first (so no device holds the bus lock), then hand
    // every device's CS pin back to the SPI peripheral (rebuilds dev_cfg) so
    // register access works again after the stream stops.
    for (uint8_t si = 0; si < c->n_streams; ++si) {
        adaq_ll_bus_release(&c->streams[si]->devices[0]->ll);
    }
    for (uint8_t i = 0; i < c->n_dev; ++i) {
        adaq7769_t *dev = c->dev[i].stream->devices[c->dev[i].local];
        adaq_ll_cs_manual_end(&dev->ll);
    }
    vTaskDelete(NULL);
}

esp_err_t adaq_stream_comb_start(adaq_stream_comb_t *c,
                                 adaq_stream_t *const *streams, uint8_t n_streams,
                                 int core, int prio)
{
    if (n_streams == 0 || n_streams > ADAQ_STREAM_MAX_DEVICES) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(c, 0, sizeof(*c));
    c->n_streams = n_streams;
    uint8_t bit = 0;
    for (uint8_t si = 0; si < n_streams; ++si) {
        adaq_stream_t *s = streams[si];
        c->streams[si] = s;
        esp_err_t err = stream_arm(s);
        if (err != ESP_OK) return err;
        for (uint8_t d = 0; d < s->device_count; ++d) {
            c->dev[c->n_dev].comb   = c;
            c->dev[c->n_dev].stream = s;
            c->dev[c->n_dev].local  = d;
            c->dev[c->n_dev].bit    = bit++;
            c->n_dev++;
        }
    }
    c->running = true;
    BaseType_t ok = xTaskCreatePinnedToCore(capture_task_comb, "adaq_cap", 4096,
                                            c, prio, &c->task, core);
    if (ok != pdPASS) {
        c->running = false;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t adaq_stream_comb_stop(adaq_stream_comb_t *c)
{
    if (!c->running) return ESP_OK;
    c->running = false;
    // Mark streams stopped (consumer/UI reads these flags).
    for (uint8_t si = 0; si < c->n_streams; ++si) {
        c->streams[si]->running = false;
    }
    // Let the task observe running=false, remove its handlers, and self-delete.
    vTaskDelay(pdMS_TO_TICKS(150));
    c->task = NULL;

    // Exit continuous-read on every device so registers are accessible again.
    for (uint8_t si = 0; si < c->n_streams; ++si) {
        adaq_stream_t *s = c->streams[si];
        for (uint8_t i = 0; i < s->device_count; ++i) {
            adaq7769_t *dev = s->devices[i];
            uint8_t key[2] = { ADAQ_CONTREAD_EXIT_KEY, 0x00 };
            adaq_ll_write_raw(&dev->ll, key, sizeof(key));
            dev->cfg.cont_read = false;
            // Restore register-access CRC (EN_SPI_CRC): streaming ran with CRC
            // off, but the trailing CRC byte is what lets the ESP32-P4 capture
            // each register LSB — without it, register reads drop bit0 again
            // (value & 0xFE). set_crc(false) first so the INTERFACE_FORMAT write
            // itself goes out as a plain (no-CRC) frame.
            adaq_ll_set_crc(&dev->ll, false, false);
            dev->cfg.crc_append = true;
            adaq7769_set_read_format(dev, /*continuous=*/false, /*status=*/false,
                                     /*crc=*/true, /*crc_xor=*/false,
                                     dev->cfg.conv16);
        }
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
