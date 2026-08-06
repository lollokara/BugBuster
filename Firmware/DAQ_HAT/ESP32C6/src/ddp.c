#include "ddp.h"
#include "ddp_proto.h"
#include "config.h"
#include "display.h"
#include "c6_config.h"
#include "npx.h"

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

// Latest mainboard power snapshot returned by the S3 via the settings tunnel.
static ddp_mb_power_t s_mb_power;
static int64_t        s_mb_us = 0;
static bool           s_mb_have = false;

static ddp_mb_scripts_t s_mb_scr;
static int64_t          s_mb_scr_us = 0;
static bool             s_mb_scr_have = false;
static ddp_mb_fwinfo_t  s_mb_fw;
static int64_t          s_mb_fw_us = 0;
static bool             s_mb_fw_have = false;

static ddp_cal_status_t s_cal;
static int64_t          s_cal_us = 0;
static bool             s_cal_have = false;

// Button events relayed from the P4 (buttons moved off the C6). OR-accumulated
// by the RX task; drained by the render loop via ddp_take_buttons().
static volatile uint8_t s_btn_events = 0;

// WiFi streaming mode, set/cleared by DDP_CMD_WIFI_STREAM_MODE from the P4.
static volatile bool s_wifi_stream_mode = false;

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
    case DDP_CMD_BUTTON_EVENT:
        // Front-panel buttons live on the P4 now and are relayed here; OR the
        // bitmask into the accumulator for the render loop to consume.
        if (len >= 1) {
            taskENTER_CRITICAL(&s_mux);
            s_btn_events |= payload[0];
            taskEXIT_CRITICAL(&s_mux);
        }
        send_frame(DDP_RSP_OK, NULL, 0);
        break;
    case DDP_CMD_CONFIG_PUSH:
        // The P4 pushes settings changed elsewhere (S3/desktop/web). Fold them
        // into g_settings + apply local side effects so the menu stays in sync.
        c6_config_apply_push(payload, len);
        send_frame(DDP_RSP_OK, NULL, 0);
        break;
    case DDP_CMD_SET_CH_LEDS:
        // 4 channel-status colour codes from the S3 (relayed by the P4): drive
        // the neopixels in pairs (DAQ_NPX_CHANNEL mode).
        if (len >= 4) npx_set_channel_codes(payload);
        send_frame(DDP_RSP_OK, NULL, 0);
        break;
    case DDP_CMD_MB_RESPONSE:
        // Result of a Main Board Settings request. Power reads/writes carry a
        // ddp_mb_power_t snapshot after [req_type][status].
        if (len >= 2) {
            uint8_t req_type = payload[0];
            if ((req_type == DDP_MB_POWER || req_type == DDP_MB_SET_RAIL ||
                 req_type == DDP_MB_SET_EFUSE || req_type == DDP_MB_SET_RAIL_EN) &&
                len >= 2 + sizeof(ddp_mb_power_t)) {
                taskENTER_CRITICAL(&s_mux);
                memcpy(&s_mb_power, &payload[2], sizeof(s_mb_power));
                s_mb_us = esp_timer_get_time();
                s_mb_have = true;
                taskEXIT_CRITICAL(&s_mux);
            } else if (req_type == DDP_MB_SCRIPTS ||
                       req_type == DDP_MB_SCRIPT_RUN ||
                       req_type == DDP_MB_SCRIPT_STOP) {
                // Variable-length script snapshot: [state][err_len,err]
                // [count]{name_len,name}*. Decode defensively into the cache.
                ddp_mb_scripts_t s = {0};
                const uint8_t *p = &payload[2];
                int rem = (int)len - 2;
                if (rem >= 1) { s.state = *p++; rem--; }
                if (rem >= 1) {
                    int el = *p++; rem--;
                    if (el > rem) el = rem;
                    int cpy = el < (int)sizeof(s.err) - 1 ? el : (int)sizeof(s.err) - 1;
                    memcpy(s.err, p, cpy); s.err[cpy] = 0;
                    p += el; rem -= el;
                }
                if (rem >= 1) {
                    int cnt = *p++; rem--;
                    int slot = 0;
                    for (int i = 0; i < cnt && rem >= 1; i++) {
                        int nl = *p++; rem--;
                        if (nl > rem) nl = rem;
                        if (slot < MB_SCRIPTS_MAX) {
                            int cpy = nl < MB_SCR_NAME_MAX ? nl : MB_SCR_NAME_MAX;
                            memcpy(s.name[slot], p, cpy); s.name[slot][cpy] = 0;
                            slot++;
                        }
                        p += nl; rem -= nl;
                    }
                    s.count = (uint8_t)slot;
                }
                taskENTER_CRITICAL(&s_mux);
                s_mb_scr = s;
                s_mb_scr_us = esp_timer_get_time();
                s_mb_scr_have = true;
                taskEXIT_CRITICAL(&s_mux);
            } else if ((req_type == DDP_MB_FWINFO || req_type == DDP_MB_FW_APPLY) &&
                       len >= 2 + sizeof(ddp_mb_fwinfo_t)) {
                taskENTER_CRITICAL(&s_mux);
                memcpy(&s_mb_fw, &payload[2], sizeof(s_mb_fw));
                s_mb_fw_us = esp_timer_get_time();
                s_mb_fw_have = true;
                taskEXIT_CRITICAL(&s_mux);
            }
        }
        send_frame(DDP_RSP_OK, NULL, 0);
        break;
    case DDP_CMD_WIFI_STREAM_MODE:
        if (len >= 1) s_wifi_stream_mode = payload[0] != 0;
        send_frame(DDP_RSP_OK, NULL, 0);
        break;
    case DDP_CMD_CAL_STATUS:
        // Live DUT-source calibration status from the P4 cal engine.
        if (len >= sizeof(ddp_cal_status_t)) {
            taskENTER_CRITICAL(&s_mux);
            memcpy(&s_cal, payload, sizeof(s_cal));
            s_cal_us = esp_timer_get_time();
            s_cal_have = true;
            taskEXIT_CRITICAL(&s_mux);
        }
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

