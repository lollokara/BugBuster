// =============================================================================
// hat.cpp - HAT Expansion Board Driver
//
// Handles detection (GPIO47 binary strap), UART communication (GPIO43/44, 921600 8N1),
// and EXP_EXT_1-4 pin configuration for attached HAT boards.
// PCB mode only.
// =============================================================================

#include "hat.h"
#include "pca9535.h"
#include "dio.h"
#include "config.h"
#include "bbp.h"
#include "ds4424.h"
#include "ws_stream.h"
#include "esp_log.h"
#include "driver/gpio.h"
#include "driver/uart.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_timer.h"
#include <string.h>
#include <inttypes.h>

static const char *TAG = "hat";

// HAT support enabled in both breadboard and PCB modes.
// In breadboard mode: no detect strap (assume HAT present), no IRQ.
// In PCB mode: binary detect strap + IRQ support.

static HatState s_state = {};
static uint8_t s_last_sent_color[9] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
#if !HAT_NO_DETECT
static bool s_detect_pin_ready = false;
#endif
static bool s_initialized = false;
static uint8_t s_last_error = 0;
static SemaphoreHandle_t s_hat_mutex = NULL;
static SemaphoreHandle_t s_log_mutex = NULL;
static HatLogRing *s_log_ring = nullptr;

static volatile bool s_la_done_pending = false;
#if PIN_HAT_LA_DONE_IRQ >= 0
// Dedicated LA-done IRQ, if a board revision routes one separately from HAT INT.
// Set from the GPIO ISR on falling edge, consumed by hat_la_done_consume().
// Tracks whether we've registered the global GPIO ISR service so we don't
// call gpio_install_isr_service() more than once.
static bool s_gpio_isr_service_installed = false;

static void IRAM_ATTR hat_la_done_isr(void *arg)
{
    (void)arg;
    s_la_done_pending = true;
}
#endif

static uint32_t hat_now_ms(void)
{
    return (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
}

static void hat_note_uart_success(void)
{
    uint32_t now = hat_now_ms();
    s_state.last_ok_ms = now;
    s_state.last_ping_ms = now;
    s_state.consecutive_timeouts = 0;
    s_state.degraded = false;
}

static void hat_note_uart_timeout(void)
{
    s_state.last_timeout_ms = hat_now_ms();
    if (s_state.consecutive_timeouts < UINT8_MAX) {
        s_state.consecutive_timeouts++;
    }
    s_state.degraded = s_state.detected && s_state.connected;
}

// -----------------------------------------------------------------------------
// CRC-8 (polynomial 0x07, same as AD74416H SPI CRC)
// -----------------------------------------------------------------------------
static uint8_t crc8(const uint8_t *data, size_t len)
{
    uint8_t crc = 0x00;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int j = 0; j < 8; j++) {
            crc = (crc & 0x80) ? ((crc << 1) ^ 0x07) : (crc << 1);
        }
    }
    return crc;
}

// -----------------------------------------------------------------------------
// UART Helpers
// -----------------------------------------------------------------------------

// Flush the UART RX FIFO to discard any stale or corrupt data (e.g. from a reboot).
void hat_uart_flush(void)
{
    ESP_LOGD(TAG, "Flushing HAT UART RX FIFO");
    uart_flush_input(HAT_UART_NUM);
}

// Flush stale UART bytes after a command failure.
//
// Do NOT clear s_state.connected here: a single timeout should not turn into a
// sticky offline state that makes every later HAT API call short-circuit before
// it even tries UART again. We want later commands to keep probing so the first
// real failure remains observable.
static void hat_reset_connection(void)
{
    ESP_LOGW(TAG, "Resetting HAT connection (UART flush)");
    hat_uart_flush();
}

// Send a command frame to the HAT. Returns true if bytes were sent.
static bool hat_send_frame(uint8_t cmd, const uint8_t *payload, uint8_t payload_len)
{
    if (payload_len > HAT_FRAME_MAX_LEN) return false;

    uint8_t frame[3 + HAT_FRAME_MAX_LEN + 1]; // SYNC + LEN + CMD + payload + CRC
    size_t pos = 0;

    frame[pos++] = HAT_FRAME_SYNC;
    frame[pos++] = payload_len;
    frame[pos++] = cmd;
    if (payload_len > 0 && payload) {
        memcpy(&frame[pos], payload, payload_len);
        pos += payload_len;
    }

    // CRC over CMD + payload
    frame[pos] = crc8(&frame[2], 1 + payload_len);
    pos++;

    ESP_LOGV(TAG, "TX frame (%" PRIu64 " us):", esp_timer_get_time());
    ESP_LOG_BUFFER_HEXDUMP(TAG, frame, pos, ESP_LOG_VERBOSE);

    int written = uart_write_bytes(HAT_UART_NUM, frame, pos);
    return written == (int)pos;
}

// Receive a response frame from the HAT.
// Blocks up to timeout_ms. Returns response CMD byte, fills payload/payload_len.
// Returns 0 on timeout or error.
static uint8_t hat_recv_frame(uint8_t *payload, uint8_t *payload_len, uint32_t timeout_ms, uint8_t max_payload_len)
{
    uint8_t buf[3 + HAT_FRAME_MAX_LEN + 1];
    size_t pos = 0;
    TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(timeout_ms);

    // 1. SYNC search: read byte-by-byte until SYNC found or timeout
    while (true) {
        TickType_t now = xTaskGetTickCount();
        if (now >= deadline) {
            ESP_LOGD(TAG, "RX timeout: no SYNC (%" PRIu64 " us)", esp_timer_get_time());
            return 0;
        }
        uint8_t b;
        if (uart_read_bytes(HAT_UART_NUM, &b, 1, 1) == 1) {
            if (b == HAT_FRAME_SYNC) {
                buf[pos++] = b;
                ESP_LOGV(TAG, "RX SYNC (0x%02X) @ %" PRIu64 " us", b, esp_timer_get_time());
                break;
            } else {
                ESP_LOGV(TAG, "RX junk (0x%02X) @ %" PRIu64 " us", b, esp_timer_get_time());
            }
        }
    }

    // 2. Header chunk: read LEN + CMD (2 bytes)
    {
        TickType_t now = xTaskGetTickCount();
        if (now >= deadline) return 0;
        int n = uart_read_bytes(HAT_UART_NUM, &buf[pos], 2, deadline - now);
        if (n != 2) {
            ESP_LOGD(TAG, "RX error: Header read failed (%d/2)", n);
            return 0;
        }
        ESP_LOGV(TAG, "RX Header @ %" PRIu64 " us: LEN=0x%02X CMD=0x%02X",
                 esp_timer_get_time(), buf[pos], buf[pos+1]);
        pos += 2;
    }

    uint8_t len = buf[1];
    uint8_t cmd = buf[2];
    if (len > HAT_FRAME_MAX_LEN) {
        ESP_LOGW(TAG, "RX error: Invalid len %d", len);
        return 0;
    }

    // 3. Payload chunk: read payload + CRC (len + 1 bytes)
    if (len + 1 > 0) {
        TickType_t now = xTaskGetTickCount();
        if (now >= deadline) return 0;
        int n = uart_read_bytes(HAT_UART_NUM, &buf[pos], len + 1, deadline - now);
        if (n != (int)(len + 1)) {
            ESP_LOGD(TAG, "RX error: Payload read failed (%d/%d)", n, len + 1);
            return 0;
        }
        ESP_LOGV(TAG, "RX Payload+CRC @ %" PRIu64 " us", esp_timer_get_time());
        pos += len + 1;
    }

    // Verify CRC over CMD + payload
    uint8_t expected_crc = crc8(&buf[2], 1 + len);
    uint8_t received_crc = buf[3 + len];
    if (expected_crc != received_crc) {
        ESP_LOGW(TAG, "CRC mismatch: expected 0x%02X, got 0x%02X", expected_crc, received_crc);
        ESP_LOG_BUFFER_HEXDUMP(TAG, buf, pos, ESP_LOG_WARN);
        return 0;
    }

    // Copy results
    if (payload && len > 0) {
        uint8_t to_copy = (len < max_payload_len) ? len : max_payload_len;
        memcpy(payload, &buf[3], to_copy);
    }
    if (payload_len) *payload_len = len;

    return cmd;
}

// ---------------------------------------------------------------------------
// Unsolicited-event buffering for hat_command / hat_command_internal
// ---------------------------------------------------------------------------

#define HAT_MAX_PENDING_EVENTS 4

typedef struct {
    uint8_t type;                        // BBP_EVT_LA_DONE or BBP_EVT_LA_LOG
    uint8_t payload[HAT_FRAME_MAX_LEN];
    uint8_t len;
} HatPendingEvent;

