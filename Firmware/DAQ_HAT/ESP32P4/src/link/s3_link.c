// =============================================================================
// s3_link.c — ESP32-S3 mainboard link: HAT-protocol slave on the P4 DAQ board.
// =============================================================================

#include "s3_link.h"
#include <string.h>
#include "driver/uart.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "config.h"
#include "daq_settings.h"
#include "daq_config_registry.h"

static const char *TAG = "s3_link";

#define S3LINK_UART_BUF   1024

uint8_t s3_link_crc8(const uint8_t *data, uint32_t len)
{
    uint8_t crc = 0x00;
    for (uint32_t i = 0; i < len; ++i) {
        crc ^= data[i];
        for (int j = 0; j < 8; ++j) {
            crc = (crc & 0x80) ? (uint8_t)((crc << 1) ^ HATP_CRC_POLY)
                               : (uint8_t)(crc << 1);
        }
    }
    return crc;
}

// Send one HAT-protocol frame: SYNC, LEN, CMD, PAYLOAD, CRC(CMD+PAYLOAD).
static void send_frame(uint8_t cmd, const uint8_t *payload, uint8_t len)
{
    uint8_t frame[3 + HATP_MAX_PAYLOAD + 1];
    if (len > HATP_MAX_PAYLOAD) len = HATP_MAX_PAYLOAD;
    frame[0] = HATP_SYNC;
    frame[1] = len;
    frame[2] = cmd;
    if (len && payload) {
        memcpy(&frame[3], payload, len);
    }
    // CRC over CMD + PAYLOAD.
    uint8_t crc_input[1 + HATP_MAX_PAYLOAD];
    crc_input[0] = cmd;
    if (len && payload) memcpy(&crc_input[1], payload, len);
    frame[3 + len] = s3_link_crc8(crc_input, (uint32_t)len + 1);

    uart_write_bytes(S3LINK_UART_NUM, frame, (size_t)len + 4);
}

static void send_ok(void)    { send_frame(HATP_RSP_OK, NULL, 0); }
static void send_error(void) { send_frame(HATP_RSP_ERROR, NULL, 0); }

// --- Settings/config command handlers (sub-range 0x70..0x7F) ----------------
// These read/write the global authoritative settings store. Changes made here
// are tagged DAQ_SRC_S3 so the notify path does not echo back to the S3.

static void handle_config_get(const uint8_t *payload, uint8_t len)
{
    if (len < 2) { send_error(); return; }
    uint16_t key = (uint16_t)(payload[0] | ((uint16_t)payload[1] << 8));
    uint8_t tlv[DAQ_TLV_HDR_LEN + DAQ_TLV_MAX_VAL];
    int n = daq_settings_encode_one(key, tlv, sizeof(tlv));
    if (n < 0) { send_error(); return; }
    send_frame(HATP_RSP_CONFIG_VALUE, tlv, (uint8_t)n);
}

static void handle_config_set(const uint8_t *payload, uint8_t len)
{
    if (daq_settings_apply_tlv(payload, len, DAQ_SRC_S3)) send_ok();
    else send_error();
}

static void handle_config_get_all(const uint8_t *payload, uint8_t len)
{
    uint8_t start = (len >= 1) ? payload[0] : 0;
    bool incl_secret = (len >= 2) && (payload[1] & HATP_CONFIG_FLAG_SECRET);

    size_t count = 0;
    const daq_setting_schema_t *tbl = daq_config_table(&count);

    uint8_t resp[HATP_MAX_PAYLOAD];
    size_t off = 1;                 // resp[0] reserved for next_idx
    size_t i = start;
    for (; i < count; i++) {
        const daq_setting_schema_t *sc = &tbl[i];
        int n;
        if ((sc->flags & DAQ_F_SECRET) && !incl_secret) {
            n = daq_tlv_encode(resp + off, sizeof(resp) - off, sc->key, sc->type, NULL, 0);
        } else {
            n = daq_settings_encode_one(sc->key, resp + off, sizeof(resp) - off);
        }
        if (n < 0) break;           // does not fit; resume here next call
        off += (size_t)n;
    }
    resp[0] = (i >= count) ? 0xFFu : (uint8_t)i;   // 0xFF == complete
    send_frame(HATP_RSP_CONFIG_VALUE, resp, (uint8_t)off);
}