uint8_t ddp_take_buttons(void)
{
    uint8_t e;
    taskENTER_CRITICAL(&s_mux);
    e = s_btn_events;
    s_btn_events = 0;
    taskEXIT_CRITICAL(&s_mux);
    return e;
}

void ddp_send_config_tlv(const uint8_t *tlvs, uint8_t len)
{
    send_frame(DDP_CMD_CONFIG_SET, tlvs, len);
}

void ddp_send_config_action(uint8_t action_id)
{
    send_frame(DDP_CMD_CONFIG_ACTION, &action_id, 1);
}

void ddp_send_mb_request(uint8_t req_type, const uint8_t *args, uint8_t args_len)
{
    uint8_t buf[1 + 30];
    buf[0] = req_type;
    if (args_len > sizeof(buf) - 1) args_len = sizeof(buf) - 1;
    if (args && args_len) memcpy(&buf[1], args, args_len);
    send_frame(DDP_CMD_MB_REQUEST, buf, (uint8_t)(1 + args_len));
}

bool ddp_get_mb_power(ddp_mb_power_t *out, uint32_t *age_ms)
{
    bool have;
    taskENTER_CRITICAL(&s_mux);
    have = s_mb_have;
    if (out) *out = s_mb_power;
    int64_t rx = s_mb_us;
    taskEXIT_CRITICAL(&s_mux);
    if (age_ms) *age_ms = have ? (uint32_t)((esp_timer_get_time() - rx) / 1000) : 0xFFFFFFFFu;
    return have;
}

bool ddp_get_mb_scripts(ddp_mb_scripts_t *out, uint32_t *age_ms)
{
    bool have;
    taskENTER_CRITICAL(&s_mux);
    have = s_mb_scr_have;
    if (out) *out = s_mb_scr;
    int64_t rx = s_mb_scr_us;
    taskEXIT_CRITICAL(&s_mux);
    if (age_ms) *age_ms = have ? (uint32_t)((esp_timer_get_time() - rx) / 1000) : 0xFFFFFFFFu;
    return have;
}

bool ddp_get_mb_fwinfo(ddp_mb_fwinfo_t *out, uint32_t *age_ms)
{
    bool have;
    taskENTER_CRITICAL(&s_mux);
    have = s_mb_fw_have;
    if (out) *out = s_mb_fw;
    int64_t rx = s_mb_fw_us;
    taskEXIT_CRITICAL(&s_mux);
    if (age_ms) *age_ms = have ? (uint32_t)((esp_timer_get_time() - rx) / 1000) : 0xFFFFFFFFu;
    return have;
}

void ddp_send_cal_ctrl(uint8_t op, uint8_t arg)
{
    uint8_t p[2] = { op, arg };
    send_frame(DDP_CMD_CAL_CTRL, p, sizeof(p));
}

bool ddp_get_cal_status(ddp_cal_status_t *out, uint32_t *age_ms)
{
    bool have;
    taskENTER_CRITICAL(&s_mux);
    have = s_cal_have;
    if (out) *out = s_cal;
    int64_t rx = s_cal_us;
    taskEXIT_CRITICAL(&s_mux);
    if (age_ms) *age_ms = have ? (uint32_t)((esp_timer_get_time() - rx) / 1000) : 0xFFFFFFFFu;
    return have;
}

void ddp_announce_presence(void)
{
    uint8_t info[4] = { DDP_HAT_TYPE, DDP_FW_MAJOR, DDP_FW_MINOR, DDP_PROTO_VERSION };
    send_frame(DDP_RSP_INFO, info, sizeof(info));
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

bool ddp_wifi_stream_mode(void)
{
    return s_wifi_stream_mode;
}

void ddp_send_config(const ddp_config_t *cfg)
{
    send_frame(DDP_CMD_SET_CONFIG, (const uint8_t *)cfg, sizeof(*cfg));
}