// Send command and wait for response. Returns response CMD, fills rsp_payload.
// Unsolicited LA events encountered during the wait are stored in pending_events[]
// instead of being dispatched immediately (s_hat_mutex is still held at this point).
static uint8_t hat_command_internal(uint8_t cmd, const uint8_t *payload, uint8_t payload_len,
                                    uint8_t *rsp_payload, uint8_t *rsp_len, uint32_t timeout_ms, uint8_t max_rsp_len,
                                    HatPendingEvent *pending_events, int *pending_count)
{
    s_last_error = 0;
    ESP_LOGD(TAG, "TX cmd=0x%02X len=%d t=%" PRIu64 "us", cmd, payload_len, esp_timer_get_time());

    // Flush any stale data before sending
    uart_flush_input(HAT_UART_NUM);

    if (!hat_send_frame(cmd, payload, payload_len)) {
        ESP_LOGW(TAG, "Failed to send command 0x%02X", cmd);
        return 0;
    }

    TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(timeout_ms);
    uint8_t final_rsp = 0;

    while (true) {
        TickType_t now = xTaskGetTickCount();
        if (now >= deadline) break;

        uint32_t remaining_ms = (deadline - now) * portTICK_PERIOD_MS;
        if (remaining_ms == 0) remaining_ms = 1;

        uint8_t local_payload[HAT_FRAME_MAX_LEN];
        uint8_t local_len = 0;
        uint8_t rsp = hat_recv_frame(local_payload, &local_len, remaining_ms, sizeof(local_payload));

        if (rsp == 0) break; 

        // Handle unsolicited LA status (capture done notification)
        if (rsp == HAT_RSP_LA_STATUS && cmd != HAT_CMD_LA_GET_STATUS && local_len >= 14) {
            uint8_t la_state = local_payload[0];
            if (la_state == 3) {  // LA_STATE_DONE
                ESP_LOGI(TAG, "LA capture done (unsolicited notification during wait)");
                if (bbpIsActive()) {
                    // Buffer the event — dispatching under s_hat_mutex risks deadlock
                    // (bbpSendEvent may acquire g_stateMutex rank-3). Will be sent
                    // after mutex release in hat_command.
                    if (*pending_count < HAT_MAX_PENDING_EVENTS) {
                        HatPendingEvent *ev = &pending_events[(*pending_count)++];
                        ev->type = BBP_EVT_LA_DONE;
                        ev->len  = local_len;
                        memcpy(ev->payload, local_payload, local_len);
                    } else {
                        ESP_LOGW(TAG, "pending LA event buffer full — BBP_EVT_LA_DONE dropped");
                    }
                }
            }
            continue;
        }

        // Forward log frames transparently — don't treat them as the command response
        if (rsp == HAT_RSP_LA_LOG) {
            ESP_LOGD(TAG, "LA log during cmd 0x%02X (len=%d)", cmd, local_len);
            if (bbpIsActive() && local_len > 0) {
                // Buffer the event — same deadlock risk as LA_DONE above.
                if (*pending_count < HAT_MAX_PENDING_EVENTS) {
                    HatPendingEvent *ev = &pending_events[(*pending_count)++];
                    ev->type = BBP_EVT_LA_LOG;
                    ev->len  = local_len;
                    memcpy(ev->payload, local_payload, local_len);
                } else {
                    ESP_LOGW(TAG, "pending LA event buffer full — BBP_EVT_LA_LOG dropped");
                }
            }
            // Push to HTTP polling ring (non-blocking, separate mutex — no deadlock risk)
            if (local_len > 0) {
                char log_line[HAT_LOG_LINE_MAX];
                uint8_t copy_len = local_len < (HAT_LOG_LINE_MAX - 1) ? local_len : (HAT_LOG_LINE_MAX - 1);
                memcpy(log_line, local_payload, copy_len);
                log_line[copy_len] = '\0';
                hat_log_ring_push(log_line);
            }
            continue;
        }

        // It's the response we wanted, or an error response
        if (rsp == HAT_RSP_ERROR && local_len >= 1) {
            s_last_error = local_payload[0];
            ESP_LOGW(TAG, "HAT command 0x%02X failed with error 0x%02X", cmd, s_last_error);
        }

        // Copy to caller's buffer
        if (rsp_payload && local_len > 0) {
            uint8_t to_copy = (local_len < max_rsp_len) ? local_len : max_rsp_len;
            memcpy(rsp_payload, local_payload, to_copy);
        }
        if (rsp_len) *rsp_len = local_len;

        ESP_LOGD(TAG, "RX rsp=0x%02X len=%d cmd=0x%02X t=%" PRIu64 "us", rsp, local_len, cmd, esp_timer_get_time());
        final_rsp = rsp;
        break;
    }

    return final_rsp;
}

uint8_t hat_command(uint8_t cmd, const uint8_t *payload, uint8_t payload_len,
                     uint8_t *rsp_payload, uint8_t *rsp_len, uint32_t timeout_ms, uint8_t max_rsp_len)
{
    if (s_hat_mutex && xSemaphoreTake(s_hat_mutex, pdMS_TO_TICKS(timeout_ms + 100)) != pdTRUE) {
        ESP_LOGE(TAG, "HAT command 0x%02X: failed to take mutex", cmd);
        return 0;
    }

    // Buffer for unsolicited LA events received while s_hat_mutex is held.
    // hat_command_internal accumulates events here instead of calling bbpSendEvent
    // directly to avoid a potential deadlock (bbpSendEvent may acquire g_stateMutex
    // rank-3 while s_hat_mutex is still held and unranked).
    HatPendingEvent pending_events[HAT_MAX_PENDING_EVENTS];
    int pending_count = 0;

    uint8_t rsp = hat_command_internal(cmd, payload, payload_len, rsp_payload, rsp_len, timeout_ms, max_rsp_len,
                                       pending_events, &pending_count);

    // One retry with a connection reset if the first attempt failed (timeout or junk)
    if (rsp == 0) {
        ESP_LOGD(TAG, "HAT command 0x%02X first attempt failed, retrying after reset...", cmd);
        hat_reset_connection();
        rsp = hat_command_internal(cmd, payload, payload_len, rsp_payload, rsp_len, timeout_ms, max_rsp_len,
                                   pending_events, &pending_count);
        if (rsp != 0) {
            s_state.connected = true; // Connection recovered
            ESP_LOGD(TAG, "HAT connection recovered during command 0x%02X", cmd);
        }
    }

    if (rsp == 0) {
        hat_note_uart_timeout();
        ESP_LOGW(TAG,
                 "HAT command 0x%02X failed or timed out after retry; "
                 "keeping HAT marked connected so later commands can retry",
                 cmd);
    } else {
        hat_note_uart_success();
    }

    if (s_hat_mutex) xSemaphoreGive(s_hat_mutex);

    // Dispatch buffered unsolicited LA events outside s_hat_mutex to avoid
    // potential deadlock (bbpSendEvent may acquire g_stateMutex rank-3).
    // TODO: bench-test LA_DONE / LA_LOG delivery latency against real HAT PCB
    // once available — the buffering adds ~0 ms but the sequencing change
    // (events arrive after command response) should be verified under load.
    for (int i = 0; i < pending_count; i++) {
        bbpSendEvent(pending_events[i].type, pending_events[i].payload, pending_events[i].len);
    }

    return rsp;
}

// -----------------------------------------------------------------------------
// Detect Pin
// -----------------------------------------------------------------------------

#if !HAT_NO_DETECT
static int hat_read_detect_level(void)
{
    if (!s_detect_pin_ready) return -1;
    return gpio_get_level(PIN_HAT_DETECT);
}
#endif

// -----------------------------------------------------------------------------
// Public API
// -----------------------------------------------------------------------------

static uint8_t hat_serialize_cal_subset(const DS4424CalData *cal, uint8_t *out, size_t out_len)
{
    if (!cal || !cal->valid || cal->count < 2 || !out) return 0;

    const uint8_t max_points = (uint8_t)(out_len / 5);
    if (max_points < 2) return 0;

    uint8_t count = cal->count < max_points ? cal->count : max_points;
    for (uint8_t i = 0; i < count; i++) {
        uint8_t idx = i;
        if (cal->count > count && count > 1) {
            idx = (uint8_t)(((uint16_t)i * (uint16_t)(cal->count - 1) + (uint16_t)(count - 1) / 2) /
                            (uint16_t)(count - 1));
        }
        out[i * 5] = (uint8_t)cal->points[idx].dac_code;
        memcpy(&out[i * 5 + 1], &cal->points[idx].measured_v, sizeof(float));
    }
    return count;
}

static bool hat_seed_3v3_adj_from_esp_cal(void)
{
    if (!s_state.connected) return false;

    const DS4424State *idac = ds4424_get_state();
    if (!idac || !idac->present) return false;

    const DS4424CalData *cal = &idac->cal[0];
    uint8_t points[30] = {};
    uint8_t count = hat_serialize_cal_subset(cal, points, sizeof(points));
    if (count < 2) {
        ESP_LOGW(TAG, "HAT 3V3_ADJ calibration missing and ESP DS4424 ch0 calibration is unavailable");
        return false;
    }

    if (!hat_calibrate_import(HAT_RAIL_3V3_ADJ, count, points, (size_t)count * 5)) {
        ESP_LOGW(TAG, "Failed to seed HAT 3V3_ADJ calibration from ESP DS4424 ch0");
        return false;
    }

    uint8_t rail_count = 0;
    hat_get_rail_status(s_state.rail, &rail_count);
    ESP_LOGI(TAG, "Seeded HAT 3V3_ADJ calibration from ESP DS4424 ch0 (%u/%u points)",
             count, cal->count);
    return true;
}

