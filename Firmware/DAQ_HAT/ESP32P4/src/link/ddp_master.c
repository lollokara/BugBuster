// =============================================================================
// ddp_master.c — ESP32-P4 DDP master (P4 <-> C6 display link).
// =============================================================================

#include "ddp_master.h"
#include <string.h>
#include "driver/uart.h"
#include "esp_log.h"
#include "config.h"
#include "daq_settings.h"
#include "daq_config_registry.h"
#include "smu_cal.h"

static const char *TAG = "ddp_master";

#define DDP_UART        ((uart_port_t)DAQ_UART_PORT)
#define DDP_UART_BUF    1024

// ---------------------------------------------------------------------------
// Frame TX.
// ---------------------------------------------------------------------------
void ddp_master_send(ddp_master_t *m, uint8_t cmd, const uint8_t *payload,
                     uint8_t len)
{
    if (len > DDP_MAX_PAYLOAD) len = DDP_MAX_PAYLOAD;

    uint8_t frame[3 + DDP_MAX_PAYLOAD + 1];
    frame[0] = DDP_SYNC;
    frame[1] = len;
    frame[2] = cmd;
    if (len && payload) memcpy(&frame[3], payload, len);

    // CRC over CMD + PAYLOAD.
    uint8_t crc_in[1 + DDP_MAX_PAYLOAD];
    crc_in[0] = cmd;
    if (len && payload) memcpy(&crc_in[1], payload, len);
    frame[3 + len] = ddp_crc8(crc_in, (size_t)len + 1);

    if (m->tx_lock) xSemaphoreTake(m->tx_lock, portMAX_DELAY);
    if (m->running) uart_write_bytes(DDP_UART, frame, (size_t)len + 4);
    if (m->tx_lock) xSemaphoreGive(m->tx_lock);
}

uint8_t ddp_master_take_mb_request(ddp_master_t *m, uint8_t *buf, uint8_t cap)
{
    if (!m->mb_req_pending) return 0;
    uint8_t n = m->mb_req_len;
    if (n > cap) n = cap;
    if (n && buf) memcpy(buf, m->mb_req, n);
    m->mb_req_pending = false;   // benign race with handle_rx; re-sent on miss
    return n;
}

uint8_t ddp_master_peek_mb_type(const ddp_master_t *m)
{
    return (m->mb_req_pending && m->mb_req_len >= 1) ? m->mb_req[0] : 0;
}

// ---------------------------------------------------------------------------
// Convenience senders.
// ---------------------------------------------------------------------------
void ddp_master_button_event(ddp_master_t *m, uint8_t events)
{
    if (!events) return;
    ddp_master_send(m, DDP_CMD_BUTTON_EVENT, &events, 1);
}
void ddp_master_set_measurement(ddp_master_t *m, float v, float i, uint8_t flags)
{
    ddp_measurement_t meas = { .voltage_v = v, .current_a = i, .flags = flags };
    ddp_master_send(m, DDP_CMD_SET_MEASUREMENT, (const uint8_t *)&meas, sizeof(meas));
}

void ddp_master_set_diagnostics(ddp_master_t *m, const ddp_diag_t *d)
{
    ddp_master_send(m, DDP_CMD_SET_DIAGNOSTICS, (const uint8_t *)d, sizeof(*d));
}

void ddp_master_set_backlight(ddp_master_t *m, uint8_t level)
{
    ddp_master_send(m, DDP_CMD_SET_BACKLIGHT, &level, 1);
}

void ddp_master_set_wifi_stream_mode(ddp_master_t *m, bool enable)
{
    uint8_t v = enable ? 1 : 0;
    ddp_master_send(m, DDP_CMD_WIFI_STREAM_MODE, &v, 1);
}

void ddp_master_set_ch_leds(ddp_master_t *m, const uint8_t codes[4])
{
    ddp_master_send(m, DDP_CMD_SET_CH_LEDS, codes, 4);
}

void ddp_master_config_push(ddp_master_t *m, const uint8_t *tlvs, uint8_t len)
{
    ddp_master_send(m, DDP_CMD_CONFIG_PUSH, tlvs, len);
}