static void handle_config_schema(const uint8_t *payload, uint8_t len)
{
    if (len < 2) { send_error(); return; }
    uint16_t key = (uint16_t)(payload[0] | ((uint16_t)payload[1] << 8));
    const daq_setting_schema_t *sc = daq_config_schema(key);
    if (!sc) { send_error(); return; }

    // [key u16][type u8][flags u8][min i32][max i32][step i32][def i32]
    // [label_len u8][label bytes].
    uint8_t r[HATP_MAX_PAYLOAD];
    size_t o = 0;
    r[o++] = (uint8_t)(sc->key & 0xFF);
    r[o++] = (uint8_t)(sc->key >> 8);
    r[o++] = sc->type;
    r[o++] = sc->flags;
    const int32_t vals[4] = { sc->min, sc->max, sc->step, sc->def };
    for (int k = 0; k < 4; k++) {
        uint32_t u = (uint32_t)vals[k];
        r[o++] = (uint8_t)(u & 0xFF);
        r[o++] = (uint8_t)((u >> 8) & 0xFF);
        r[o++] = (uint8_t)((u >> 16) & 0xFF);
        r[o++] = (uint8_t)((u >> 24) & 0xFF);
    }
    const char *label = sc->label ? sc->label : "";
    size_t llen = strlen(label);
    if (llen > (size_t)(HATP_MAX_PAYLOAD - o - 1)) llen = HATP_MAX_PAYLOAD - o - 1;
    r[o++] = (uint8_t)llen;
    memcpy(&r[o], label, llen);
    o += llen;
    send_frame(HATP_RSP_CONFIG_SCHEMA, r, (uint8_t)o);
}

static void handle_config_action(const uint8_t *payload, uint8_t len)
{
    if (len < 1) { send_error(); return; }
    if (daq_settings_action(payload[0], DAQ_SRC_S3)) send_ok();
    else send_error();
}

