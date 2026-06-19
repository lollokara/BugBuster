#include "ddp.h"
#include "ddp_proto.h"
#include "config.h"
#include "display.h"

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "driver/uart.h"

static const char *TAG = "ddp";

// Serializes TX so frames from the RX task (responses) and the app task
// (config events) never interleave on the wire.
static SemaphoreHandle_t s_tx_lock = NULL;

// Shared latest-measurement snapshot (written by RX task, read by render loop).
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static float    s_v = 0, s_i = 0;
static uint8_t  s_flags = 0;
static int64_t  s_rx_us = 0;
static bool     s_have = false;

// Latest diagnostics snapshot pushed by the P4.
static ddp_diag_t s_diag;
static int64_t    s_diag_us = 0;
static bool       s_diag_have = false;

static uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

static void send_frame(uint8_t cmd, const uint8_t *payload, uint8_t len)
{
    uint8_t buf[4 + DDP_MAX_PAYLOAD];
    buf[0] = DDP_SYNC;
    buf[1] = len;
    buf[2] = cmd;
    if (len) memcpy(&buf[3], payload, len);
    // CRC over CMD + PAYLOAD.
    uint8_t crc_in[1 + DDP_MAX_PAYLOAD];
    crc_in[0] = cmd;
    if (len) memcpy(&crc_in[1], payload, len);
    buf[3 + len] = ddp_crc8(crc_in, 1 + len);

    if (s_tx_lock) xSemaphoreTake(s_tx_lock, portMAX_DELAY);
    uart_write_bytes(DAQ_UART_PORT, (const char *)buf, 4 + len);
    if (s_tx_lock) xSemaphoreGive(s_tx_lock);
}

static void handle_frame(uint8_t cmd, const uint8_t *payload, uint8_t len)
{
    switch (cmd) {
    case DDP_CMD_PING:
        send_frame(DDP_RSP_OK, NULL, 0);
        break;
    case DDP_CMD_GET_INFO: {
        uint8_t info[4] = { DDP_HAT_TYPE, DDP_FW_MAJOR, DDP_FW_MINOR, DDP_PROTO_VERSION };
        send_frame(DDP_RSP_INFO, info, sizeof(info));
        break;
    }
    case DDP_CMD_SET_MEASUREMENT:
        if (len >= sizeof(ddp_measurement_t)) {
            ddp_measurement_t m;
            memcpy(&m, payload, sizeof(m));
            taskENTER_CRITICAL(&s_mux);
            s_v = m.voltage_v;
            s_i = m.current_a;
            s_flags = m.flags;
            s_rx_us = esp_timer_get_time();
            s_have = true;
            taskEXIT_CRITICAL(&s_mux);
            send_frame(DDP_RSP_OK, NULL, 0);
        } else {
            uint8_t e = 1; send_frame(DDP_RSP_ERR, &e, 1);
        }
        break;
    case DDP_CMD_SET_BACKLIGHT:
        if (len >= 1) display_set_backlight(payload[0]);
        send_frame(DDP_RSP_OK, NULL, 0);
        break;
    case DDP_CMD_SET_DIAGNOSTICS:
        if (len >= sizeof(ddp_diag_t)) {
            taskENTER_CRITICAL(&s_mux);
            memcpy(&s_diag, payload, sizeof(s_diag));
            s_diag_us = esp_timer_get_time();
            s_diag_have = true;
            taskEXIT_CRITICAL(&s_mux);
            send_frame(DDP_RSP_OK, NULL, 0);
        } else {
            uint8_t e = 1; send_frame(DDP_RSP_ERR, &e, 1);
        }
        break;
    case DDP_CMD_CLEAR:
        taskENTER_CRITICAL(&s_mux);
        s_have = false; s_flags = 0;
        taskEXIT_CRITICAL(&s_mux);
        send_frame(DDP_RSP_OK, NULL, 0);
        break;
    default: {
        uint8_t e = 0xFF; send_frame(DDP_RSP_ERR, &e, 1);
        break;
    }
    }
}

// Byte-at-a-time frame parser.
static void rx_task(void *arg)
{
    enum { S_SYNC, S_LEN, S_CMD, S_PAYLOAD, S_CRC } st = S_SYNC;
    uint8_t len = 0, cmd = 0, idx = 0;
    uint8_t payload[DDP_MAX_PAYLOAD];

    uint8_t b;
    while (1) {
        int n = uart_read_bytes(DAQ_UART_PORT, &b, 1, pdMS_TO_TICKS(100));
        if (n != 1) continue;
        switch (st) {
        case S_SYNC:
            if (b == DDP_SYNC) st = S_LEN;
            break;
        case S_LEN:
            if (b > DDP_MAX_PAYLOAD) { st = S_SYNC; break; }
            len = b; idx = 0; st = S_CMD;
            break;
        case S_CMD:
            cmd = b;
            st = len ? S_PAYLOAD : S_CRC;
            break;
        case S_PAYLOAD:
            payload[idx++] = b;
            if (idx >= len) st = S_CRC;
            break;
        case S_CRC: {
            uint8_t crc_in[1 + DDP_MAX_PAYLOAD];
            crc_in[0] = cmd;
            if (len) memcpy(&crc_in[1], payload, len);
            if (ddp_crc8(crc_in, 1 + len) == b)
                handle_frame(cmd, payload, len);
            else
                ESP_LOGW(TAG, "CRC mismatch cmd=0x%02X", cmd);
            st = S_SYNC;
            break;
        }
        }
    }
}

void ddp_init(void)
{
    s_tx_lock = xSemaphoreCreateMutex();

    uart_config_t cfg = {
        .baud_rate  = DAQ_UART_BAUD,
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_DISABLE,
        .stop_bits  = UART_STOP_BITS_1,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    ESP_ERROR_CHECK(uart_driver_install(DAQ_UART_PORT, 512, 256, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(DAQ_UART_PORT, &cfg));
    ESP_ERROR_CHECK(uart_set_pin(DAQ_UART_PORT, DAQ_UART_TX_PIN, DAQ_UART_RX_PIN,
                                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
    xTaskCreate(rx_task, "ddp_rx", 4096, NULL, 6, NULL);
    ESP_LOGI(TAG, "DDP slave on UART%d @ %d baud", DAQ_UART_PORT, DAQ_UART_BAUD);
}

bool ddp_get_latest(float *v, float *i, uint8_t *flags, uint32_t *age_ms)
{
    bool have;
    taskENTER_CRITICAL(&s_mux);
    have = s_have;
    if (v) *v = s_v;
    if (i) *i = s_i;
    if (flags) *flags = s_flags;
    int64_t rx = s_rx_us;
    taskEXIT_CRITICAL(&s_mux);
    if (age_ms) *age_ms = have ? (uint32_t)((esp_timer_get_time() - rx) / 1000) : 0xFFFFFFFFu;
    return have;
}

bool ddp_get_diag(ddp_diag_t *out, uint32_t *age_ms)
{
    bool have;
    taskENTER_CRITICAL(&s_mux);
    have = s_diag_have;
    if (out) *out = s_diag;
    int64_t rx = s_diag_us;
    taskEXIT_CRITICAL(&s_mux);
    if (age_ms) *age_ms = have ? (uint32_t)((esp_timer_get_time() - rx) / 1000) : 0xFFFFFFFFu;
    return have;
}

void ddp_send_config(const ddp_config_t *cfg)
{
    send_frame(DDP_CMD_SET_CONFIG, (const uint8_t *)cfg, sizeof(*cfg));
}