// ---------------------------------------------------------------------------
// RX: handle C6 -> P4 frames (unsolicited config events + responses).
// ---------------------------------------------------------------------------
// A mainboard request is a "write" (must not be dropped) vs a periodic "read".
static inline bool mb_req_is_write(uint8_t type)
{
    return type == DDP_MB_SET_RAIL || type == DDP_MB_SET_EFUSE ||
           type == DDP_MB_SET_RAIL_EN || type == DDP_MB_SCRIPT_RUN ||
           type == DDP_MB_SCRIPT_STOP;
}

static void handle_rx(ddp_master_t *m, uint8_t cmd, const uint8_t *payload,
                      uint8_t len)
{
    switch (cmd) {
        case DDP_CMD_CONFIG_SET: {
            // One or more TLVs from a C6 menu edit. Apply each to the store,
            // tagged DAQ_SRC_C6 so the notify path doesn't echo back to the C6.
            size_t off = 0;
            while (off < len) {
                uint16_t key; uint8_t type, vlen; const uint8_t *val;
                int used = daq_tlv_parse(payload + off, len - off, &key, &type, &val, &vlen);
                if (used < 0) break;
                daq_settings_apply_tlv(payload + off, len - off, DAQ_SRC_C6);
                off += (size_t)used;
            }
            break;
        }
        case DDP_CMD_SET_CONFIG: {
            // Legacy fixed snapshot from older C6 builds: map to registry keys.
            if (len >= sizeof(ddp_config_t)) {
                const ddp_config_t *c = (const ddp_config_t *)payload;
                daq_settings_set_i32(DAQ_K_AUTORANGING,    c->autoranging,     DAQ_SRC_C6);
                daq_settings_set_i32(DAQ_K_RANGE_IDX,      c->range_idx,       DAQ_SRC_C6);
                daq_settings_set_i32(DAQ_K_SAMPLE_RATE_IDX,c->sample_rate_idx, DAQ_SRC_C6);
                daq_settings_set_i32(DAQ_K_DUT_ILIMIT_MA,  c->dut_current_ma,  DAQ_SRC_C6);
                daq_settings_set_i32(DAQ_K_DUT_VOLTAGE_MV, c->dut_voltage_mv,  DAQ_SRC_C6);
                daq_settings_set_i32(DAQ_K_BRIGHTNESS_PCT, c->brightness_pct,  DAQ_SRC_C6);
                daq_settings_set_i32(DAQ_K_DARK_MODE,      c->dark_mode,       DAQ_SRC_C6);
            }
            break;
        }
        case DDP_RSP_INFO:
            if (len >= 4) {
                m->c6_present  = true;
                m->c6_fw_major = payload[1];
                m->c6_fw_minor = payload[2];
            }
            break;
        case DDP_CMD_MB_REQUEST:
            // The C6 Main Board Settings menu wants to read/write an S3 resource.
            // Cache it for the S3 poll (HATP_CMD_MB_POLL); one-deep overwrite.
            // The Power menu also issues periodic POWER *reads*; never let one of
            // those clobber a not-yet-claimed *write* (SET_*), or the user's
            // toggle would be silently dropped.
            if (len > 0 && len <= sizeof(m->mb_req)) {
                bool new_is_write = mb_req_is_write(payload[0]);
                bool pending_write = m->mb_req_pending && mb_req_is_write(m->mb_req[0]);
                if (!(pending_write && !new_is_write)) {
                    memcpy(m->mb_req, payload, len);
                    m->mb_req_len = len;
                    m->mb_req_pending = true;
                }
            }
            break;
        case DDP_CMD_CAL_CTRL:
            // DUT source calibration control from the C6 wizard. Acts directly
            // on the P4's background cal engine and answers STATUS requests.
            if (m->cal && len >= 1) {
                smu_cal_t *c = (smu_cal_t *)m->cal;
                uint8_t op  = payload[0];
                uint8_t arg = (len >= 2) ? payload[1] : 0;
                switch (op) {
                    case DDP_CAL_OP_START: smu_cal_start(c, (smu_cal_mode_t)arg); break;
                    case DDP_CAL_OP_ACK:   smu_cal_ack(c);   break;
                    case DDP_CAL_OP_ABORT: smu_cal_abort(c); break;
                    default: break;
                }
                // Always answer with a fresh status snapshot.
                smu_cal_status_t st;
                smu_cal_get_status(c, &st);
                ddp_master_send(m, DDP_CMD_CAL_STATUS,
                                (const uint8_t *)&st, sizeof(st));
            }
            break;
        case DDP_RSP_OK:
        case DDP_RSP_ERR:
        default:
            break;   // responses to our pushes — nothing to do
    }
}