// Dispatch a fully-received, CRC-checked frame.
static void handle_frame(s3_link_t *s, uint8_t cmd, const uint8_t *payload,
                         uint8_t len)
{
    switch (cmd) {
        case HATP_CMD_PING:
            send_ok();
            break;

        case HATP_CMD_GET_INFO: {
            uint8_t info[3] = { HAT_TYPE_DAQ_POWER, S3LINK_FW_MAJOR, S3LINK_FW_MINOR };
            send_frame(HATP_RSP_INFO, info, sizeof(info));
            break;
        }

        case HATP_CMD_GET_CAPS: {
            // Minimal capability blob: HAT type + a feature bitmask.
            //   bit0 current-stream, bit1 source/SMU, bit2 FFT, bit3 markers.
            uint8_t caps[4] = { HAT_TYPE_DAQ_POWER, 0x0F, 0x00, 0x00 };
            send_frame(HATP_RSP_CAPS, caps, sizeof(caps));
            break;
        }

        case HATP_CMD_RESET:
            send_ok();
            // A real reset is handled by the board; ack first.
            break;

        // Settings/config commands -> global settings store.
        case HATP_CMD_CONFIG_GET:     handle_config_get(payload, len);     break;
        case HATP_CMD_CONFIG_SET:     handle_config_set(payload, len);     break;
        case HATP_CMD_CONFIG_GET_ALL: handle_config_get_all(payload, len); break;
        case HATP_CMD_CONFIG_SCHEMA:  handle_config_schema(payload, len);  break;
        case HATP_CMD_CONFIG_ACTION:  handle_config_action(payload, len);  break;

        // DAQ-specific commands -> board callback.
        case HATP_CMD_DAQ_START:
        case HATP_CMD_DAQ_STOP:
        case HATP_CMD_DAQ_SET_SOURCE:
        case HATP_CMD_DAQ_GET_STATUS:
        case HATP_CMD_DAQ_SYNC:
        case HATP_CMD_DAQ_ARM:
        case HATP_CMD_DAQ_MARK:
        case HATP_CMD_DAQ_TELEMETRY:
        case HATP_CMD_SET_CH_LEDS:
        case HATP_CMD_DAQ_CAL_START:
        case HATP_CMD_DAQ_CAL_ACK:
        case HATP_CMD_DAQ_CAL_STATUS:
        case HATP_CMD_DAQ_CAL_ABORT:
        case HATP_CMD_MB_POLL:
        case HATP_CMD_MB_RESULT:
        case HATP_CMD_DAQ_WIFI_STREAM_START:
        case HATP_CMD_DAQ_WIFI_STREAM_STOP:
        case HATP_CMD_DAQ_WIFI_STREAM_INFO:
        case HATP_CMD_DAQ_VDUT_STATUS:
        case HATP_CMD_DAQ_VDUT_ENABLE:
        case HATP_CMD_DAQ_VDUT_SETPOINT:
        case HATP_CMD_DAQ_SET_ACQ_CONFIG: {
            if (!s->cmd_cb) {
                send_error();
                break;
            }
            uint8_t resp[HATP_MAX_PAYLOAD];
            int n = s->cmd_cb(cmd, payload, len, resp, s->cmd_user);
            if (n < 0) {
                send_error();
            } else if (cmd == HATP_CMD_DAQ_GET_STATUS) {
                send_frame(HATP_RSP_DAQ_STATUS, resp, (uint8_t)n);
            } else if (cmd == HATP_CMD_DAQ_CAL_STATUS) {
                send_frame(HATP_RSP_DAQ_CAL_STATUS, resp, (uint8_t)n);
            } else if (cmd == HATP_CMD_MB_POLL) {
                send_frame(HATP_RSP_MB_REQ, resp, (uint8_t)n);
            } else if (cmd == HATP_CMD_DAQ_WIFI_STREAM_INFO) {
                send_frame(HATP_RSP_DAQ_WIFI_STREAM_INFO, resp, (uint8_t)n);
            } else if (cmd == HATP_CMD_DAQ_VDUT_STATUS) {
                send_frame(HATP_RSP_DAQ_VDUT_STATUS, resp, (uint8_t)n);
            } else {
                send_ok();
            }
            break;
        }

        // Version + OTA commands -> board callback. Response code depends on cmd.
        case HATP_CMD_GET_VERSION:
        case HATP_CMD_OTA_BEGIN:
        case HATP_CMD_OTA_DATA:
        case HATP_CMD_OTA_END:
        case HATP_CMD_OTA_ABORT:
        case HATP_CMD_OTA_STATUS:
        case HATP_CMD_OTA_CONFIRM:
        case HATP_CMD_OTA_ROLLBACK:
        case HATP_CMD_STAGE_READ: {
            if (!s->cmd_cb) {
                send_error();
                break;
            }
            uint8_t resp[HATP_MAX_PAYLOAD];
            int n = s->cmd_cb(cmd, payload, len, resp, s->cmd_user);
            if (n < 0) {
                send_error();
            } else if (cmd == HATP_CMD_GET_VERSION) {
                send_frame(HATP_RSP_VERSION, resp, (uint8_t)n);
            } else if (cmd == HATP_CMD_OTA_STATUS) {
                send_frame(HATP_RSP_OTA_STATUS, resp, (uint8_t)n);
            } else if (cmd == HATP_CMD_STAGE_READ) {
                send_frame(HATP_RSP_STAGE_DATA, resp, (uint8_t)n);
            } else {
                send_ok();
            }
            break;
        }

        default:
            send_error();
            break;
    }
}

// Byte-wise frame parser state machine.
typedef enum { ST_SYNC, ST_LEN, ST_CMD, ST_PAYLOAD, ST_CRC } parse_state_t;