bool hat_init(void)
{
    memset(&s_state, 0, sizeof(s_state));

    if (s_hat_mutex == NULL) {
        s_hat_mutex = xSemaphoreCreateMutex();
    }
    if (s_log_mutex == NULL) {
        s_log_mutex = xSemaphoreCreateMutex();
    }
    if (s_log_ring == nullptr) {
        s_log_ring = (HatLogRing *)heap_caps_calloc(1, sizeof(HatLogRing), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }

    // Initialize detect pin as a binary strap:
    // HIGH = no HAT, LOW = HAT present.
#if !HAT_NO_DETECT
    {
        gpio_config_t detect_cfg = {
            .pin_bit_mask = (1ULL << PIN_HAT_DETECT),
            .mode = GPIO_MODE_INPUT,
            .pull_up_en = GPIO_PULLUP_ENABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        esp_err_t err = gpio_config(&detect_cfg);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Detect pin init failed: %s", esp_err_to_name(err));
            // Continue without detect — will try UART ping instead
        } else {
            s_detect_pin_ready = true;
        }
    }
#else
    ESP_LOGI(TAG, "No detect pin (breadboard) — will probe via UART ping");
#endif

    // Initialize UART for HAT communication
    uart_config_t uart_cfg = {};
    uart_cfg.baud_rate = HAT_UART_BAUD;
    uart_cfg.data_bits = UART_DATA_8_BITS;
    uart_cfg.parity = UART_PARITY_DISABLE;
    uart_cfg.stop_bits = UART_STOP_BITS_1;
    uart_cfg.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
    uart_cfg.source_clk = UART_SCLK_DEFAULT;
    esp_err_t err = uart_param_config(HAT_UART_NUM, &uart_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "UART config failed: %s", esp_err_to_name(err));
        return false;
    }
    err = uart_set_pin(HAT_UART_NUM, PIN_HAT_TX, PIN_HAT_RX, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "UART pin config failed: %s", esp_err_to_name(err));
        return false;
    }
    err = uart_driver_install(HAT_UART_NUM, HAT_UART_BUF_SIZE, HAT_UART_BUF_SIZE, 0, NULL, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "UART driver install failed: %s", esp_err_to_name(err));
        return false;
    }

    // Configure IRQ pin as open-drain input (shared line), if available
#if !HAT_NO_DETECT
    if ((int)PIN_HAT_IRQ >= 0) {
        gpio_config_t irq_cfg = {
            .pin_bit_mask = (1ULL << PIN_HAT_IRQ),
            .mode = GPIO_MODE_INPUT_OUTPUT_OD,
            .pull_up_en = GPIO_PULLUP_ENABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        err = gpio_config(&irq_cfg);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "HAT IRQ pin config failed: %s (non-fatal)", esp_err_to_name(err));
        } else {
            gpio_set_level(PIN_HAT_IRQ, 1);
        }
    }
#endif

    // Configure the dedicated LA-done IRQ input from the RP2040 (active low,
    // falling-edge interrupt). Uses the internal pull-up since the RP2040
    // side drives push-pull only during the brief done pulse.
#if PIN_HAT_LA_DONE_IRQ >= 0
    {
        gpio_config_t la_done_cfg = {
            .pin_bit_mask = (1ULL << PIN_HAT_LA_DONE_IRQ),
            .mode = GPIO_MODE_INPUT,
            .pull_up_en = GPIO_PULLUP_ENABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_NEGEDGE,
        };
        err = gpio_config(&la_done_cfg);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "LA-done IRQ pin config failed: %s (non-fatal)", esp_err_to_name(err));
        } else {
            if (!s_gpio_isr_service_installed) {
                esp_err_t isr_err = gpio_install_isr_service(0);
                // ESP_ERR_INVALID_STATE means it was already installed by
                // another subsystem — that's fine, we can still add a handler.
                if (isr_err == ESP_OK || isr_err == ESP_ERR_INVALID_STATE) {
                    s_gpio_isr_service_installed = true;
                } else {
                    ESP_LOGW(TAG, "gpio_install_isr_service failed: %s",
                             esp_err_to_name(isr_err));
                }
            }
            if (s_gpio_isr_service_installed) {
                esp_err_t add_err = gpio_isr_handler_add(
                    PIN_HAT_LA_DONE_IRQ, hat_la_done_isr, NULL);
                if (add_err != ESP_OK) {
                    ESP_LOGW(TAG, "LA-done ISR handler add failed: %s",
                             esp_err_to_name(add_err));
                } else {
                    ESP_LOGI(TAG, "LA-done IRQ armed on GPIO%d (falling edge)",
                             (int)PIN_HAT_LA_DONE_IRQ);
                }
            }
        }
    }
#endif

    s_initialized = true;
    ESP_LOGI(TAG, "HAT subsystem initialized (UART%d: GPIO%d TX, GPIO%d RX, %d baud)",
             HAT_UART_NUM, PIN_HAT_TX, PIN_HAT_RX, HAT_UART_BAUD);

    // Initial detection — with retry to handle the race where the ESP32 boots
    // faster than the RP2040 HAT. The RP2040 takes ~1.5 s to complete its own
    // boot animation before it drives the detect GPIO low. We poll up to
    // HAT_BOOT_RETRY_COUNT times with HAT_BOOT_RETRY_DELAY_MS between attempts.
    // If the detect GPIO is active on the first try we skip straight through.
#ifndef HAT_BOOT_RETRY_COUNT
#define HAT_BOOT_RETRY_COUNT   8
#endif
#ifndef HAT_BOOT_RETRY_DELAY_MS
#define HAT_BOOT_RETRY_DELAY_MS  250
#endif
#ifndef HAT_CONNECT_RETRY_COUNT
#define HAT_CONNECT_RETRY_COUNT   6
#endif
#ifndef HAT_CONNECT_RETRY_DELAY_MS
#define HAT_CONNECT_RETRY_DELAY_MS  500
#endif

    hat_detect();

    if (!s_state.detected) {
        ESP_LOGI(TAG, "HAT not detected yet (%.2fV) — waiting for RP2040 boot (up to %d ms)...",
                 s_state.detect_voltage,
                 HAT_BOOT_RETRY_COUNT * HAT_BOOT_RETRY_DELAY_MS);
        for (int attempt = 1; attempt <= HAT_BOOT_RETRY_COUNT && !s_state.detected; attempt++) {
            vTaskDelay(pdMS_TO_TICKS(HAT_BOOT_RETRY_DELAY_MS));
            hat_detect();
            if (s_state.detected) {
                ESP_LOGI(TAG, "HAT detected on retry %d/%d (%.2fV)",
                         attempt, HAT_BOOT_RETRY_COUNT, s_state.detect_voltage);
            }
        }
    }

    if (s_state.detected) {
        ESP_LOGI(TAG, "HAT detected: %s (%.2fV)", hat_type_name(s_state.type), s_state.detect_voltage);
        for (int attempt = 1; attempt <= HAT_CONNECT_RETRY_COUNT && !s_state.connected; attempt++) {
            if (hat_connect()) {
                ESP_LOGI(TAG, "HAT connected: fw v%d.%d", s_state.fw_version_major, s_state.fw_version_minor);
                break;
            }
            if (attempt < HAT_CONNECT_RETRY_COUNT) {
                ESP_LOGW(TAG, "HAT UART connection attempt %d/%d failed; retrying in %d ms",
                         attempt, HAT_CONNECT_RETRY_COUNT, HAT_CONNECT_RETRY_DELAY_MS);
                vTaskDelay(pdMS_TO_TICKS(HAT_CONNECT_RETRY_DELAY_MS));
            }
        }
        if (!s_state.connected) {
            ESP_LOGW(TAG, "HAT detected but UART connection failed after %d attempts",
                     HAT_CONNECT_RETRY_COUNT);
        }
    } else {
        ESP_LOGI(TAG, "No HAT detected after retries (%.2fV)", s_state.detect_voltage);
    }

    return true;
}

bool hat_detected(void)
{
    return s_state.detected;
}

const HatState* hat_get_state(void)
{
    return &s_state;
}

HatType hat_detect(void)
{
    // If no detect pin (breadboard mode), assume HAT might be present — probe via UART
#if HAT_NO_DETECT
    ESP_LOGI(TAG, "No detect pin — trying UART ping...");
    s_state.detect_voltage = 0.0f;
    s_state.type = HAT_TYPE_SWD_GPIO;  // Assume SWD/GPIO for breadboard test
    s_state.detected = true;            // Will be confirmed/denied by hat_connect()
    return s_state.type;
#else
    int level = hat_read_detect_level();
    if (level < 0) {
        s_state.detected = false;
        s_state.type = HAT_TYPE_UNKNOWN;
        s_state.detect_voltage = -1.0f;
        return HAT_TYPE_UNKNOWN;
    }

    s_state.detect_voltage = level ? 3.3f : 0.0f;
    s_state.type = level ? HAT_TYPE_NONE : HAT_TYPE_SWD_GPIO;
    s_state.detected = (level == 0);

    return s_state.type;
#endif  // HAT_NO_DETECT
}