typedef enum { ST_SYNC, ST_LEN, ST_CMD, ST_PAYLOAD, ST_CRC } parse_state_t;

static void rx_task(void *arg)
{
    ddp_master_t *m = (ddp_master_t *)arg;
    parse_state_t st = ST_SYNC;
    uint8_t cmd = 0, len = 0, idx = 0;
    uint8_t payload[DDP_MAX_PAYLOAD];
    uint8_t byte;

    while (m->running) {
        int r = uart_read_bytes(DDP_UART, &byte, 1, pdMS_TO_TICKS(100));
        if (r != 1) continue;
        switch (st) {
            case ST_SYNC:
                if (byte == DDP_SYNC) st = ST_LEN;
                break;
            case ST_LEN:
                if (byte > DDP_MAX_PAYLOAD) { st = ST_SYNC; break; }
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
                uint8_t crc_in[1 + DDP_MAX_PAYLOAD];
                crc_in[0] = cmd;
                if (len) memcpy(&crc_in[1], payload, len);
                if (ddp_crc8(crc_in, (size_t)len + 1) == byte) {
                    m->rx_frames++;
                    handle_rx(m, cmd, len ? payload : NULL, len);
                } else {
                    m->crc_errors++;
                }
                st = ST_SYNC;
                break;
            }
        }
    }
    vTaskDelete(NULL);
}

// ---------------------------------------------------------------------------
// Init / start / stop.
// ---------------------------------------------------------------------------
esp_err_t ddp_master_init(ddp_master_t *m)
{
    memset(m, 0, sizeof(*m));
    m->tx_lock = xSemaphoreCreateMutex();

    uart_config_t cfg = {
        .baud_rate  = DAQ_UART_BAUD,
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_DISABLE,
        .stop_bits  = UART_STOP_BITS_1,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    esp_err_t err = uart_driver_install(DDP_UART, DDP_UART_BUF, DDP_UART_BUF, 0, NULL, 0);
    if (err != ESP_OK) return err;
    uart_param_config(DDP_UART, &cfg);
    uart_set_pin(DDP_UART, DAQ_UART_TX_PIN, DAQ_UART_RX_PIN,
                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    return ESP_OK;
}

esp_err_t ddp_master_start(ddp_master_t *m, int task_core, int task_prio)
{
    if (m->running) return ESP_OK;
    m->running = true;
    BaseType_t ok = xTaskCreatePinnedToCore(rx_task, "ddp_master", 4096, m,
                                             task_prio, &m->task, task_core);
    if (ok != pdPASS) {
        m->running = false;
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "DDP master started (UART%d, %d baud)", DAQ_UART_PORT, DAQ_UART_BAUD);
    return ESP_OK;
}

void ddp_master_stop(ddp_master_t *m)
{
    if (!m->running) return;
    m->running = false;
    vTaskDelay(pdMS_TO_TICKS(150));
    m->task = NULL;
}

void ddp_master_deinit(ddp_master_t *m)
{
    ddp_master_stop(m);                 // running=false, RX task exits
    // Serialise with any in-flight sender, then release UART2 + the lock so a
    // clean ddp_master_init() can recreate them after flashing.
    if (m->tx_lock) xSemaphoreTake(m->tx_lock, portMAX_DELAY);
    uart_driver_delete(DDP_UART);
    if (m->tx_lock) {
        SemaphoreHandle_t lock = m->tx_lock;
        m->tx_lock = NULL;
        xSemaphoreGive(lock);
        vSemaphoreDelete(lock);
    }
}