static void service_task(void *arg)
{
    s3_link_t *s = (s3_link_t *)arg;
    parse_state_t st = ST_SYNC;
    uint8_t cmd = 0, len = 0, idx = 0;
    uint8_t payload[HATP_MAX_PAYLOAD];
    uint8_t byte;

    while (s->running) {
        int r = uart_read_bytes(S3LINK_UART_NUM, &byte, 1, pdMS_TO_TICKS(100));
        if (r != 1) {
            continue;
        }
        switch (st) {
            case ST_SYNC:
                if (byte == HATP_SYNC) st = ST_LEN;
                break;
            case ST_LEN:
                if (byte > HATP_MAX_PAYLOAD) { st = ST_SYNC; break; }
                len = byte;
                st  = ST_CMD;
                break;
            case ST_CMD:
                cmd = byte;
                idx = 0;
                st  = (len == 0) ? ST_CRC : ST_PAYLOAD;
                break;
            case ST_PAYLOAD:
                payload[idx++] = byte;
                if (idx >= len) st = ST_CRC;
                break;
            case ST_CRC: {
                uint8_t crc_input[1 + HATP_MAX_PAYLOAD];
                crc_input[0] = cmd;
                if (len) memcpy(&crc_input[1], payload, len);
                uint8_t expect = s3_link_crc8(crc_input, (uint32_t)len + 1);
                if (expect == byte) {
                    s->rx_frames++;
                    handle_frame(s, cmd, len ? payload : NULL, len);
                } else {
                    s->crc_errors++;
                }
                st = ST_SYNC;
                break;
            }
        }
    }
    vTaskDelete(NULL);
}

esp_err_t s3_link_init(s3_link_t *s, s3link_cmd_cb_t cmd_cb, void *user)
{
    memset(s, 0, sizeof(*s));
    s->cmd_cb   = cmd_cb;
    s->cmd_user = user;

    uart_config_t uart_cfg = {
        .baud_rate  = S3LINK_BAUD,
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_DISABLE,
        .stop_bits  = UART_STOP_BITS_1,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    esp_err_t err = uart_driver_install(S3LINK_UART_NUM, S3LINK_UART_BUF,
                                        S3LINK_UART_BUF, 0, NULL, 0);
    if (err != ESP_OK) return err;
    uart_param_config(S3LINK_UART_NUM, &uart_cfg);
    uart_set_pin(S3LINK_UART_NUM, S3LINK_TX_PIN, S3LINK_RX_PIN,
                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);

    // IRQ line: open-drain, idle released (high-Z; external pull-up -> HIGH).
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << S3LINK_INT_PIN,
        .mode         = GPIO_MODE_OUTPUT_OD,
    };
    gpio_config(&io);
    gpio_set_level(S3LINK_INT_PIN, 1);   // released
    return ESP_OK;
}

esp_err_t s3_link_start(s3_link_t *s, int task_core, int task_prio)
{
    if (s->running) return ESP_OK;
    s->running = true;
    BaseType_t ok = xTaskCreatePinnedToCore(service_task, "s3_link", 4096, s,
                                             task_prio, &s->task, task_core);
    if (ok != pdPASS) {
        s->running = false;
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "S3 HAT-protocol slave started (UART%d, %d baud)",
             S3LINK_UART_NUM, S3LINK_BAUD);
    return ESP_OK;
}

void s3_link_stop(s3_link_t *s)
{
    if (!s->running) return;
    s->running = false;
    vTaskDelay(pdMS_TO_TICKS(150));
    s->task = NULL;
}

void s3_link_assert_irq(s3_link_t *s)
{
    (void)s;
    gpio_set_level(S3LINK_INT_PIN, 0);   // open-drain assert LOW
}

void s3_link_release_irq(s3_link_t *s)
{
    (void)s;
    gpio_set_level(S3LINK_INT_PIN, 1);   // release (high-Z)
}