bool hat_connect(void)
{
    if (!s_initialized || !s_state.detected) return false;

    memset(s_last_sent_color, 0xFF, sizeof(s_last_sent_color));

    // Send PING command
    uint8_t rsp[8] = {};
    uint8_t rsp_len = 0;
    uint8_t cmd = hat_command(HAT_CMD_PING, NULL, 0, rsp, &rsp_len, 200, sizeof(rsp));

    if (cmd != HAT_RSP_OK) {
        s_state.connected = false;
        s_state.degraded = false;
        return false;
    }

    s_state.connected = true;
    hat_note_uart_success();

    // Query HAT info. PING only proves UART liveness; firmware metadata lives
    // in GET_INFO and must be valid before we mark the HAT connected.
    cmd = hat_command(HAT_CMD_GET_INFO, NULL, 0, rsp, &rsp_len, 200, sizeof(rsp));
    if (cmd != HAT_RSP_INFO || rsp_len < 3) {
        ESP_LOGW(TAG, "HAT GET_INFO failed after PING (cmd=0x%02x len=%u)", cmd, rsp_len);
        s_state.connected = false;
        s_state.degraded = false;
        return false;
    }
    if (rsp[1] == 0 && rsp[2] == 0) {
        ESP_LOGW(TAG, "HAT reports sentinel firmware v0.0; rebuild/flash RP2040 from CMake output");
        s_state.connected = false;
        s_state.degraded = true;
        return false;
    }
    s_state.type = (HatType)rsp[0];
    s_state.fw_version_major = rsp[1];
    s_state.fw_version_minor = rsp[2];

    // Query current pin config
    hat_get_pin_config(s_state.pin_config);
    hat_get_caps(&s_state.caps);
    {
        uint8_t rail_count = 0;
        if (hat_get_rail_status(s_state.rail, &rail_count)) {
            hat_seed_3v3_adj_from_esp_cal();
        }
    }

    hat_update_leds();

    return true;
}

// Enum slots 1..4 are reserved (formerly SWDIO/SWCLK/TRACE1/TRACE2).
// SWD now lives on the dedicated 3-pin connector — these function codes
// are no longer assignable to EXP_EXT pins.
static inline bool hat_func_is_reserved(HatPinFunction func)
{
    return (uint8_t)func >= 1 && (uint8_t)func <= 4;
}

bool hat_set_pin(uint8_t ext_pin, HatPinFunction func)
{
    if (!s_state.connected) return false;
    if (ext_pin >= HAT_NUM_EXT_PINS) return false;
    if (func >= HAT_FUNC_COUNT) return false;
    if (hat_func_is_reserved(func)) {
        ESP_LOGW(TAG,
                 "EXP_EXT_%d: function code %u (SWDIO/SWCLK/TRACE) is reserved — "
                 "SWD now uses the dedicated connector, use hat_setup_swd() instead",
                 ext_pin + 1, (unsigned)func);
        s_last_error = HAT_ERR_INVALID_FUNC;
        return false;
    }

    uint8_t payload[2] = { ext_pin, (uint8_t)func };
    uint8_t rsp[4] = {};
    uint8_t rsp_len = 0;

    uint8_t cmd = hat_command(HAT_CMD_SET_PIN_CONFIG, payload, 2, rsp, &rsp_len, 300, sizeof(rsp));
    if (cmd == HAT_RSP_OK) {
        s_state.pin_config[ext_pin] = func;
        s_state.config_confirmed = true;
        ESP_LOGI(TAG, "EXP_EXT_%d → %s (confirmed)", ext_pin + 1, hat_func_name(func));
        hat_update_leds();
        return true;
    }

    ESP_LOGW(TAG, "EXP_EXT_%d config failed (rsp=0x%02X)", ext_pin + 1, cmd);
    s_state.config_confirmed = false;
    return false;
}

bool hat_set_all_pins(const HatPinFunction config[HAT_NUM_EXT_PINS])
{
    if (!s_state.connected) return false;

    uint8_t payload[HAT_NUM_EXT_PINS];
    for (int i = 0; i < HAT_NUM_EXT_PINS; i++) {
        if (config[i] >= HAT_FUNC_COUNT) return false;
        if (hat_func_is_reserved(config[i])) {
            ESP_LOGW(TAG,
                     "EXP_EXT_%d: function code %u reserved (SWD moved to dedicated connector)",
                     i + 1, (unsigned)config[i]);
            s_last_error = HAT_ERR_INVALID_FUNC;
            return false;
        }
        payload[i] = (uint8_t)config[i];
    }

    uint8_t rsp[4] = {};
    uint8_t rsp_len = 0;

    uint8_t cmd = hat_command(HAT_CMD_SET_PIN_CONFIG, payload, HAT_NUM_EXT_PINS, rsp, &rsp_len, 300, sizeof(rsp));
    if (cmd == HAT_RSP_OK) {
        for (int i = 0; i < HAT_NUM_EXT_PINS; i++) {
            s_state.pin_config[i] = config[i];
        }
        s_state.config_confirmed = true;
        ESP_LOGI(TAG, "All EXP_EXT pins configured (confirmed)");
        hat_update_leds();
        return true;
    }

    s_state.config_confirmed = false;
    return false;
}

bool hat_get_pin_config(HatPinFunction config[HAT_NUM_EXT_PINS])
{
    if (!s_state.connected) return false;

    uint8_t rsp[8] = {};
    uint8_t rsp_len = 0;

    uint8_t cmd = hat_command(HAT_CMD_GET_PIN_CONFIG, NULL, 0, rsp, &rsp_len, 200, sizeof(rsp));
    if (cmd == HAT_RSP_OK && rsp_len >= HAT_NUM_EXT_PINS) {
        for (int i = 0; i < HAT_NUM_EXT_PINS; i++) {
            config[i] = (HatPinFunction)rsp[i];
            s_state.pin_config[i] = config[i];
        }
        return true;
    }

    return false;
}

bool hat_reset(void)
{
    if (!s_state.connected) return false;

    uint8_t rsp[4] = {};
    uint8_t rsp_len = 0;

    uint8_t cmd = hat_command(HAT_CMD_RESET, NULL, 0, rsp, &rsp_len, 500, sizeof(rsp));
    if (cmd == HAT_RSP_OK) {
        for (int i = 0; i < HAT_NUM_EXT_PINS; i++) {
            s_state.pin_config[i] = HAT_FUNC_DISCONNECTED;
        }
        s_state.config_confirmed = true;
        ESP_LOGI(TAG, "HAT reset (all pins disconnected)");
        hat_update_leds();
        return true;
    }

    return false;
}

const char* hat_func_name(HatPinFunction func)
{
    switch (func) {
        case HAT_FUNC_DISCONNECTED: return "Disconnected";
        case HAT_FUNC_RESERVED_1:
        case HAT_FUNC_RESERVED_2:
        case HAT_FUNC_RESERVED_3:
        case HAT_FUNC_RESERVED_4:   return "Reserved (deprecated)";
        case HAT_FUNC_GPIO1:        return "GPIO1";
        case HAT_FUNC_GPIO2:        return "GPIO2";
        case HAT_FUNC_GPIO3:        return "GPIO3";
        case HAT_FUNC_GPIO4:        return "GPIO4";
        default:                     return "Unknown";
    }
}

const char* hat_type_name(HatType type)
{
    switch (type) {
        case HAT_TYPE_NONE:      return "None";
        case HAT_TYPE_SWD_GPIO:  return "HAT";
        case HAT_TYPE_UNKNOWN:   return "Unknown";
        default:                 return "Unknown";
    }
}

const char* hat_la_state_name(uint8_t state)
{
    switch (state) {
        case LA_STATE_IDLE:      return "IDLE";
        case LA_STATE_ARMED:     return "ARMED";
        case LA_STATE_CAPTURING: return "CAPTURING";
        case LA_STATE_DONE:      return "DONE";
        case LA_STATE_STREAMING: return "STREAMING";
        case LA_STATE_ERROR:     return "ERROR";
        default:                 return "UNKNOWN";
    }
}

const char* hat_la_stop_reason_name(uint8_t reason)
{
    switch (reason) {
        case LA_STREAM_STOP_NONE:            return "NONE";
        case LA_STREAM_STOP_HOST:            return "HOST";
        case LA_STREAM_STOP_USB_SHORT_WRITE: return "USB_SHORT_WRITE";
        case LA_STREAM_STOP_DMA_OVERRUN:     return "DMA_OVERRUN";
        default:                             return "UNKNOWN";
    }
}

// =============================================================================
// Power Management
// =============================================================================

static bool hat_get_io_voltage(void)
{
    if (!s_state.connected) return false;

    uint8_t rsp[8] = {};
    uint8_t rsp_len = 0;

    uint8_t cmd = hat_command(HAT_CMD_GET_IO_VOLTAGE, NULL, 0, rsp, &rsp_len, 200, sizeof(rsp));
    if (cmd == HAT_RSP_OK && rsp_len >= 2) {
        uint16_t actual_mv = (uint16_t)rsp[0] | ((uint16_t)rsp[1] << 8);
        if (rsp_len >= 4) {
            actual_mv = (uint16_t)rsp[2] | ((uint16_t)rsp[3] << 8);
        }
        s_state.io_voltage_mv = actual_mv;
        if (rsp_len >= 5) {
            s_state.hvpak_part = rsp[4];
        }
        if (rsp_len >= 6) {
            s_state.hvpak_ready = rsp[5] != 0;
        }
        if (rsp_len >= 7) {
            s_state.hvpak_last_error = rsp[6];
        }
        return true;
    }
    return false;
}

bool hat_set_power(HatConnector conn, bool on)
{
    if (!s_state.connected) return false;
    if (conn > HAT_CONNECTOR_B) return false;

    uint8_t payload[2] = { (uint8_t)conn, (uint8_t)(on ? 1 : 0) };
    uint8_t rsp[4] = {};
    uint8_t rsp_len = 0;

    uint8_t cmd = hat_command(HAT_CMD_SET_POWER, payload, 2, rsp, &rsp_len, 300, sizeof(rsp));
    if (cmd == HAT_RSP_OK) {
        s_state.connector[conn].enabled = on;
        ESP_LOGI(TAG, "Connector %c power %s", 'A' + conn, on ? "ON" : "OFF");
        return true;
    }
    ESP_LOGW(TAG, "Connector %c power command failed", 'A' + conn);
    return false;
}

bool hat_get_power_status(void)
{
    if (!s_state.connected) return false;

    uint8_t rsp[16] = {};
    uint8_t rsp_len = 0;

    uint8_t cmd = hat_command(HAT_CMD_GET_POWER_STATUS, NULL, 0, rsp, &rsp_len, 200, sizeof(rsp));
    if (cmd == HAT_RSP_POWER_STATUS && rsp_len >= 6) {
        // Connector A
        s_state.connector[0].enabled = rsp[0] != 0;
        memcpy(&s_state.connector[0].current_ma, &rsp[1], sizeof(float)); // bytes 1-4: float
        s_state.connector[0].fault = rsp[5] != 0;
        // Connector B (if present in response)
        if (rsp_len >= 12) {
            s_state.connector[1].enabled = rsp[6] != 0;
            memcpy(&s_state.connector[1].current_ma, &rsp[7], sizeof(float));
            s_state.connector[1].fault = rsp[11] != 0;
        }
        hat_get_io_voltage();
        return true;
    }
    return false;
}

bool hat_get_caps(HatCaps *caps)
{
    if (!s_state.connected) return false;

    uint8_t rsp[16] = {};
    uint8_t rsp_len = 0;

    uint8_t cmd = hat_command(HAT_CMD_GET_CAPS, NULL, 0, rsp, &rsp_len, 200, sizeof(rsp));
    if (cmd != HAT_RSP_CAPS || rsp_len < 12) {
        return false;
    }

    HatCaps parsed = {};
    size_t p = 0;
    parsed.hw_revision = rsp[p++];
    memcpy(&parsed.flags, &rsp[p], sizeof(parsed.flags)); p += sizeof(parsed.flags);
    parsed.rail_count = rsp[p++];
    parsed.led_count = rsp[p++];
    parsed.shifted_io_count = rsp[p++];
    parsed.la_routes = rsp[p++];
    parsed.fw_major = rsp[p++];
    parsed.fw_minor = rsp[p++];
    parsed.hvpak_present = rsp[p++] != 0;

    s_state.caps = parsed;
    s_state.caps_valid = true;
    if (caps) *caps = parsed;
    return true;
}

bool hat_get_rail_status(HatRailStatus rails[HAT_RAIL_COUNT], uint8_t *rail_count)
{
    if (!s_state.connected) return false;

    uint8_t rsp[32] = {};
    uint8_t rsp_len = 0;

    uint8_t cmd = hat_command(HAT_CMD_GET_RAIL_STATUS, NULL, 0, rsp, &rsp_len, 200, sizeof(rsp));
    if (cmd != HAT_RSP_RAIL_STATUS || rsp_len < 1) {
        return false;
    }

    uint8_t count = rsp[0];
    size_t p = 1;
    uint8_t parsed_count = 0;

    for (uint8_t i = 0; i < count && i < HAT_RAIL_COUNT; i++) {
        if (p + 7 > rsp_len) break;
        HatRailStatus st = {};
        st.rail_id = rsp[p++];
        st.enabled = rsp[p++] != 0;
        memcpy(&st.voltage_mv, &rsp[p], sizeof(st.voltage_mv)); p += sizeof(st.voltage_mv);
        memcpy(&st.current_ma, &rsp[p], sizeof(st.current_ma)); p += sizeof(st.current_ma);
        st.status = rsp[p++];

        if (st.rail_id < HAT_RAIL_COUNT) {
            s_state.rail[st.rail_id] = st;
        }
        if (rails && parsed_count < HAT_RAIL_COUNT) {
            rails[parsed_count] = st;
        }
        parsed_count++;
    }

    if (rail_count) *rail_count = parsed_count;
    return parsed_count > 0;
}

bool hat_set_rail_enable(uint8_t rail_id, bool enable)
{
    if (!s_state.connected) return false;
    if (rail_id >= HAT_RAIL_COUNT) return false;

    uint8_t payload[2] = { rail_id, (uint8_t)(enable ? 1 : 0) };
    uint8_t rsp[32] = {};
    uint8_t rsp_len = 0;

    uint8_t cmd = hat_command(HAT_CMD_SET_RAIL_ENABLE, payload, sizeof(payload),
                              rsp, &rsp_len, 300, sizeof(rsp));
    if (cmd != HAT_RSP_RAIL_STATUS || rsp_len < 1) {
        return false;
    }

    uint8_t count = rsp[0];
    size_t p = 1;
    for (uint8_t i = 0; i < count && i < HAT_RAIL_COUNT; i++) {
        if (p + 7 > rsp_len) break;
        HatRailStatus st = {};
        st.rail_id = rsp[p++];
        st.enabled = rsp[p++] != 0;
        memcpy(&st.voltage_mv, &rsp[p], sizeof(st.voltage_mv)); p += sizeof(st.voltage_mv);
        memcpy(&st.current_ma, &rsp[p], sizeof(st.current_ma)); p += sizeof(st.current_ma);
        st.status = rsp[p++];
        if (st.rail_id < HAT_RAIL_COUNT) {
            s_state.rail[st.rail_id] = st;
        }
    }

    hat_update_leds();

    return true;
}

bool hat_set_led_state(uint8_t led_index, uint8_t color_code)
{
    if (!s_state.connected) return false;

    if (led_index < 9 && s_last_sent_color[led_index] == color_code) {
        return true;
    }

    uint8_t payload[2] = { led_index, color_code };
    uint8_t rsp[4] = {};
    uint8_t rsp_len = 0;

    bool ok = hat_command(HAT_CMD_SET_LED_STATE, payload, sizeof(payload),
                          rsp, &rsp_len, 200, sizeof(rsp)) == HAT_RSP_OK;
    if (ok && led_index < 9) {
        s_last_sent_color[led_index] = color_code;
    }
    return ok;
}

void hat_update_leds(void)
{
    if (!s_state.connected) return;

    // Color legend (applies to all LEDs below):
    //   0 = Off     — no supply, no IO
    //   1 = Red     — EFUSE fault
    //   2 = Green   — supply present + IO/MUX configured
    //   3 = Blue    — supply present, no IO configured
    //   4 = Yellow  — IO configured, no supply

    // 1. LED 2: Conn2 — 3V3_ADJ supply + low-speed IO route
    {
        bool supply = s_state.rail[HAT_RAIL_3V3_ADJ].enabled;
        bool io = false;
        if (s_state.la_route == HAT_LA_ROUTE_LOW_SPEED) {
            for (int i = 0; i < 4; i++) {
                if (s_state.pin_config[i] != HAT_FUNC_DISCONNECTED) { io = true; break; }
            }
        }
        uint8_t c = (supply && io) ? 2 : supply ? 3 : io ? 4 : 0;
        hat_set_led_state(2, c);
    }

    // 2. LED 3: Conn1 — VADJ3 supply + high-speed route
    {
        bool supply = s_state.rail[HAT_RAIL_VADJ3].enabled;
        bool io = (s_state.la_route == HAT_LA_ROUTE_HIGH_SPEED);
        uint8_t c = (supply && io) ? 2 : supply ? 3 : io ? 4 : 0;
        hat_set_led_state(3, c);
    }

    // 3. LEDs 4–7: Mainboard IOBLOCKs 1–4
    // Block 1 (LED 4): EFUSE1 + IO1–IO3.
    // Block 2 (LED 5): EFUSE2 + IO4–IO6.
    // Block 3 (LED 6): EFUSE3 + IO7–IO9.
    // Block 4 (LED 7): EFUSE4 + IO10–IO12.
    {
        const DioState *dio = dio_get_all();
        const PCA9535State *pca = pca9535_present() ? pca9535_get_state() : nullptr;
        static const uint8_t logical_to_led[4] = { 4, 5, 6, 7 };
        for (int j = 0; j < 4; j++) {
            bool fault  = pca && pca->efuse_flt[j];
            bool supply = pca && pca->efuse_en[j];
            bool io     = (dio[3*j].mode     != DIO_MODE_DISABLED) ||
                          (dio[3*j + 1].mode != DIO_MODE_DISABLED) ||
                          (dio[3*j + 2].mode != DIO_MODE_DISABLED);
            uint8_t c = fault ? 1 : (supply && io) ? 2 : supply ? 3 : io ? 4 : 0;
            hat_set_led_state(logical_to_led[j], c);
        }
    }

    // 4. LED 8: SWD Connector — VADJ4 supply + DAP/target active
    {
        bool supply = s_state.rail[HAT_RAIL_VADJ4].enabled;
        bool io     = s_state.dap_connected || s_state.target_detected;
        uint8_t c = (supply && io) ? 2 : supply ? 3 : io ? 4 : 0;
        hat_set_led_state(8, c);
    }
}

bool hat_set_io_voltage(uint16_t mv)
{
    if (!s_state.connected) return false;
    if (mv < 1200 || mv > 5500) {
        ESP_LOGW(TAG, "I/O voltage %u mV out of range (1200-5500)", mv);
        return false;
    }

    uint8_t payload[2] = { (uint8_t)(mv & 0xFF), (uint8_t)(mv >> 8) };
    uint8_t rsp[8] = {};
    uint8_t rsp_len = 0;

    uint8_t cmd = hat_command(HAT_CMD_SET_IO_VOLTAGE, payload, 2, rsp, &rsp_len, 300, sizeof(rsp));
    if (cmd == HAT_RSP_OK && rsp_len >= 2) {
        uint16_t actual_mv = (uint16_t)rsp[0] | ((uint16_t)rsp[1] << 8);
        if (rsp_len >= 4) {
            actual_mv = (uint16_t)rsp[2] | ((uint16_t)rsp[3] << 8);
        }
        s_state.io_voltage_mv = actual_mv;
        if (rsp_len >= 5) {
            s_state.hvpak_part = rsp[4];
        }
        if (rsp_len >= 6) {
            s_state.hvpak_ready = rsp[5] != 0;
        }
        if (rsp_len >= 7) {
            s_state.hvpak_last_error = rsp[6];
        }
        ESP_LOGI(TAG, "I/O voltage set to %u mV (actual %u mV)", mv, actual_mv);
        return true;
    }
    return false;
}

bool hat_la_set_route(uint8_t route)
{
    if (!s_state.connected) return false;

    uint8_t payload[1] = { route };
    uint8_t rsp[4] = {};
    uint8_t rsp_len = 0;

    uint8_t cmd = hat_command(HAT_CMD_LA_SET_ROUTE, payload, sizeof(payload),
                              rsp, &rsp_len, 200, sizeof(rsp));
    if (cmd == HAT_RSP_OK && rsp_len >= 1) {
        s_state.la_route = rsp[0];
        hat_update_leds();
        return true;
    }
    return false;
}

bool hat_calibrate_start(uint8_t rail_id, uint8_t *status_out)
{
    if (!s_state.connected) return false;
    uint8_t payload[1] = { rail_id };
    uint8_t rsp[4] = {};
    uint8_t rsp_len = 0;
    uint8_t cmd = hat_command(HAT_CMD_CALIBRATE_START, payload, sizeof(payload),
                              rsp, &rsp_len, 500, sizeof(rsp));
    if (cmd == HAT_RSP_OK && rsp_len >= 1) {
        if (status_out) *status_out = rsp[0];
        return true;
    }
    return false;
}

bool hat_calibrate_status(uint8_t *state, uint8_t *progress, uint8_t *rail_id,
                          uint8_t *last_error, uint8_t *persist_state,
                          uint8_t *stage, uint8_t *point, int8_t *code,
                          int32_t *measured_mv, int32_t *min_mv,
                          int32_t *max_mv, int32_t *max_gap_mv,
                          int32_t *max_error_mv, uint16_t *validation_flags)
{
    if (!s_state.connected) return false;
    uint8_t rsp[32] = {};
    uint8_t rsp_len = 0;
    uint8_t cmd = hat_command(HAT_CMD_CALIBRATE_STATUS, NULL, 0,
                              rsp, &rsp_len, 500, sizeof(rsp));
    if (cmd == HAT_RSP_CALIBRATE_STATUS && rsp_len >= 12) {
        if (state)       *state       = rsp[0];
        if (progress)    *progress    = rsp[1];
        if (rail_id)     *rail_id     = rsp[2];
        if (last_error)  *last_error  = rsp[3];
        if (persist_state) *persist_state = rsp[4];
        if (stage)       *stage       = rsp[5];
        if (point)       *point       = rsp[6];
        if (code)        *code        = (int8_t)rsp[7];
        if (measured_mv) memcpy(measured_mv, &rsp[8], sizeof(int32_t));
        if (min_mv) {
            int32_t v = -1;
            if (rsp_len >= 16) memcpy(&v, &rsp[12], sizeof(v));
            *min_mv = v;
        }
        if (max_mv) {
            int32_t v = -1;
            if (rsp_len >= 20) memcpy(&v, &rsp[16], sizeof(v));
            *max_mv = v;
        }
        if (max_gap_mv) {
            int32_t v = -1;
            if (rsp_len >= 24) memcpy(&v, &rsp[20], sizeof(v));
            *max_gap_mv = v;
        }
        if (max_error_mv) {
            int32_t v = -1;
            if (rsp_len >= 28) memcpy(&v, &rsp[24], sizeof(v));
            *max_error_mv = v;
        }
        if (validation_flags) {
            uint16_t v = 0;
            if (rsp_len >= 30) memcpy(&v, &rsp[28], sizeof(v));
            *validation_flags = v;
        }
        return true;
    }
    return false;
}

bool hat_set_rail_voltage(uint8_t rail_id, uint16_t mv)
{
    if (!s_state.connected) return false;
    if (rail_id == HAT_RAIL_3V3_ADJ) {
        if (!hat_set_io_voltage(mv)) return false;
        s_state.rail[HAT_RAIL_3V3_ADJ].voltage_mv = s_state.io_voltage_mv;
        return true;
    }
    if (rail_id != HAT_RAIL_VADJ3 && rail_id != HAT_RAIL_VADJ4) {
        ESP_LOGW(TAG, "Rail voltage set rejected for rail %u", rail_id);
        return false;
    }

    uint8_t payload[3] = { rail_id, (uint8_t)(mv & 0xFF), (uint8_t)(mv >> 8) };
    uint8_t rsp[8] = {};
    uint8_t rsp_len = 0;

    uint8_t cmd = hat_command(HAT_CMD_SET_RAIL_VOLTAGE, payload, sizeof(payload),
                              rsp, &rsp_len, 300, sizeof(rsp));
    if (cmd == HAT_RSP_RAIL_STATUS && rsp_len >= 1) {
        uint8_t count = rsp[0];
        size_t p = 1;
        for (uint8_t i = 0; i < count && i < HAT_RAIL_COUNT; i++) {
            if (p + 7 > rsp_len) break;
            HatRailStatus st = {};
            st.rail_id = rsp[p++];
            st.enabled = rsp[p++] != 0;
            memcpy(&st.voltage_mv, &rsp[p], sizeof(st.voltage_mv)); p += sizeof(st.voltage_mv);
            memcpy(&st.current_ma, &rsp[p], sizeof(st.current_ma)); p += sizeof(st.current_ma);
            st.status = rsp[p++];
            if (st.rail_id < HAT_RAIL_COUNT) {
                s_state.rail[st.rail_id] = st;
            }
        }
        return true;
    }
    return false;
}

bool hat_calibrate_import(uint8_t rail_id, uint8_t count, const uint8_t *points_data, size_t data_len)
{
    if (!s_state.connected) return false;
    if (2 + data_len > 32) return false;
    uint8_t payload[32] = {};
    payload[0] = rail_id;
    payload[1] = count;
    if (data_len > 0 && points_data) {
        memcpy(&payload[2], points_data, data_len);
    }
    uint8_t rsp[4] = {};
    uint8_t rsp_len = 0;
    uint8_t cmd = hat_command(HAT_CMD_CALIBRATE_IMPORT, payload, 2 + data_len,
                              rsp, &rsp_len, 500, sizeof(rsp));
    return cmd == HAT_RSP_OK;
}

bool hat_set_io_bank(uint8_t dirs, uint8_t ups, uint8_t dns, uint8_t vals)
{
    if (!s_state.connected) return false;
    uint8_t payload[4] = { dirs, ups, dns, vals };
    uint8_t rsp[4] = {};
    uint8_t rsp_len = 0;
    uint8_t cmd = hat_command(HAT_CMD_SET_IO_BANK, payload, sizeof(payload),
                              rsp, &rsp_len, 300, sizeof(rsp));
    return cmd == HAT_RSP_OK;
}

bool hat_set_level_shift(bool oe, bool dir, bool *oe_out, bool *dir_out)
{
    if (!s_state.connected) return false;
    uint8_t payload[2] = { (uint8_t)(oe ? 1 : 0), (uint8_t)(dir ? 1 : 0) };
    uint8_t rsp[4] = {};
    uint8_t rsp_len = 0;
    uint8_t cmd = hat_command(HAT_CMD_SET_LEVEL_SHIFT, payload, sizeof(payload),
                              rsp, &rsp_len, 300, sizeof(rsp));
    if (cmd == HAT_RSP_OK && rsp_len >= 2) {
        if (oe_out)  *oe_out  = rsp[0] != 0;
        if (dir_out) *dir_out = rsp[1] != 0;
        return true;
    }
    return false;
}

bool hat_fw_begin(uint32_t image_size, uint32_t expected_crc32)
{
    if (!s_state.connected) return false;
    uint8_t payload[8];
    memcpy(payload, &image_size, 4);
    memcpy(payload + 4, &expected_crc32, 4);
    uint8_t rsp[4] = {};
    uint8_t rsp_len = 0;
    uint8_t cmd = hat_command(HAT_CMD_FW_BEGIN, payload, sizeof(payload),
                              rsp, &rsp_len, 15000, sizeof(rsp));
    return cmd == HAT_RSP_OK;
}

bool hat_fw_chunk(uint32_t offset, const uint8_t *data, uint8_t len, uint32_t *ack_offset)
{
    if (!s_state.connected || !data || len == 0 || len > (HAT_FRAME_MAX_LEN - 4)) return false;
    uint8_t payload[HAT_FRAME_MAX_LEN] = {};
    memcpy(payload, &offset, 4);
    memcpy(payload + 4, data, len);
    uint8_t rsp[4] = {};
    uint8_t rsp_len = 0;
    uint8_t cmd = hat_command(HAT_CMD_FW_CHUNK, payload, (uint8_t)(len + 4),
                              rsp, &rsp_len, 1000, sizeof(rsp));
    if (cmd != HAT_RSP_OK || rsp_len < 4) return false;
    if (ack_offset) memcpy(ack_offset, rsp, 4);
    return true;
}

bool hat_fw_commit(void)
{
    if (!s_state.connected) return false;
    uint8_t rsp[4] = {};
    uint8_t rsp_len = 0;
    uint8_t cmd = hat_command(HAT_CMD_FW_COMMIT, NULL, 0, rsp, &rsp_len, 1000, sizeof(rsp));
    return cmd == HAT_RSP_OK;
}

bool hat_fw_status(HatFwUpdateStatus *status)
{
    if (!s_state.connected || !status) return false;
    uint8_t rsp[18] = {};
    uint8_t rsp_len = 0;
    uint8_t cmd = hat_command(HAT_CMD_FW_STATUS, NULL, 0, rsp, &rsp_len, 1000, sizeof(rsp));
    if (cmd != HAT_RSP_OK || rsp_len < sizeof(rsp)) return false;
    size_t p = 0;
    status->state = rsp[p++];
    status->last_error = rsp[p++];
    memcpy(&status->bytes_written, rsp + p, 4); p += 4;
    memcpy(&status->image_size, rsp + p, 4); p += 4;
    memcpy(&status->expected_crc32, rsp + p, 4); p += 4;
    memcpy(&status->actual_crc32, rsp + p, 4);
    return true;
}

bool hat_hvpak_request(uint8_t cmd, const uint8_t *payload, uint8_t payload_len,
                       uint8_t *rsp_payload, uint8_t *rsp_len, uint32_t timeout_ms, uint8_t max_rsp_len)
{
    if (!s_state.connected) return false;
    return hat_command(cmd, payload, payload_len, rsp_payload, rsp_len, timeout_ms, max_rsp_len) == HAT_RSP_OK;
}

uint8_t hat_get_last_error(void)
{
    return s_last_error;
}

bool hat_setup_swd(uint16_t target_voltage_mv, HatConnector connector)
{
    if (!s_state.connected) return false;

    ESP_LOGI(TAG, "SWD quick-setup: %umV on connector %c", target_voltage_mv, 'A' + connector);

    // 1. Set HVPAK I/O voltage to match target
    if (!hat_set_io_voltage(target_voltage_mv)) {
        ESP_LOGW(TAG, "SWD setup: failed to set I/O voltage (hat err=0x%02X, hvpak err=0x%02X). "
                      "Proceeding anyway (breadboard mode?)",
                 s_last_error, s_state.hvpak_last_error);
    }
    delay_ms(50);  // HVPAK stabilization (was 5ms)

    // 2. Enable connector power
    if (!hat_set_power(connector, true)) {
        ESP_LOGE(TAG, "SWD setup: failed to enable connector power");
        return false;
    }
    delay_ms(200);  // Target power-up (was 50ms)

    // 3. SWD routing is no longer done via EXP_EXT pin assignment.
    //    The new HAT PCB (2026-04-09) exposes a dedicated 3-pin SWD
    //    connector (SWDIO/SWCLK/TRACE) wired directly to the RP2040
    //    debugprobe pins. The debugprobe PIO is always running on those
    //    pins, so this function just sets voltage + power + leaves
    //    EXP_EXT alone.
    //    See .omc/specs/deep-interview-swd-exp-ext-cleanup-2026-04-09.md.

    ESP_LOGI(TAG, "SWD setup complete — dedicated SWD connector active, "
                  "connect debug tool to USB CMSIS-DAP");
    return true;
}

// =============================================================================
// SWD Management
// =============================================================================

bool hat_get_dap_status(void)
{
    if (!s_state.connected) return false;

    uint8_t rsp[16] = {};
    uint8_t rsp_len = 0;

    uint8_t cmd = hat_command(HAT_CMD_GET_DAP_STATUS, NULL, 0, rsp, &rsp_len, 200, sizeof(rsp));
    if (cmd == HAT_RSP_DAP_STATUS && rsp_len >= 8) {
        s_state.dap_connected = rsp[0] != 0;
        s_state.target_detected = rsp[1] != 0;
        memcpy(&s_state.target_dpidr, &rsp[2], 4);
        // swd_clock_khz at bytes 6-7 (u16 LE) — stored for display but not in HatState currently
        hat_update_leds();
        return true;
    }
    return false;
}

bool hat_set_swd_clock(uint16_t khz)
{
    if (!s_state.connected) return false;

    uint8_t payload[2] = { (uint8_t)(khz & 0xFF), (uint8_t)(khz >> 8) };
    uint8_t rsp[4] = {};
    uint8_t rsp_len = 0;

    uint8_t cmd = hat_command(HAT_CMD_SET_SWD_CLOCK, payload, 2, rsp, &rsp_len, 200, sizeof(rsp));
    return cmd == HAT_RSP_OK;
}

// =============================================================================
// Logic Analyzer
// =============================================================================

bool hat_la_configure(uint8_t channels, uint32_t rate_hz, uint32_t depth)
{
    if (!s_state.connected) return false;

    // Best effort stop before reconfiguring
    hat_la_stop();

    uint8_t payload[9];
    payload[0] = channels;
    memcpy(&payload[1], &rate_hz, 4);
    memcpy(&payload[5], &depth, 4);

    uint8_t rsp[4] = {};
    uint8_t rsp_len = 0;
    return hat_command(HAT_CMD_LA_CONFIG, payload, 9, rsp, &rsp_len, 500, sizeof(rsp)) == HAT_RSP_OK;
}

bool hat_la_set_trigger(uint8_t type, uint8_t channel)
{
    if (!s_state.connected) return false;
    uint8_t payload[2] = { type, channel };
    uint8_t rsp[4] = {};
    uint8_t rsp_len = 0;
    return hat_command(HAT_CMD_LA_SET_TRIGGER, payload, 2, rsp, &rsp_len, 200, sizeof(rsp)) == HAT_RSP_OK;
}

bool hat_la_arm(void)
{
    if (!s_state.connected) return false;
    uint8_t rsp[4] = {};
    uint8_t rsp_len = 0;
    return hat_command(HAT_CMD_LA_ARM, NULL, 0, rsp, &rsp_len, 200, sizeof(rsp)) == HAT_RSP_OK;
}

bool hat_la_force(void)
{
    if (!s_state.connected) return false;
    uint8_t rsp[4] = {};
    uint8_t rsp_len = 0;
    return hat_command(HAT_CMD_LA_FORCE, NULL, 0, rsp, &rsp_len, 200, sizeof(rsp)) == HAT_RSP_OK;
}

bool hat_la_stop(void)
{
    if (!s_state.connected) return false;
    uint8_t rsp[4] = {};
    uint8_t rsp_len = 0;
    return hat_command(HAT_CMD_LA_STOP, NULL, 0, rsp, &rsp_len, 2000, sizeof(rsp)) == HAT_RSP_OK;
}

bool hat_la_stream_start(void)
{
    if (!s_state.connected) return false;
    uint8_t rsp[4] = {};
    uint8_t rsp_len = 0;
    return hat_command(HAT_CMD_LA_STREAM_START, NULL, 0, rsp, &rsp_len, 2000, sizeof(rsp)) == HAT_RSP_OK;
}

bool hat_la_usb_reset(void)
{
    if (!s_state.connected) return false;
    
    // 1. Flush UART RX to discard bootloader noise or stale data
    hat_uart_flush();

    // 2. Send reset command with a bit more timeout to allow for device-side SIE reset
    uint8_t rsp[4] = {};
    uint8_t rsp_len = 0;
    return hat_command(HAT_CMD_LA_USB_RESET, NULL, 0, rsp, &rsp_len, 500, sizeof(rsp)) == HAT_RSP_OK;
}

bool hat_la_log_enable(bool enable)
{
    if (!s_state.connected) return false;
    uint8_t payload[1] = { static_cast<uint8_t>(enable ? 1u : 0u) };
    uint8_t rsp[4] = {};
    uint8_t rsp_len = 0;
    return hat_command(HAT_CMD_LA_LOG_ENABLE, payload, 1, rsp, &rsp_len, 200, sizeof(rsp)) == HAT_RSP_OK;
}

bool hat_la_get_status(HatLaStatus *status)
{
    if (!s_state.connected || !status) return false;
    uint8_t rsp[28] = {};
    uint8_t rsp_len = 0;
    uint8_t cmd = hat_command(HAT_CMD_LA_GET_STATUS, NULL, 0, rsp, &rsp_len, 500, sizeof(rsp));
    if (cmd == HAT_RSP_LA_STATUS && rsp_len >= 14) {
        status->state = rsp[0];
        status->channels = rsp[1];
        memcpy(&status->samples_captured, &rsp[2], 4);
        memcpy(&status->total_samples, &rsp[6], 4);
        memcpy(&status->actual_rate_hz, &rsp[10], 4);
        if (rsp_len >= 16) {
            status->usb_connected = rsp[14];
            status->usb_mounted = rsp[15];
        }
        if (rsp_len >= 17) {
            status->stream_stop_reason = rsp[16];
        }
        if (rsp_len >= 21) {
            memcpy(&status->stream_overrun_count, &rsp[17], 4);
        }
        if (rsp_len >= 25) {
            memcpy(&status->stream_short_write_count, &rsp[21], 4);
        }
        if (rsp_len >= 26) {
            status->usb_rearm_pending = rsp[25];
        }
        if (rsp_len >= 27) {
            status->usb_rearm_request_count = rsp[26];
        }
        if (rsp_len >= 28) {
            status->usb_rearm_complete_count = rsp[27];
        }
        return true;
    }
    return false;
}

uint8_t hat_la_read_data(uint32_t offset, uint8_t *buf, uint8_t len)
{
    if (!s_state.connected) return 0;
    if (len > 28) len = 28;

    uint8_t payload[6];
    memcpy(&payload[0], &offset, 4);
    payload[4] = (uint8_t)(len & 0xFF);
    payload[5] = (uint8_t)(len >> 8);

    uint8_t rsp[28] = {};
    uint8_t rsp_len = 0;
    uint8_t cmd = hat_command(HAT_CMD_LA_READ_DATA, payload, 6, rsp, &rsp_len, 200, sizeof(rsp));
    if (cmd == HAT_RSP_LA_DATA && rsp_len > 0) {
        memcpy(buf, rsp, rsp_len);
        return rsp_len;
    }
    return 0;
}

// =============================================================================
// Polling for unsolicited messages
// =============================================================================

// Simple non-blocking check for incoming UART frames
void hat_poll(void)
{
    if (!s_initialized || !s_state.connected) return;

#if PIN_HAT_LA_DONE_IRQ >= 0
    // Fast GPIO path: if the dedicated LA-done wire fired, emit BBP event immediately
    // without waiting for the RP2040 to send a status frame over UART.
    if (hat_la_done_consume()) {
        ESP_LOGD(TAG, "LA_DONE IRQ consumed — forwarding to host");
        if (bbpIsActive()) {
            bbpSendEvent(BBP_EVT_LA_DONE, NULL, 0);
        }
        ws_stream_forward(WS_STREAM_LA_META, NULL, 0);
    }
#endif

    // Check if any bytes available on UART without blocking
    size_t buffered = 0;
    uart_get_buffered_data_len(HAT_UART_NUM, &buffered);
    if (buffered == 0) return;

    // Protect UART access
    if (s_hat_mutex == NULL || xSemaphoreTake(s_hat_mutex, 0) != pdTRUE) return;

    // Try to receive a frame with short timeout
    uint8_t rsp[HAT_FRAME_MAX_LEN] = {};
    uint8_t rsp_len = 0;
    uint8_t cmd = hat_recv_frame(rsp, &rsp_len, 20, HAT_FRAME_MAX_LEN);

    xSemaphoreGive(s_hat_mutex);

    if (cmd == 0) return;  // No valid frame or CRC error

    // Handle unsolicited LA log message relay
    if (cmd == HAT_RSP_LA_LOG && rsp_len > 0) {
        if (bbpIsActive()) {
            bbpSendEvent(BBP_EVT_LA_LOG, rsp, rsp_len);
        }
        // Mirror to WiFi WS subscribers (independent of BBP activity).
        ws_stream_forward(WS_STREAM_LA_META, rsp, rsp_len);
        // Push to HTTP polling ring buffer (always).
        {
            char log_line[HAT_LOG_LINE_MAX];
            uint8_t copy_len = rsp_len < (HAT_LOG_LINE_MAX - 1) ? rsp_len : (HAT_LOG_LINE_MAX - 1);
            memcpy(log_line, rsp, copy_len);
            log_line[copy_len] = '\0';
            hat_log_ring_push(log_line);
        }
        return;
    }

    // Handle unsolicited LA status (capture done notification)
    if (cmd == HAT_RSP_LA_STATUS && rsp_len >= 14) {
        uint8_t la_state = rsp[0];
        if (la_state == 3) {  // LA_STATE_DONE
            ESP_LOGI(TAG, "LA capture done (unsolicited notification)");

            // Forward as BBP event to host
            if (bbpIsActive()) {
                // Payload: [state, channels, samples_captured(u32), total_samples(u32), rate(u32)]
                bbpSendEvent(BBP_EVT_LA_DONE, rsp, rsp_len);
            }
            // Mirror to WS subscribers too — same payload format.
            ws_stream_forward(WS_STREAM_LA_META, rsp, rsp_len);
        }
    }
}

// -----------------------------------------------------------------------------
// LA-done IRQ (dedicated GPIO from RP2040 BB_LA_DONE_PIN)
// -----------------------------------------------------------------------------
bool hat_la_done_pending(void)
{
    return s_la_done_pending;
}

bool hat_la_done_consume(void)
{
    // Atomic-enough for this use: the ISR only sets the flag, the task side
    // only clears it, and we tolerate a single missed edge in the unlikely
    // race window. If that ever matters, promote to atomic_exchange.
    if (!s_la_done_pending) return false;
    s_la_done_pending = false;
    return true;
}

// =============================================================================
// RP2040 Debug Log Ring Buffer
// =============================================================================

void hat_log_ring_push(const char *line)
{
    if (!line || !s_log_mutex || !s_log_ring) return;
    if (xSemaphoreTake(s_log_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        char *slot = s_log_ring->lines[s_log_ring->head];
        strncpy(slot, line, HAT_LOG_LINE_MAX - 1);
        slot[HAT_LOG_LINE_MAX - 1] = '\0';
        s_log_ring->head = (s_log_ring->head + 1) % HAT_LOG_RING_SIZE;
        if (s_log_ring->count < HAT_LOG_RING_SIZE) s_log_ring->count++;
        xSemaphoreGive(s_log_mutex);
    }
}

int hat_log_ring_drain(char *out_buf, size_t buf_sz)
{
    if (!out_buf || buf_sz < 4) return -1;
    if (!s_log_mutex || !s_log_ring || xSemaphoreTake(s_log_mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        snprintf(out_buf, buf_sz, "[]");
        return 2;
    }

    // Reconstruct in order: oldest first
    int n = s_log_ring->count;
    int start = ((int)s_log_ring->head - n + HAT_LOG_RING_SIZE) % HAT_LOG_RING_SIZE;

    int pos = 0;
    out_buf[pos++] = '[';
    for (int i = 0; i < n; i++) {
        int idx = (start + i) % HAT_LOG_RING_SIZE;
        const char *ln = s_log_ring->lines[idx];
        if (pos + (int)strlen(ln) + 5 >= (int)buf_sz) break;
        if (i > 0) out_buf[pos++] = ',';
        out_buf[pos++] = '"';
        for (const char *c = ln; *c; c++) {
            if (*c == '"' || *c == '\\') {
                if (pos + 3 >= (int)buf_sz) goto done;
                out_buf[pos++] = '\\';
            }
            if (pos + 2 >= (int)buf_sz) goto done;
            out_buf[pos++] = *c;
        }
        out_buf[pos++] = '"';
    }
done:
    // Clear ring
    s_log_ring->head = 0;
    s_log_ring->count = 0;
    xSemaphoreGive(s_log_mutex);

    if (pos + 2 >= (int)buf_sz) return -1;
    out_buf[pos++] = ']';
    out_buf[pos] = '\0';
    return pos;
}

// end of hat.cpp
