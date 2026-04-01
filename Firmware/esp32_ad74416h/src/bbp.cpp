// =============================================================================
// bbp.cpp - BugBuster Binary Protocol implementation
//
// COBS-framed binary protocol over USB CDC #0.
// Handles command dispatch, response building, and ADC/scope streaming.
// =============================================================================

#include "bbp.h"
#include "usb_cdc.h"
#include "tasks.h"
#include "config.h"
#include "uart_bridge.h"
#include "adgs2414d.h"
#include "dio.h"
#include "selftest.h"
#include "ds4424.h"
#include "husb238.h"
#include "pca9535.h"
#include "wifi_manager.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "esp_log.h"

#include <string.h>
#include <math.h>

static const char *TAG = "bbp";

// -----------------------------------------------------------------------------
// Internal state
// -----------------------------------------------------------------------------

static AD74416H     *s_dev = nullptr;
static AD74416H_SPI *s_spi = nullptr;

static bool     s_active = false;       // Binary mode active
static uint16_t s_evtSeq = 0;          // Event sequence counter

// Handshake detection state
static uint8_t  s_magic_idx = 0;
static const uint8_t s_magic[BBP_MAGIC_LEN] = {
    BBP_MAGIC_0, BBP_MAGIC_1, BBP_MAGIC_2, BBP_MAGIC_3
};

// ADC stream state
static uint8_t  s_adcStreamMask = 0;    // 0 = inactive
static uint8_t  s_adcStreamDiv  = 1;
static uint8_t  s_adcDivCount   = 0;    // Divider counter

// Scope stream state
static bool     s_scopeStreamActive = false;
static uint16_t s_scopeLastSeq = 0;     // Last scope seq sent

// ADC stream ring buffer (lock-free SPSC)
static BbpAdcStreamBuf s_adcBuf = {};

// RX frame accumulator
#define RX_BUF_SIZE  (BBP_COBS_MAX + 16)
static uint8_t  s_rxBuf[RX_BUF_SIZE];
static uint16_t s_rxLen = 0;

// TX work buffers
static uint8_t  s_msgBuf[BBP_MAX_PAYLOAD];  // Raw message assembly
static uint8_t  s_cobsBuf[BBP_COBS_MAX];    // COBS-encoded output

// ADC batch buffer for streaming
#define ADC_BATCH_MAX  50
static BbpAdcSample s_adcBatch[ADC_BATCH_MAX];

// Timeout: if no valid frame within 5s after handshake, revert
static uint32_t s_lastFrameMs = 0;
#define BBP_IDLE_TIMEOUT_MS  5000

// -----------------------------------------------------------------------------
// COBS Codec
// -----------------------------------------------------------------------------

size_t bbp_cobs_encode(const uint8_t *input, size_t length, uint8_t *output)
{
    size_t read_idx  = 0;
    size_t write_idx = 1;
    size_t code_idx  = 0;
    uint8_t code     = 1;

    while (read_idx < length) {
        if (input[read_idx] == 0x00) {
            output[code_idx] = code;
            code_idx = write_idx++;
            code = 1;
        } else {
            output[write_idx++] = input[read_idx];
            code++;
            if (code == 0xFF) {
                output[code_idx] = code;
                code_idx = write_idx++;
                code = 1;
            }
        }
        read_idx++;
    }
    output[code_idx] = code;
    return write_idx;
}

size_t bbp_cobs_decode(const uint8_t *input, size_t length, uint8_t *output)
{
    size_t read_idx  = 0;
    size_t write_idx = 0;

    while (read_idx < length) {
        uint8_t code = input[read_idx++];
        if (code == 0) break;  // Invalid in COBS stream
        for (uint8_t i = 1; i < code && read_idx < length; i++) {
            output[write_idx++] = input[read_idx++];
        }
        // Add implicit zero delimiter between groups, but NOT after the last group
        if (code != 0xFF && read_idx < length) {
            output[write_idx++] = 0x00;
        }
    }
    return write_idx;
}

// -----------------------------------------------------------------------------
// CRC-16/CCITT
// -----------------------------------------------------------------------------

uint16_t bbp_crc16(const uint8_t *data, size_t len)
{
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < len; i++) {
        crc ^= (uint16_t)data[i] << 8;
        for (int j = 0; j < 8; j++) {
            crc = (crc & 0x8000) ? (crc << 1) ^ 0x1021 : crc << 1;
        }
    }
    return crc;
}

// -----------------------------------------------------------------------------
// Low-level helpers: encode values into buffer (little-endian)
// -----------------------------------------------------------------------------

static inline void put_u8(uint8_t *buf, size_t *pos, uint8_t v)
{
    buf[(*pos)++] = v;
}

static inline void put_u16(uint8_t *buf, size_t *pos, uint16_t v)
{
    buf[(*pos)++] = (uint8_t)(v & 0xFF);
    buf[(*pos)++] = (uint8_t)(v >> 8);
}

static inline void put_u24(uint8_t *buf, size_t *pos, uint32_t v)
{
    buf[(*pos)++] = (uint8_t)(v & 0xFF);
    buf[(*pos)++] = (uint8_t)((v >> 8) & 0xFF);
    buf[(*pos)++] = (uint8_t)((v >> 16) & 0xFF);
}

static inline void put_u32(uint8_t *buf, size_t *pos, uint32_t v)
{
    buf[(*pos)++] = (uint8_t)(v & 0xFF);
    buf[(*pos)++] = (uint8_t)((v >> 8) & 0xFF);
    buf[(*pos)++] = (uint8_t)((v >> 16) & 0xFF);
    buf[(*pos)++] = (uint8_t)((v >> 24) & 0xFF);
}

static inline void put_f32(uint8_t *buf, size_t *pos, float v)
{
    uint32_t bits;
    memcpy(&bits, &v, 4);
    put_u32(buf, pos, bits);
}

static inline void put_bool(uint8_t *buf, size_t *pos, bool v)
{
    buf[(*pos)++] = v ? 0x01 : 0x00;
}

// Read helpers
static inline uint8_t get_u8(const uint8_t *buf, size_t *pos)
{
    return buf[(*pos)++];
}

static inline uint16_t get_u16(const uint8_t *buf, size_t *pos)
{
    uint16_t v = buf[*pos] | ((uint16_t)buf[*pos + 1] << 8);
    *pos += 2;
    return v;
}

static inline uint32_t get_u32(const uint8_t *buf, size_t *pos)
{
    uint32_t v = buf[*pos] | ((uint32_t)buf[*pos+1] << 8) |
                 ((uint32_t)buf[*pos+2] << 16) | ((uint32_t)buf[*pos+3] << 24);
    *pos += 4;
    return v;
}

static inline float get_f32(const uint8_t *buf, size_t *pos)
{
    uint32_t bits = get_u32(buf, pos);
    float v;
    memcpy(&v, &bits, 4);
    return v;
}

static inline bool get_bool(const uint8_t *buf, size_t *pos)
{
    return buf[(*pos)++] != 0;
}

// -----------------------------------------------------------------------------
// Send a raw COBS-framed message over CDC #0
// -----------------------------------------------------------------------------

static void sendFrame(const uint8_t *msg, size_t msgLen)
{
    size_t cobsLen = bbp_cobs_encode(msg, msgLen, s_cobsBuf);
    s_cobsBuf[cobsLen] = BBP_FRAME_DELIMITER;
    usb_cdc_cli_write(s_cobsBuf, cobsLen + 1);
}

// Build and send a response/event/error message
static void sendMsg(uint8_t msgType, uint16_t seq, uint8_t cmdId,
                    const uint8_t *payload, size_t payloadLen)
{
    size_t pos = 0;
    put_u8(s_msgBuf, &pos, msgType);
    put_u16(s_msgBuf, &pos, seq);
    put_u8(s_msgBuf, &pos, cmdId);
    if (payload && payloadLen > 0) {
        if (pos + payloadLen + 2 > BBP_MAX_PAYLOAD) {
            ESP_LOGW(TAG, "sendMsg: payload too large (%u + %u > %u)", (unsigned)pos, (unsigned)payloadLen, BBP_MAX_PAYLOAD);
            return;
        }
        memcpy(s_msgBuf + pos, payload, payloadLen);
        pos += payloadLen;
    }
    uint16_t crc = bbp_crc16(s_msgBuf, pos);
    put_u16(s_msgBuf, &pos, crc);
    sendFrame(s_msgBuf, pos);
}

static void sendResponse(uint16_t seq, uint8_t cmdId,
                          const uint8_t *payload, size_t payloadLen)
{
    sendMsg(BBP_MSG_RSP, seq, cmdId, payload, payloadLen);
}

static void sendError(uint16_t seq, uint8_t cmdId, uint8_t errCode)
{
    uint8_t payload[2] = { errCode, cmdId };
    sendMsg(BBP_MSG_ERR, seq, cmdId, payload, 2);
}

static void sendEvent(uint8_t evtId, const uint8_t *payload, size_t payloadLen)
{
    sendMsg(BBP_MSG_EVT, s_evtSeq++, evtId, payload, payloadLen);
}

// -----------------------------------------------------------------------------
// Command handlers
// Each returns the response payload length written into `out`, or -1 on error.
// `out` points to a buffer of at least BBP_MAX_PAYLOAD bytes.
// -----------------------------------------------------------------------------

static int handleGetStatus(uint16_t seq, uint8_t cmdId, uint8_t *out)
{
    size_t pos = 0;

    if (xSemaphoreTake(g_stateMutex, pdMS_TO_TICKS(50)) != pdTRUE) {
        sendError(seq, cmdId, BBP_ERR_BUSY);
        return -1;
    }

    put_bool(out, &pos, g_deviceState.spiOk);
    put_f32(out, &pos, g_deviceState.dieTemperature);
    put_u16(out, &pos, g_deviceState.alertStatus);
    put_u16(out, &pos, g_deviceState.alertMask);
    put_u16(out, &pos, g_deviceState.supplyAlertStatus);
    put_u16(out, &pos, g_deviceState.supplyAlertMask);
    put_u16(out, &pos, g_deviceState.liveStatus);

    for (uint8_t ch = 0; ch < 4; ch++) {
        const ChannelState &cs = g_deviceState.channels[ch];
        put_u8(out, &pos, ch);
        put_u8(out, &pos, (uint8_t)cs.function);
        put_u24(out, &pos, cs.adcRawCode);
        put_f32(out, &pos, cs.adcValue);
        put_u8(out, &pos, (uint8_t)cs.adcRange);
        put_u8(out, &pos, (uint8_t)cs.adcRate);
        put_u8(out, &pos, (uint8_t)cs.adcMux);
        put_u16(out, &pos, cs.dacCode);
        put_f32(out, &pos, cs.dacValue);
        put_bool(out, &pos, cs.dinState);
        put_u32(out, &pos, cs.dinCounter);
        put_bool(out, &pos, cs.doState);
        put_u16(out, &pos, cs.channelAlertStatus);
        put_u16(out, &pos, cs.rtdExcitationUa);
    }

    // Diagnostic slots
    for (uint8_t d = 0; d < 4; d++) {
        put_u8(out, &pos, g_deviceState.diag[d].source);
        put_u16(out, &pos, g_deviceState.diag[d].rawCode);
        put_f32(out, &pos, g_deviceState.diag[d].value);
    }

    // MUX switch states
    for (uint8_t m = 0; m < ADGS_NUM_DEVICES; m++) {
        put_u8(out, &pos, g_deviceState.muxState[m]);
    }
    // Pad to 4 if fewer devices
    for (uint8_t m = ADGS_NUM_DEVICES; m < 4; m++) {
        put_u8(out, &pos, 0);
    }

    xSemaphoreGive(g_stateMutex);
    return (int)pos;
}

static int handleGetDeviceInfo(uint16_t seq, uint8_t cmdId, uint8_t *out)
{
    size_t pos = 0;

    uint16_t siliconRev = 0, id0 = 0, id1 = 0;
    bool spiOk = true;
    spiOk &= s_spi->readRegister(0x46, &siliconRev);  // SILICON_REV
    spiOk &= s_spi->readRegister(0x47, &id0);          // SILICON_ID0
    spiOk &= s_spi->readRegister(0x48, &id1);          // SILICON_ID1

    if (!spiOk) {
        sendError(seq, cmdId, BBP_ERR_SPI_FAIL);
        return -1;
    }

    put_bool(out, &pos, g_deviceState.spiOk);
    put_u8(out, &pos, (uint8_t)(siliconRev & 0xFF));
    put_u16(out, &pos, id0);
    put_u16(out, &pos, id1);
    return (int)pos;
}

static int handleGetFaults(uint16_t seq, uint8_t cmdId, uint8_t *out)
{
    size_t pos = 0;
    if (xSemaphoreTake(g_stateMutex, pdMS_TO_TICKS(50)) != pdTRUE) {
        sendError(seq, cmdId, BBP_ERR_BUSY);
        return -1;
    }

    put_u16(out, &pos, g_deviceState.alertStatus);
    put_u16(out, &pos, g_deviceState.alertMask);
    put_u16(out, &pos, g_deviceState.supplyAlertStatus);
    put_u16(out, &pos, g_deviceState.supplyAlertMask);

    for (uint8_t ch = 0; ch < 4; ch++) {
        put_u8(out, &pos, ch);
        put_u16(out, &pos, g_deviceState.channels[ch].channelAlertStatus);
        put_u16(out, &pos, g_deviceState.channels[ch].channelAlertMask);
    }

    xSemaphoreGive(g_stateMutex);
    return (int)pos;
}

static int handleGetDiagnostics(uint16_t seq, uint8_t cmdId, uint8_t *out)
{
    size_t pos = 0;
    if (xSemaphoreTake(g_stateMutex, pdMS_TO_TICKS(50)) != pdTRUE) {
        sendError(seq, cmdId, BBP_ERR_BUSY);
        return -1;
    }

    for (uint8_t d = 0; d < 4; d++) {
        put_u8(out, &pos, d);
        put_u8(out, &pos, g_deviceState.diag[d].source);
        put_u16(out, &pos, g_deviceState.diag[d].rawCode);
        put_f32(out, &pos, g_deviceState.diag[d].value);
    }

    xSemaphoreGive(g_stateMutex);
    return (int)pos;
}

static int handleGetAdcValue(uint16_t seq, uint8_t cmdId,
                              const uint8_t *payload, size_t len, uint8_t *out)
{
    if (len < 1) { sendError(seq, cmdId, BBP_ERR_INVALID_PARAM); return -1; }
    size_t rpos = 0;
    uint8_t ch = get_u8(payload, &rpos);
    if (ch >= 4) { sendError(seq, cmdId, BBP_ERR_INVALID_CH); return -1; }

    size_t pos = 0;
    if (xSemaphoreTake(g_stateMutex, pdMS_TO_TICKS(50)) != pdTRUE) {
        sendError(seq, cmdId, BBP_ERR_BUSY);
        return -1;
    }

    const ChannelState &cs = g_deviceState.channels[ch];
    put_u8(out, &pos, ch);
    put_u24(out, &pos, cs.adcRawCode);
    put_f32(out, &pos, cs.adcValue);
    put_u8(out, &pos, (uint8_t)cs.adcRange);
    put_u8(out, &pos, (uint8_t)cs.adcRate);
    put_u8(out, &pos, (uint8_t)cs.adcMux);

    xSemaphoreGive(g_stateMutex);
    return (int)pos;
}

static int handleGetDacReadback(uint16_t seq, uint8_t cmdId,
                                 const uint8_t *payload, size_t len, uint8_t *out)
{
    if (len < 1) { sendError(seq, cmdId, BBP_ERR_INVALID_PARAM); return -1; }
    size_t rpos = 0;
    uint8_t ch = get_u8(payload, &rpos);
    if (ch >= 4) { sendError(seq, cmdId, BBP_ERR_INVALID_CH); return -1; }

    uint16_t active = s_dev->getDacActive(ch);
    size_t pos = 0;
    put_u8(out, &pos, ch);
    put_u16(out, &pos, active);
    return (int)pos;
}

static int handleGetGpioStatus(uint16_t seq, uint8_t cmdId, uint8_t *out)
{
    size_t pos = 0;
    if (xSemaphoreTake(g_stateMutex, pdMS_TO_TICKS(50)) != pdTRUE) {
        sendError(seq, cmdId, BBP_ERR_BUSY);
        return -1;
    }

    for (uint8_t g = 0; g < 6; g++) {
        put_u8(out, &pos, g);
        put_u8(out, &pos, g_deviceState.gpio[g].mode);
        put_bool(out, &pos, g_deviceState.gpio[g].outputVal);
        put_bool(out, &pos, g_deviceState.gpio[g].inputVal);
        put_bool(out, &pos, g_deviceState.gpio[g].pulldown);
    }

    xSemaphoreGive(g_stateMutex);
    return (int)pos;
}

static int handleGetUartConfig(uint16_t seq, uint8_t cmdId, uint8_t *out)
{
    size_t pos = 0;
    put_u8(out, &pos, CDC_BRIDGE_COUNT);

    for (int i = 0; i < CDC_BRIDGE_COUNT; i++) {
        UartBridgeConfig cfg;
        uart_bridge_get_config(i, &cfg);
        put_u8(out, &pos, (uint8_t)i);
        put_u8(out, &pos, cfg.uart_num);
        put_u8(out, &pos, (uint8_t)cfg.tx_pin);
        put_u8(out, &pos, (uint8_t)cfg.rx_pin);
        put_u32(out, &pos, cfg.baudrate);
        put_u8(out, &pos, cfg.data_bits);
        put_u8(out, &pos, cfg.parity);
        put_u8(out, &pos, cfg.stop_bits);
        put_bool(out, &pos, cfg.enabled);
    }
    return (int)pos;
}

static int handleGetUartPins(uint16_t seq, uint8_t cmdId, uint8_t *out)
{
    int pins[40];
    int count = uart_bridge_get_available_pins(pins, 40);
    size_t pos = 0;
    put_u8(out, &pos, (uint8_t)count);
    for (int i = 0; i < count; i++) {
        put_u8(out, &pos, (uint8_t)pins[i]);
    }
    return (int)pos;
}

static int handleSetRtdConfig(uint16_t seq, uint8_t cmdId,
                               const uint8_t *payload, size_t len, uint8_t *out)
{
    // Payload: channel(u8) + current(u8)
    // current: 0 = 500 µA, 1 = 1000 µA (1 mA)
    if (len < 2) { sendError(seq, cmdId, BBP_ERR_INVALID_PARAM); return -1; }
    size_t rpos = 0;
    uint8_t ch      = get_u8(payload, &rpos);
    uint8_t current = get_u8(payload, &rpos);
    if (ch >= 4)          { sendError(seq, cmdId, BBP_ERR_INVALID_CH);    return -1; }
    if (current > 1)      { sendError(seq, cmdId, BBP_ERR_INVALID_PARAM); return -1; }

    Command cmd = {};
    cmd.type          = CMD_SET_RTD_CONFIG;
    cmd.channel       = ch;
    cmd.rtdCfg.current = current;
    sendCommand(cmd);

    size_t pos = 0;
    put_u8(out, &pos, ch);
    put_u8(out, &pos, current);
    return (int)pos;
}

// --- Commands that enqueue into the command queue ---

static int handleSetChFunc(uint16_t seq, uint8_t cmdId,
                            const uint8_t *payload, size_t len, uint8_t *out)
{
    if (len < 2) { sendError(seq, cmdId, BBP_ERR_INVALID_PARAM); return -1; }
    size_t rpos = 0;
    uint8_t ch = get_u8(payload, &rpos);
    uint8_t func = get_u8(payload, &rpos);
    if (ch >= 4) { sendError(seq, cmdId, BBP_ERR_INVALID_CH); return -1; }

    Command cmd = {};
    cmd.type = CMD_SET_CHANNEL_FUNC;
    cmd.channel = ch;
    cmd.func = (ChannelFunction)func;
    sendCommand(cmd);

    size_t pos = 0;
    put_u8(out, &pos, ch);
    put_u8(out, &pos, func);
    return (int)pos;
}

static int handleSetDacCode(uint16_t seq, uint8_t cmdId,
                             const uint8_t *payload, size_t len, uint8_t *out)
{
    if (len < 3) { sendError(seq, cmdId, BBP_ERR_INVALID_PARAM); return -1; }
    size_t rpos = 0;
    uint8_t ch = get_u8(payload, &rpos);
    uint16_t code = get_u16(payload, &rpos);
    if (ch >= 4) { sendError(seq, cmdId, BBP_ERR_INVALID_CH); return -1; }

    Command cmd = {};
    cmd.type = CMD_SET_DAC_CODE;
    cmd.channel = ch;
    cmd.dacCode = code;
    sendCommand(cmd);

    size_t pos = 0;
    put_u8(out, &pos, ch);
    put_u16(out, &pos, code);
    return (int)pos;
}

static int handleSetDacVoltage(uint16_t seq, uint8_t cmdId,
                                const uint8_t *payload, size_t len, uint8_t *out)
{
    if (len < 6) { sendError(seq, cmdId, BBP_ERR_INVALID_PARAM); return -1; }
    size_t rpos = 0;
    uint8_t ch = get_u8(payload, &rpos);
    float voltage = get_f32(payload, &rpos);
    bool bipolar = get_bool(payload, &rpos);
    if (ch >= 4) { sendError(seq, cmdId, BBP_ERR_INVALID_CH); return -1; }

    Command cmd = {};
    cmd.type = CMD_SET_DAC_VOLTAGE;
    cmd.channel = ch;
    cmd.dacVoltage.voltage = voltage;
    cmd.dacVoltage.bipolar = bipolar;
    sendCommand(cmd);

    size_t pos = 0;
    put_u8(out, &pos, ch);
    put_f32(out, &pos, voltage);
    put_bool(out, &pos, bipolar);
    return (int)pos;
}

static int handleSetDacCurrent(uint16_t seq, uint8_t cmdId,
                                const uint8_t *payload, size_t len, uint8_t *out)
{
    if (len < 5) { sendError(seq, cmdId, BBP_ERR_INVALID_PARAM); return -1; }
    size_t rpos = 0;
    uint8_t ch = get_u8(payload, &rpos);
    float current_mA = get_f32(payload, &rpos);
    if (ch >= 4) { sendError(seq, cmdId, BBP_ERR_INVALID_CH); return -1; }

    Command cmd = {};
    cmd.type = CMD_SET_DAC_CURRENT;
    cmd.channel = ch;
    cmd.floatVal = current_mA;
    sendCommand(cmd);

    size_t pos = 0;
    put_u8(out, &pos, ch);
    put_f32(out, &pos, current_mA);
    return (int)pos;
}

static int handleSetAdcConfig(uint16_t seq, uint8_t cmdId,
                               const uint8_t *payload, size_t len, uint8_t *out)
{
    if (len < 4) { sendError(seq, cmdId, BBP_ERR_INVALID_PARAM); return -1; }
    size_t rpos = 0;
    uint8_t ch = get_u8(payload, &rpos);
    uint8_t mux = get_u8(payload, &rpos);
    uint8_t range = get_u8(payload, &rpos);
    uint8_t rate = get_u8(payload, &rpos);
    if (ch >= 4) { sendError(seq, cmdId, BBP_ERR_INVALID_CH); return -1; }

    Command cmd = {};
    cmd.type = CMD_ADC_CONFIG;
    cmd.channel = ch;
    cmd.adcCfg.mux = (AdcConvMux)mux;
    cmd.adcCfg.range = (AdcRange)range;
    cmd.adcCfg.rate = (AdcRate)rate;
    sendCommand(cmd);

    size_t pos = 0;
    put_u8(out, &pos, ch);
    put_u8(out, &pos, mux);
    put_u8(out, &pos, range);
    put_u8(out, &pos, rate);
    return (int)pos;
}

static int handleSetDinConfig(uint16_t seq, uint8_t cmdId,
                               const uint8_t *payload, size_t len, uint8_t *out)
{
    if (len < 8) { sendError(seq, cmdId, BBP_ERR_INVALID_PARAM); return -1; }
    size_t rpos = 0;
    uint8_t ch = get_u8(payload, &rpos);
    uint8_t thresh = get_u8(payload, &rpos);
    bool threshMode = get_bool(payload, &rpos);
    uint8_t debounce = get_u8(payload, &rpos);
    uint8_t sink = get_u8(payload, &rpos);
    bool sinkRange = get_bool(payload, &rpos);
    bool ocDet = get_bool(payload, &rpos);
    bool scDet = get_bool(payload, &rpos);
    if (ch >= 4) { sendError(seq, cmdId, BBP_ERR_INVALID_CH); return -1; }

    Command cmd = {};
    cmd.type = CMD_DIN_CONFIG;
    cmd.channel = ch;
    cmd.dinCfg.thresh = thresh;
    cmd.dinCfg.threshMode = threshMode;
    cmd.dinCfg.debounce = debounce;
    cmd.dinCfg.sink = sink;
    cmd.dinCfg.sinkRange = sinkRange;
    cmd.dinCfg.ocDet = ocDet;
    cmd.dinCfg.scDet = scDet;
    sendCommand(cmd);

    // Echo full request as response
    memcpy(out, payload, 8);
    return 8;
}

static int handleSetDoConfig(uint16_t seq, uint8_t cmdId,
                              const uint8_t *payload, size_t len, uint8_t *out)
{
    if (len < 5) { sendError(seq, cmdId, BBP_ERR_INVALID_PARAM); return -1; }
    size_t rpos = 0;
    uint8_t ch = get_u8(payload, &rpos);
    uint8_t mode = get_u8(payload, &rpos);
    bool srcSelGpio = get_bool(payload, &rpos);
    uint8_t t1 = get_u8(payload, &rpos);
    uint8_t t2 = get_u8(payload, &rpos);
    if (ch >= 4) { sendError(seq, cmdId, BBP_ERR_INVALID_CH); return -1; }

    Command cmd = {};
    cmd.type = CMD_DO_CONFIG;
    cmd.channel = ch;
    cmd.doCfg.mode = mode;
    cmd.doCfg.srcSelGpio = srcSelGpio;
    cmd.doCfg.t1 = t1;
    cmd.doCfg.t2 = t2;
    sendCommand(cmd);

    memcpy(out, payload, 5);
    return 5;
}

static int handleSetDoState(uint16_t seq, uint8_t cmdId,
                             const uint8_t *payload, size_t len, uint8_t *out)
{
    if (len < 2) { sendError(seq, cmdId, BBP_ERR_INVALID_PARAM); return -1; }
    size_t rpos = 0;
    uint8_t ch = get_u8(payload, &rpos);
    bool on = get_bool(payload, &rpos);
    if (ch >= 4) { sendError(seq, cmdId, BBP_ERR_INVALID_CH); return -1; }

    Command cmd = {};
    cmd.type = CMD_DO_SET;
    cmd.channel = ch;
    cmd.boolVal = on;
    sendCommand(cmd);

    memcpy(out, payload, 2);
    return 2;
}

static int handleSetVoutRange(uint16_t seq, uint8_t cmdId,
                               const uint8_t *payload, size_t len, uint8_t *out)
{
    if (len < 2) { sendError(seq, cmdId, BBP_ERR_INVALID_PARAM); return -1; }
    size_t rpos = 0;
    uint8_t ch = get_u8(payload, &rpos);
    bool bipolar = get_bool(payload, &rpos);
    if (ch >= 4) { sendError(seq, cmdId, BBP_ERR_INVALID_CH); return -1; }

    Command cmd = {};
    cmd.type = CMD_SET_VOUT_RANGE;
    cmd.channel = ch;
    cmd.boolVal = bipolar;
    sendCommand(cmd);

    memcpy(out, payload, 2);
    return 2;
}

static int handleSetIlimit(uint16_t seq, uint8_t cmdId,
                            const uint8_t *payload, size_t len, uint8_t *out)
{
    if (len < 2) { sendError(seq, cmdId, BBP_ERR_INVALID_PARAM); return -1; }
    size_t rpos = 0;
    uint8_t ch = get_u8(payload, &rpos);
    bool limit = get_bool(payload, &rpos);
    if (ch >= 4) { sendError(seq, cmdId, BBP_ERR_INVALID_CH); return -1; }

    Command cmd = {};
    cmd.type = CMD_SET_CURRENT_LIMIT;
    cmd.channel = ch;
    cmd.boolVal = limit;
    sendCommand(cmd);

    memcpy(out, payload, 2);
    return 2;
}

static int handleSetAvddSel(uint16_t seq, uint8_t cmdId,
                              const uint8_t *payload, size_t len, uint8_t *out)
{
    if (len < 2) { sendError(seq, cmdId, BBP_ERR_INVALID_PARAM); return -1; }
    size_t rpos = 0;
    uint8_t ch = get_u8(payload, &rpos);
    uint8_t sel = get_u8(payload, &rpos);
    if (ch >= 4) { sendError(seq, cmdId, BBP_ERR_INVALID_CH); return -1; }

    Command cmd = {};
    cmd.type = CMD_SET_AVDD_SELECT;
    cmd.channel = ch;
    cmd.avddSel = sel;
    sendCommand(cmd);

    memcpy(out, payload, 2);
    return 2;
}

static int handleClearAllAlerts(uint16_t seq, uint8_t cmdId, uint8_t *out)
{
    Command cmd = {};
    cmd.type = CMD_CLEAR_ALERTS;
    sendCommand(cmd);
    return 0;
}

static int handleClearChAlert(uint16_t seq, uint8_t cmdId,
                               const uint8_t *payload, size_t len, uint8_t *out)
{
    if (len < 1) { sendError(seq, cmdId, BBP_ERR_INVALID_PARAM); return -1; }
    size_t rpos = 0;
    uint8_t ch = get_u8(payload, &rpos);
    if (ch >= 4) { sendError(seq, cmdId, BBP_ERR_INVALID_CH); return -1; }

    Command cmd = {};
    cmd.type = CMD_CLEAR_CHANNEL_ALERT;
    cmd.channel = ch;
    sendCommand(cmd);

    out[0] = ch;
    return 1;
}

static int handleSetAlertMask(uint16_t seq, uint8_t cmdId,
                               const uint8_t *payload, size_t len, uint8_t *out)
{
    if (len < 4) { sendError(seq, cmdId, BBP_ERR_INVALID_PARAM); return -1; }
    size_t rpos = 0;
    uint16_t alertMask = get_u16(payload, &rpos);
    uint16_t supplyMask = get_u16(payload, &rpos);

    Command cmd = {};
    cmd.type = CMD_SET_ALERT_MASK;
    cmd.maskVal = alertMask;
    sendCommand(cmd);

    Command cmd2 = {};
    cmd2.type = CMD_SET_SUPPLY_ALERT_MASK;
    cmd2.maskVal = supplyMask;
    sendCommand(cmd2);

    memcpy(out, payload, 4);
    return 4;
}

static int handleSetChAlertMask(uint16_t seq, uint8_t cmdId,
                                 const uint8_t *payload, size_t len, uint8_t *out)
{
    if (len < 3) { sendError(seq, cmdId, BBP_ERR_INVALID_PARAM); return -1; }
    size_t rpos = 0;
    uint8_t ch = get_u8(payload, &rpos);
    uint16_t mask = get_u16(payload, &rpos);
    if (ch >= 4) { sendError(seq, cmdId, BBP_ERR_INVALID_CH); return -1; }

    Command cmd = {};
    cmd.type = CMD_SET_CH_ALERT_MASK;
    cmd.channel = ch;
    cmd.maskVal = mask;
    sendCommand(cmd);

    memcpy(out, payload, 3);
    return 3;
}

static int handleSetDiagConfig(uint16_t seq, uint8_t cmdId,
                                const uint8_t *payload, size_t len, uint8_t *out)
{
    if (len < 2) { sendError(seq, cmdId, BBP_ERR_INVALID_PARAM); return -1; }
    size_t rpos = 0;
    uint8_t slot = get_u8(payload, &rpos);
    uint8_t source = get_u8(payload, &rpos);
    if (slot >= 4 || source > 13) { sendError(seq, cmdId, BBP_ERR_INVALID_PARAM); return -1; }

    Command cmd = {};
    cmd.type = CMD_DIAG_CONFIG;
    cmd.diagCfg.slot = slot;
    cmd.diagCfg.source = source;
    sendCommand(cmd);

    memcpy(out, payload, 2);
    return 2;
}

static int handleSetGpioConfig(uint16_t seq, uint8_t cmdId,
                                const uint8_t *payload, size_t len, uint8_t *out)
{
    if (len < 3) { sendError(seq, cmdId, BBP_ERR_INVALID_PARAM); return -1; }
    size_t rpos = 0;
    uint8_t gpio = get_u8(payload, &rpos);
    uint8_t mode = get_u8(payload, &rpos);
    bool pulldown = get_bool(payload, &rpos);
    if (gpio >= 6) { sendError(seq, cmdId, BBP_ERR_INVALID_PARAM); return -1; }

    Command cmd = {};
    cmd.type = CMD_GPIO_CONFIG;
    cmd.gpioCfg.gpio = gpio;
    cmd.gpioCfg.mode = mode;
    cmd.gpioCfg.pulldown = pulldown;
    sendCommand(cmd);

    memcpy(out, payload, 3);
    return 3;
}

static int handleSetGpioValue(uint16_t seq, uint8_t cmdId,
                               const uint8_t *payload, size_t len, uint8_t *out)
{
    if (len < 2) { sendError(seq, cmdId, BBP_ERR_INVALID_PARAM); return -1; }
    size_t rpos = 0;
    uint8_t gpio = get_u8(payload, &rpos);
    bool value = get_bool(payload, &rpos);
    if (gpio >= 6) { sendError(seq, cmdId, BBP_ERR_INVALID_PARAM); return -1; }

    Command cmd = {};
    cmd.type = CMD_GPIO_SET;
    cmd.gpioSet.gpio = gpio;
    cmd.gpioSet.value = value;
    sendCommand(cmd);

    memcpy(out, payload, 2);
    return 2;
}

// -----------------------------------------------------------------------------
// Digital IO (ESP32 GPIO) handlers
// -----------------------------------------------------------------------------

// 0x43 DIO_GET_ALL — read all 12 IO states
static int handleDioGetAll(uint16_t seq, uint8_t cmdId, uint8_t *out)
{
    const DioState *all = dio_get_all();
    dio_poll_inputs();  // refresh input levels before responding

    size_t pos = 0;
    put_u8(out, &pos, DIO_NUM_IOS);
    for (int i = 0; i < DIO_NUM_IOS; i++) {
        put_u8(out, &pos, (uint8_t)(i + 1));          // io number (1-12)
        put_u8(out, &pos, all[i].gpio_num);            // ESP32 GPIO pin
        put_u8(out, &pos, all[i].mode);                // 0=disabled, 1=input, 2=output
        put_bool(out, &pos, all[i].output_level);      // last written output
        put_bool(out, &pos, all[i].input_level);       // last read input
    }
    return (int)pos;
}

// 0x44 DIO_CONFIG — configure IO direction
static int handleDioConfig(uint16_t seq, uint8_t cmdId,
                           const uint8_t *payload, size_t len, uint8_t *out)
{
    if (len < 2) { sendError(seq, cmdId, BBP_ERR_INVALID_PARAM); return -1; }
    size_t rpos = 0;
    uint8_t io   = get_u8(payload, &rpos);   // IO number 1-12
    uint8_t mode = get_u8(payload, &rpos);   // 0=disabled, 1=input, 2=output

    if (!dio_configure(io, mode)) {
        sendError(seq, cmdId, BBP_ERR_INVALID_PARAM);
        return -1;
    }

    size_t pos = 0;
    put_u8(out, &pos, io);
    put_u8(out, &pos, mode);
    return (int)pos;
}

// 0x45 DIO_WRITE — set output level
static int handleDioWrite(uint16_t seq, uint8_t cmdId,
                          const uint8_t *payload, size_t len, uint8_t *out)
{
    if (len < 2) { sendError(seq, cmdId, BBP_ERR_INVALID_PARAM); return -1; }
    size_t rpos = 0;
    uint8_t io  = get_u8(payload, &rpos);    // IO number 1-12
    bool level  = get_bool(payload, &rpos);  // true=HIGH, false=LOW

    if (!dio_write(io, level)) {
        sendError(seq, cmdId, BBP_ERR_INVALID_PARAM);
        return -1;
    }

    size_t pos = 0;
    put_u8(out, &pos, io);
    put_bool(out, &pos, level);
    return (int)pos;
}

// 0x46 DIO_READ — read single IO
static int handleDioRead(uint16_t seq, uint8_t cmdId,
                         const uint8_t *payload, size_t len, uint8_t *out)
{
    if (len < 1) { sendError(seq, cmdId, BBP_ERR_INVALID_PARAM); return -1; }
    size_t rpos = 0;
    uint8_t io = get_u8(payload, &rpos);

    DioState st;
    if (!dio_get_state(io, &st)) {
        sendError(seq, cmdId, BBP_ERR_INVALID_PARAM);
        return -1;
    }

    // Re-read the pin live
    bool level = false;
    if (st.mode == DIO_MODE_INPUT) {
        level = dio_read(io);
    } else if (st.mode == DIO_MODE_OUTPUT) {
        level = st.output_level;
    }

    size_t pos = 0;
    put_u8(out, &pos, io);
    put_u8(out, &pos, st.mode);
    put_bool(out, &pos, level);
    return (int)pos;
}

// -----------------------------------------------------------------------------
// Self-Test / Calibration handlers
// -----------------------------------------------------------------------------

// 0x05 SELFTEST_STATUS — get boot result + cal status
static int handleSelftestStatus(uint16_t seq, uint8_t cmdId, uint8_t *out)
{
    const SelftestBootResult *boot = selftest_boot_check();  // returns cached if already ran
    const SelftestCalResult  *cal  = selftest_get_cal_result();

    size_t pos = 0;
    // Boot result
    put_bool(out, &pos, boot->ran);
    put_bool(out, &pos, boot->passed);
    put_f32(out, &pos, boot->vadj1_v);
    put_f32(out, &pos, boot->vadj2_v);
    put_f32(out, &pos, boot->vlogic_v);
    // Cal result
    put_u8(out, &pos, cal->status);
    put_u8(out, &pos, cal->channel);
    put_u8(out, &pos, cal->points_collected);
    put_f32(out, &pos, cal->error_mv);
    return (int)pos;
}

// 0x06 SELFTEST_MEASURE_SUPPLY — measure a rail
static int handleSelftestMeasureSupply(uint16_t seq, uint8_t cmdId,
                                       const uint8_t *payload, size_t len, uint8_t *out)
{
    if (len < 1) { sendError(seq, cmdId, BBP_ERR_INVALID_PARAM); return -1; }
    uint8_t rail = payload[0];

    float voltage = selftest_measure_supply(rail);

    size_t pos = 0;
    put_u8(out, &pos, rail);
    put_f32(out, &pos, voltage);
    return (int)pos;
}

// 0x07 SELFTEST_EFUSE_CURRENTS — get all e-fuse currents
static int handleSelftestEfuseCurrents(uint16_t seq, uint8_t cmdId, uint8_t *out)
{
    const SelftestEfuseCurrents *ec = selftest_get_efuse_currents();

    size_t pos = 0;
    put_bool(out, &pos, ec->available);
    put_u32(out, &pos, ec->timestamp_ms);
    for (int i = 0; i < SELFTEST_EFUSE_COUNT; i++) {
        put_f32(out, &pos, ec->current_a[i]);
    }
    return (int)pos;
}

// 0x08 SELFTEST_AUTO_CAL — start auto-calibration
static int handleSelftestAutoCal(uint16_t seq, uint8_t cmdId,
                                  const uint8_t *payload, size_t len, uint8_t *out)
{
    if (len < 1) { sendError(seq, cmdId, BBP_ERR_INVALID_PARAM); return -1; }
    uint8_t idac_ch = payload[0];

    bool ok = selftest_start_auto_calibrate(idac_ch);
    if (!ok) {
        sendError(seq, cmdId, BBP_ERR_BUSY);
        return -1;
    }

    const SelftestCalResult *cal = selftest_get_cal_result();
    size_t pos = 0;
    put_u8(out, &pos, cal->status);
    put_u8(out, &pos, cal->channel);
    put_u8(out, &pos, cal->points_collected);
    put_f32(out, &pos, cal->error_mv);
    return (int)pos;
}

static int handleSetUartConfig(uint16_t seq, uint8_t cmdId,
                                const uint8_t *payload, size_t len, uint8_t *out)
{
    if (len < 12) { sendError(seq, cmdId, BBP_ERR_INVALID_PARAM); return -1; }
    size_t rpos = 0;
    uint8_t bridgeId = get_u8(payload, &rpos);
    if (bridgeId >= CDC_BRIDGE_COUNT) { sendError(seq, cmdId, BBP_ERR_INVALID_PARAM); return -1; }

    UartBridgeConfig cfg;
    cfg.uart_num  = get_u8(payload, &rpos);
    cfg.tx_pin    = (int)get_u8(payload, &rpos);
    cfg.rx_pin    = (int)get_u8(payload, &rpos);
    cfg.baudrate  = get_u32(payload, &rpos);
    cfg.data_bits = get_u8(payload, &rpos);
    cfg.parity    = get_u8(payload, &rpos);
    cfg.stop_bits = get_u8(payload, &rpos);
    cfg.enabled   = get_bool(payload, &rpos);

    uart_bridge_set_config(bridgeId, &cfg);

    memcpy(out, payload, 12);
    return 12;
}

static int handleRegRead(uint16_t seq, uint8_t cmdId,
                          const uint8_t *payload, size_t len, uint8_t *out)
{
    if (len < 1) { sendError(seq, cmdId, BBP_ERR_INVALID_PARAM); return -1; }
    uint8_t addr = payload[0];
    uint16_t value = 0;
    if (!s_spi->readRegister(addr, &value)) {
        sendError(seq, cmdId, BBP_ERR_SPI_FAIL);
        return -1;
    }
    size_t pos = 0;
    put_u8(out, &pos, addr);
    put_u16(out, &pos, value);
    return (int)pos;
}

static int handleRegWrite(uint16_t seq, uint8_t cmdId,
                           const uint8_t *payload, size_t len, uint8_t *out)
{
    if (len < 3) { sendError(seq, cmdId, BBP_ERR_INVALID_PARAM); return -1; }
    size_t rpos = 0;
    uint8_t addr = get_u8(payload, &rpos);
    uint16_t value = get_u16(payload, &rpos);
    s_spi->writeRegister(addr, value);

    memcpy(out, payload, 3);
    return 3;
}

static int handleDeviceReset(uint16_t seq, uint8_t cmdId, uint8_t *out)
{
    // Reset all channels to HIGH_IMP + clear alerts (same as web /api/device/reset)
    for (uint8_t ch = 0; ch < 4; ch++) {
        Command cmd = {};
        cmd.type = CMD_SET_CHANNEL_FUNC;
        cmd.channel = ch;
        cmd.func = CH_FUNC_HIGH_IMP;
        sendCommand(cmd);
    }
    Command clr = {};
    clr.type = CMD_CLEAR_ALERTS;
    sendCommand(clr);
    return 0;
}

// --- DS4424 IDAC commands ---

static int handleIdacGetStatus(uint16_t seq, uint8_t cmdId, uint8_t *out)
{
    const DS4424State *st = ds4424_get_state();
    size_t pos = 0;
    put_bool(out, &pos, st->present);
    for (uint8_t ch = 0; ch < DS4424_NUM_CHANNELS; ch++) {
        put_u8(out, &pos, ch);
        put_u8(out, &pos, (uint8_t)(st->state[ch].dac_code & 0xFF));
        put_f32(out, &pos, st->state[ch].target_v);
        put_f32(out, &pos, st->state[ch].actual_v);
        put_f32(out, &pos, st->config[ch].midpoint_v);
        put_f32(out, &pos, st->config[ch].v_min);
        put_f32(out, &pos, st->config[ch].v_max);
        put_f32(out, &pos, ds4424_step_mv(ch));
        put_bool(out, &pos, st->cal[ch].valid);
        // Calibration points (code, voltage pairs)
        put_u8(out, &pos, st->cal[ch].count);
        for (uint8_t p = 0; p < st->cal[ch].count; p++) {
            put_u8(out, &pos, (uint8_t)(st->cal[ch].points[p].dac_code & 0xFF));
            put_f32(out, &pos, st->cal[ch].points[p].measured_v);
        }
    }
    return (int)pos;
}

static int handleIdacSetCode(uint16_t seq, uint8_t cmdId,
                              const uint8_t *payload, size_t len, uint8_t *out)
{
    if (len < 2) { sendError(seq, cmdId, BBP_ERR_INVALID_PARAM); return -1; }
    size_t rpos = 0;
    uint8_t ch = get_u8(payload, &rpos);
    int8_t code = (int8_t)get_u8(payload, &rpos);
    if (ch >= DS4424_NUM_CHANNELS) { sendError(seq, cmdId, BBP_ERR_INVALID_CH); return -1; }

    if (!ds4424_set_code(ch, code)) {
        sendError(seq, cmdId, BBP_ERR_SPI_FAIL);
        return -1;
    }

    size_t pos = 0;
    put_u8(out, &pos, ch);
    put_u8(out, &pos, (uint8_t)(code & 0xFF));
    put_f32(out, &pos, ds4424_code_to_voltage(ch, code));
    return (int)pos;
}

static int handleIdacSetVoltage(uint16_t seq, uint8_t cmdId,
                                 const uint8_t *payload, size_t len, uint8_t *out)
{
    if (len < 5) { sendError(seq, cmdId, BBP_ERR_INVALID_PARAM); return -1; }
    size_t rpos = 0;
    uint8_t ch = get_u8(payload, &rpos);
    float voltage = get_f32(payload, &rpos);
    if (ch >= 3) { sendError(seq, cmdId, BBP_ERR_INVALID_CH); return -1; }

    if (!ds4424_set_voltage(ch, voltage)) {
        sendError(seq, cmdId, BBP_ERR_SPI_FAIL);
        return -1;
    }

    const DS4424State *st = ds4424_get_state();
    size_t pos = 0;
    put_u8(out, &pos, ch);
    put_u8(out, &pos, (uint8_t)(st->state[ch].dac_code & 0xFF));
    put_f32(out, &pos, st->state[ch].target_v);
    return (int)pos;
}

// ADC read callback for IDAC calibration.
// Reads the cached ADC value from channel 0 (the user must connect ADC Ch A
// to the measurement point and set it to VIN mode before starting calibration).
// The ADC poll task continuously updates g_deviceState.channels[0].adcValue.
static uint8_t s_cal_adc_channel = 0;  // Which AD74416H channel reads the voltage

static float cal_read_adc(uint8_t idac_ch)
{
    (void)idac_ch;  // We always read from the configured ADC channel
    float val = 0.0f;
    if (xSemaphoreTake(g_stateMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        val = g_deviceState.channels[s_cal_adc_channel].adcValue;
        xSemaphoreGive(g_stateMutex);
    }
    return val;
}

static int handleIdacCalibrate(uint16_t seq, uint8_t cmdId,
                                const uint8_t *payload, size_t len, uint8_t *out)
{
    // Payload: idac_ch(u8) + step(u8) + settle_ms(u16) + adc_channel(u8)
    if (len < 5) { sendError(seq, cmdId, BBP_ERR_INVALID_PARAM); return -1; }
    size_t rpos = 0;
    uint8_t ch = get_u8(payload, &rpos);
    uint8_t step = get_u8(payload, &rpos);
    uint16_t settle = get_u16(payload, &rpos);
    uint8_t adc_ch = get_u8(payload, &rpos);
    if (ch >= 3) { sendError(seq, cmdId, BBP_ERR_INVALID_CH); return -1; }
    if (adc_ch >= 4) { sendError(seq, cmdId, BBP_ERR_INVALID_CH); return -1; }

    // Check that the ADC channel is in VIN mode
    ChannelFunction func = CH_FUNC_HIGH_IMP;
    if (xSemaphoreTake(g_stateMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        func = g_deviceState.channels[adc_ch].function;
        xSemaphoreGive(g_stateMutex);
    }
    if (func != CH_FUNC_VIN) {
        ESP_LOGW(TAG, "IDAC calibrate: ADC channel %d not in VIN mode (func=%d)", adc_ch, func);
        sendError(seq, cmdId, BBP_ERR_INVALID_STATE);
        return -1;
    }

    s_cal_adc_channel = adc_ch;

    ESP_LOGI(TAG, "Starting IDAC%d auto-calibration (step=%d, settle=%dms, ADC=ch%d)",
             ch, step, settle, adc_ch);

    int points = ds4424_cal_auto(ch, cal_read_adc, step, settle);

    // Save to NVS
    ds4424_cal_save();

    ESP_LOGI(TAG, "IDAC%d calibration complete: %d points, saved to NVS", ch, points);

    size_t pos = 0;
    put_u8(out, &pos, ch);
    put_u8(out, &pos, (uint8_t)points);
    return (int)pos;
}

static int handleIdacCalAddPoint(uint16_t seq, uint8_t cmdId,
                                  const uint8_t *payload, size_t len, uint8_t *out)
{
    if (len < 6) { sendError(seq, cmdId, BBP_ERR_INVALID_PARAM); return -1; }
    size_t rpos = 0;
    uint8_t ch = get_u8(payload, &rpos);
    int8_t code = (int8_t)get_u8(payload, &rpos);
    float measured_v = get_f32(payload, &rpos);
    if (ch >= 3) { sendError(seq, cmdId, BBP_ERR_INVALID_CH); return -1; }

    ds4424_cal_add_point(ch, code, measured_v);

    const DS4424State *st = ds4424_get_state();
    size_t pos = 0;
    put_u8(out, &pos, ch);
    put_u8(out, &pos, st->cal[ch].count);
    put_bool(out, &pos, st->cal[ch].valid);
    return (int)pos;
}

static int handleIdacCalClear(uint16_t seq, uint8_t cmdId,
                               const uint8_t *payload, size_t len, uint8_t *out)
{
    if (len < 1) { sendError(seq, cmdId, BBP_ERR_INVALID_PARAM); return -1; }
    uint8_t ch = payload[0];
    if (ch >= 3) { sendError(seq, cmdId, BBP_ERR_INVALID_CH); return -1; }

    ds4424_cal_clear(ch);

    size_t pos = 0;
    put_u8(out, &pos, ch);
    return (int)pos;
}

static int handleIdacCalSave(uint16_t seq, uint8_t cmdId, uint8_t *out)
{
    bool ok = ds4424_cal_save();
    size_t pos = 0;
    put_bool(out, &pos, ok);
    return (int)pos;
}

// --- PCA9535 GPIO Expander commands ---

static int handlePcaGetStatus(uint16_t seq, uint8_t cmdId, uint8_t *out)
{
    pca9535_update();  // Read latest inputs
    const PCA9535State *st = pca9535_get_state();
    size_t pos = 0;
    put_bool(out, &pos, st->present);
    put_u8(out, &pos, st->input0);
    put_u8(out, &pos, st->input1);
    put_u8(out, &pos, st->output0);
    put_u8(out, &pos, st->output1);
    // Decoded status
    put_bool(out, &pos, st->logic_pg);
    put_bool(out, &pos, st->vadj1_pg);
    put_bool(out, &pos, st->vadj2_pg);
    for (int i = 0; i < 4; i++) put_bool(out, &pos, st->efuse_flt[i]);
    // Decoded enables
    put_bool(out, &pos, st->vadj1_en);
    put_bool(out, &pos, st->vadj2_en);
    put_bool(out, &pos, st->en_15v);
    put_bool(out, &pos, st->en_mux);
    put_bool(out, &pos, st->en_usb_hub);
    for (int i = 0; i < 4; i++) put_bool(out, &pos, st->efuse_en[i]);
    return (int)pos;
}

static int handlePcaSetControl(uint16_t seq, uint8_t cmdId,
                                const uint8_t *payload, size_t len, uint8_t *out)
{
    if (len < 2) { sendError(seq, cmdId, BBP_ERR_INVALID_PARAM); return -1; }
    size_t rpos = 0;
    uint8_t ctrl = get_u8(payload, &rpos);
    bool on = get_bool(payload, &rpos);
    if (ctrl >= PCA_CTRL_COUNT) { sendError(seq, cmdId, BBP_ERR_INVALID_PARAM); return -1; }

    if (!pca9535_set_control((PcaControl)ctrl, on)) {
        sendError(seq, cmdId, BBP_ERR_SPI_FAIL);
        return -1;
    }

    size_t pos = 0;
    put_u8(out, &pos, ctrl);
    put_bool(out, &pos, on);
    return (int)pos;
}

static int handlePcaSetPort(uint16_t seq, uint8_t cmdId,
                             const uint8_t *payload, size_t len, uint8_t *out)
{
    if (len < 2) { sendError(seq, cmdId, BBP_ERR_INVALID_PARAM); return -1; }
    size_t rpos = 0;
    uint8_t port = get_u8(payload, &rpos);
    uint8_t val = get_u8(payload, &rpos);
    if (port > 1) { sendError(seq, cmdId, BBP_ERR_INVALID_PARAM); return -1; }

    if (!pca9535_set_port(port, val)) {
        sendError(seq, cmdId, BBP_ERR_SPI_FAIL);
        return -1;
    }

    size_t pos = 0;
    put_u8(out, &pos, port);
    put_u8(out, &pos, val);
    return (int)pos;
}

// --- HUSB238 USB PD commands ---

static int handleUsbpdGetStatus(uint16_t seq, uint8_t cmdId, uint8_t *out)
{
    husb238_update();
    const Husb238State *st = husb238_get_state();
    size_t pos = 0;
    put_bool(out, &pos, st->present);
    put_bool(out, &pos, st->attached);
    put_bool(out, &pos, st->cc_direction);
    put_u8(out, &pos, st->pd_response);
    put_u8(out, &pos, (uint8_t)st->voltage);
    put_u8(out, &pos, (uint8_t)st->current);
    put_f32(out, &pos, st->voltage_v);
    put_f32(out, &pos, st->current_a);
    put_f32(out, &pos, st->power_w);
    // Source PDOs
    struct { bool det; Husb238Current cur; } pdos[] = {
        {st->pdo_5v.detected, st->pdo_5v.max_current},
        {st->pdo_9v.detected, st->pdo_9v.max_current},
        {st->pdo_12v.detected, st->pdo_12v.max_current},
        {st->pdo_15v.detected, st->pdo_15v.max_current},
        {st->pdo_18v.detected, st->pdo_18v.max_current},
        {st->pdo_20v.detected, st->pdo_20v.max_current},
    };
    for (int i = 0; i < 6; i++) {
        put_bool(out, &pos, pdos[i].det);
        put_u8(out, &pos, (uint8_t)pdos[i].cur);
    }
    put_u8(out, &pos, st->selected_pdo);
    return (int)pos;
}

static int handleUsbpdSelectPdo(uint16_t seq, uint8_t cmdId,
                                 const uint8_t *payload, size_t len, uint8_t *out)
{
    if (len < 1) { sendError(seq, cmdId, BBP_ERR_INVALID_PARAM); return -1; }
    size_t rpos = 0;
    uint8_t voltage = get_u8(payload, &rpos);

    if (!husb238_select_pdo((Husb238Voltage)voltage)) {
        sendError(seq, cmdId, BBP_ERR_SPI_FAIL);
        return -1;
    }

    size_t pos = 0;
    put_u8(out, &pos, voltage);
    return (int)pos;
}

static int handleUsbpdGo(uint16_t seq, uint8_t cmdId,
                          const uint8_t *payload, size_t len, uint8_t *out)
{
    if (len < 1) { sendError(seq, cmdId, BBP_ERR_INVALID_PARAM); return -1; }
    uint8_t cmd = payload[0];
    if (!husb238_go_command(cmd)) {
        sendError(seq, cmdId, BBP_ERR_SPI_FAIL);
        return -1;
    }
    out[0] = cmd;
    return 1;
}

// --- MUX commands ---

static int handleMuxSetAll(uint16_t seq, uint8_t cmdId,
                            const uint8_t *payload, size_t len, uint8_t *out)
{
    if (len < ADGS_NUM_DEVICES) { sendError(seq, cmdId, BBP_ERR_INVALID_PARAM); return -1; }
    adgs_set_all_safe(payload);  // Includes 100ms dead time
    // Update cached state and echo back
    adgs_get_all_states(out);
    if (xSemaphoreTake(g_stateMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        adgs_get_all_states(g_deviceState.muxState);
        xSemaphoreGive(g_stateMutex);
    }
    return ADGS_NUM_DEVICES;
}

static int handleMuxGetAll(uint16_t seq, uint8_t cmdId, uint8_t *out)
{
    adgs_get_all_states(out);
    return ADGS_NUM_DEVICES;
}

static int handleMuxSetSwitch(uint16_t seq, uint8_t cmdId,
                               const uint8_t *payload, size_t len, uint8_t *out)
{
    if (len < 3) { sendError(seq, cmdId, BBP_ERR_INVALID_PARAM); return -1; }
    uint8_t device = payload[0];
    uint8_t sw = payload[1];
    bool closed = payload[2] != 0;
    if (device >= ADGS_NUM_DEVICES || sw >= ADGS_NUM_SWITCHES) {
        sendError(seq, cmdId, BBP_ERR_INVALID_PARAM);
        return -1;
    }
    {
        // Group masks: S1-S4 share output A, S5-S6 share B, S7-S8 share C
        uint8_t group_mask;
        if (sw < 4)      group_mask = 0x0F;  // Group A
        else if (sw < 6) group_mask = 0x30;  // Group B
        else             group_mask = 0xC0;  // Group C

        uint8_t states[ADGS_NUM_DEVICES];
        adgs_get_all_states(states);

        if (closed) {
            // First open all switches in the same group (break-before-make)
            uint8_t prev = states[device];
            states[device] &= ~group_mask;
            if (states[device] != prev) {
                adgs_set_all_raw(states);
                delay_ms(ADGS_DEAD_TIME_MS);
            }
            // Then close only the requested switch
            states[device] |= (1 << sw);
        } else {
            states[device] &= ~(1 << sw);
        }
        adgs_set_all_raw(states);
    }
    // Update cached state
    if (xSemaphoreTake(g_stateMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        adgs_get_all_states(g_deviceState.muxState);
        xSemaphoreGive(g_stateMutex);
    }
    out[0] = device;
    out[1] = sw;
    out[2] = closed ? 1 : 0;
    return 3;
}

static int handleSetLshiftOe(uint16_t seq, uint8_t cmdId,
                              const uint8_t *payload, size_t len, uint8_t *out)
{
    if (len < 1) { sendError(seq, cmdId, BBP_ERR_INVALID_PARAM); return -1; }
    bool on = payload[0] != 0;
    pin_write(PIN_LSHIFT_OE, on ? 1 : 0);
    ESP_LOGI(TAG, "Level shifter OE = %s", on ? "ON" : "OFF");
    out[0] = on ? 1 : 0;
    return 1;
}

static int handleSetSpiClock(uint16_t seq, uint8_t cmdId,
                              const uint8_t *payload, size_t len, uint8_t *out)
{
    if (len < 4) { sendError(seq, cmdId, BBP_ERR_INVALID_PARAM); return -1; }
    size_t rpos = 0;
    uint32_t hz = get_u32(payload, &rpos);

    // Pause ADC task during SPI device reconfiguration
    extern volatile bool g_spi_bus_request;
    extern volatile bool g_spi_bus_granted;
    g_spi_bus_granted = false;
    g_spi_bus_request = true;
    for (int i = 0; i < 200 && !g_spi_bus_granted; i++) delay_ms(1);

    bool ok = s_spi->setClockSpeed(hz);

    g_spi_bus_request = false;
    g_spi_bus_granted = false;

    if (!ok) { sendError(seq, cmdId, BBP_ERR_SPI_FAIL); return -1; }

    // Verify SPI still works
    uint16_t scratch = 0;
    s_spi->writeRegister(0x76, 0xA5C3);  // SCRATCH register
    bool crc_ok = s_spi->readRegister(0x76, &scratch);
    bool match = (scratch == 0xA5C3);
    s_spi->writeRegister(0x76, 0x0000);

    ESP_LOGI(TAG, "SPI clock set to %lu Hz — verify: %s", (unsigned long)hz, match ? "OK" : "FAIL");

    size_t pos = 0;
    put_u32(out, &pos, hz);
    put_bool(out, &pos, match && crc_ok);
    return (int)pos;
}

static int handlePing(uint16_t seq, uint8_t cmdId,
                       const uint8_t *payload, size_t len, uint8_t *out)
{
    size_t pos = 0;
    uint32_t token = 0;
    if (len >= 4) {
        size_t rpos = 0;
        token = get_u32(payload, &rpos);
    }
    put_u32(out, &pos, token);
    put_u32(out, &pos, millis_now());
    return (int)pos;
}

// --- WiFi Management ---

static int handleWifiGetStatus(uint16_t seq, uint8_t cmdId,
                                const uint8_t *payload, size_t len, uint8_t *out)
{
    size_t pos = 0;

    // connected (bool)
    bool connected = wifi_is_connected();
    put_bool(out, &pos, connected);

    // sta_ssid: len(u8) + bytes
    const char *sta_ssid = wifi_get_sta_ssid();
    uint8_t ssid_len = (uint8_t)strnlen(sta_ssid, 32);
    put_u8(out, &pos, ssid_len);
    memcpy(out + pos, sta_ssid, ssid_len);
    pos += ssid_len;

    // sta_ip: len(u8) + bytes
    const char *sta_ip = wifi_get_sta_ip();
    uint8_t ip_len = (uint8_t)strnlen(sta_ip, 16);
    put_u8(out, &pos, ip_len);
    memcpy(out + pos, sta_ip, ip_len);
    pos += ip_len;

    // rssi (i32 as u32)
    int32_t rssi = (int32_t)wifi_get_rssi();
    put_u32(out, &pos, (uint32_t)rssi);

    // ap_ssid: get from ESP-IDF config
    wifi_config_t ap_cfg = {};
    esp_wifi_get_config(WIFI_IF_AP, &ap_cfg);
    const char *ap_ssid = (const char *)ap_cfg.ap.ssid;
    uint8_t ap_ssid_len = (uint8_t)strnlen(ap_ssid, 32);
    put_u8(out, &pos, ap_ssid_len);
    memcpy(out + pos, ap_ssid, ap_ssid_len);
    pos += ap_ssid_len;

    // ap_ip: len(u8) + bytes
    const char *ap_ip = wifi_get_ap_ip();
    uint8_t ap_ip_len = (uint8_t)strnlen(ap_ip, 16);
    put_u8(out, &pos, ap_ip_len);
    memcpy(out + pos, ap_ip, ap_ip_len);
    pos += ap_ip_len;

    // ap_mac: len(u8) + bytes
    const char *ap_mac = wifi_get_ap_mac();
    uint8_t ap_mac_len = (uint8_t)strnlen(ap_mac, 18);
    put_u8(out, &pos, ap_mac_len);
    memcpy(out + pos, ap_mac, ap_mac_len);
    pos += ap_mac_len;

    return (int)pos;
}

static int handleWifiConnect(uint16_t seq, uint8_t cmdId,
                              const uint8_t *payload, size_t len, uint8_t *out)
{
    if (len < 2) { sendError(seq, cmdId, BBP_ERR_INVALID_PARAM); return -1; }

    size_t rpos = 0;
    uint8_t ssid_len = get_u8(payload, &rpos);
    if (ssid_len == 0 || ssid_len > 32 || rpos + ssid_len >= len) {
        sendError(seq, cmdId, BBP_ERR_INVALID_PARAM); return -1;
    }
    char ssid[33] = {};
    memcpy(ssid, payload + rpos, ssid_len);
    ssid[ssid_len] = '\0';
    rpos += ssid_len;

    uint8_t pass_len = get_u8(payload, &rpos);
    if (pass_len > 64 || rpos + pass_len > len) {
        sendError(seq, cmdId, BBP_ERR_INVALID_PARAM); return -1;
    }
    char pass[65] = {};
    memcpy(pass, payload + rpos, pass_len);
    pass[pass_len] = '\0';

    ESP_LOGI(TAG, "WiFi connect to '%s'", ssid);
    bool ok = wifi_connect(ssid, pass);

    size_t pos = 0;
    put_bool(out, &pos, ok);
    return (int)pos;
}

static int handleWifiScan(uint16_t seq, uint8_t cmdId,
                           const uint8_t *payload, size_t len, uint8_t *out)
{
    // Scan takes ~3s. Response: count(u8) + N * (ssid_len(u8) + ssid + rssi(i8) + auth(u8))
    wifi_scan_result_t results[20];
    int count = wifi_scan(results, 20);

    size_t pos = 0;
    put_u8(out, &pos, (uint8_t)count);
    for (int i = 0; i < count; i++) {
        uint8_t slen = (uint8_t)strlen(results[i].ssid);
        // Bounds check BEFORE writing: need 1 (slen) + slen (ssid) + 1 (rssi) + 1 (auth)
        if (pos + 1 + slen + 2 > BBP_MAX_PAYLOAD - 4) break;
        put_u8(out, &pos, slen);
        memcpy(out + pos, results[i].ssid, slen);
        pos += slen;
        out[pos++] = (uint8_t)(int8_t)results[i].rssi;
        put_u8(out, &pos, (uint8_t)results[i].auth);
    }
    return (int)pos;
}

// --- Streaming commands ---

static int handleStartAdcStream(uint16_t seq, uint8_t cmdId,
                                 const uint8_t *payload, size_t len, uint8_t *out)
{
    if (s_adcStreamMask != 0) {
        sendError(seq, cmdId, BBP_ERR_STREAM_ACTIVE);
        return -1;
    }
    if (len < 2) { sendError(seq, cmdId, BBP_ERR_INVALID_PARAM); return -1; }

    size_t rpos = 0;
    uint8_t mask = get_u8(payload, &rpos);
    uint8_t div  = get_u8(payload, &rpos);
    if (mask == 0 || mask > 0x0F) { sendError(seq, cmdId, BBP_ERR_INVALID_PARAM); return -1; }
    if (div == 0) div = 1;

    // Reset ring buffer
    s_adcBuf.head = 0;
    s_adcBuf.tail = 0;
    s_adcDivCount = 0;

    s_adcStreamMask = mask;
    s_adcStreamDiv  = div;

    // Estimate effective sample rate from fastest active channel
    uint16_t effectiveRate = 20; // default
    if (xSemaphoreTake(g_stateMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        for (uint8_t ch = 0; ch < 4; ch++) {
            if (mask & (1 << ch)) {
                AdcRate r = g_deviceState.channels[ch].adcRate;
                uint16_t sps = 20;
                switch (r) {
                    case ADC_RATE_10SPS_H:   sps = 10; break;
                    case ADC_RATE_20SPS:
                    case ADC_RATE_20SPS_H:   sps = 20; break;
                    case ADC_RATE_200SPS_H1:
                    case ADC_RATE_200SPS_H:  sps = 200; break;
                    case ADC_RATE_1_2KSPS:
                    case ADC_RATE_1_2KSPS_H: sps = 1200; break;
                    case ADC_RATE_4_8KSPS:   sps = 4800; break;
                    case ADC_RATE_9_6KSPS:   sps = 9600; break;
                    default: sps = 20; break;
                }
                if (sps > effectiveRate) effectiveRate = sps;
            }
        }
        xSemaphoreGive(g_stateMutex);
    }
    effectiveRate /= div;

    size_t pos = 0;
    put_u8(out, &pos, mask);
    put_u8(out, &pos, div);
    put_u16(out, &pos, effectiveRate);

    ESP_LOGI(TAG, "ADC stream started: mask=0x%02X div=%d rate=%d", mask, div, effectiveRate);
    return (int)pos;
}

static int handleStopAdcStream(uint16_t seq, uint8_t cmdId, uint8_t *out)
{
    s_adcStreamMask = 0;
    ESP_LOGI(TAG, "ADC stream stopped");
    return 0;
}

static int handleStartScopeStream(uint16_t seq, uint8_t cmdId, uint8_t *out)
{
    if (s_scopeStreamActive) {
        sendError(seq, cmdId, BBP_ERR_STREAM_ACTIVE);
        return -1;
    }
    // Sync to current scope sequence
    if (xSemaphoreTake(g_stateMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        s_scopeLastSeq = g_deviceState.scope.seq;
        xSemaphoreGive(g_stateMutex);
    }
    s_scopeStreamActive = true;
    ESP_LOGI(TAG, "Scope stream started");
    return 0;
}

static int handleStopScopeStream(uint16_t seq, uint8_t cmdId, uint8_t *out)
{
    s_scopeStreamActive = false;
    ESP_LOGI(TAG, "Scope stream stopped");
    return 0;
}

// --- Waveform Generator commands ---

// Task handle is defined in tasks.cpp; we declare it extern here
extern TaskHandle_t s_wavegenTask;

static int handleStartWavegen(uint16_t seq, uint8_t cmdId,
                               const uint8_t *payload, size_t len, uint8_t *out)
{
    // Payload: channel(u8) + waveform(u8) + freq_hz(f32) + amplitude(f32) + offset(f32) + mode(u8) = 15 bytes
    if (len < 15) { sendError(seq, cmdId, BBP_ERR_INVALID_PARAM); return -1; }

    size_t rpos = 0;
    uint8_t channel = get_u8(payload, &rpos);
    uint8_t waveform = get_u8(payload, &rpos);
    float freq_hz = get_f32(payload, &rpos);
    float amplitude = get_f32(payload, &rpos);
    float offset = get_f32(payload, &rpos);
    uint8_t mode = get_u8(payload, &rpos);

    if (channel >= 4) { sendError(seq, cmdId, BBP_ERR_INVALID_CH); return -1; }
    if (waveform > 3) { sendError(seq, cmdId, BBP_ERR_INVALID_PARAM); return -1; }
    if (mode > 1) { sendError(seq, cmdId, BBP_ERR_INVALID_PARAM); return -1; }
    if (freq_hz < 0.1f || freq_hz > 100.0f) { sendError(seq, cmdId, BBP_ERR_INVALID_PARAM); return -1; }

    // Set channel function to VOUT or IOUT first
    {
        Command cmd = {};
        cmd.type = CMD_SET_CHANNEL_FUNC;
        cmd.channel = channel;
        cmd.func = (mode == 1) ? CH_FUNC_IOUT : CH_FUNC_VOUT;
        sendCommand(cmd);
    }

    // Store wavegen state and notify the task
    if (xSemaphoreTake(g_stateMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        g_deviceState.wavegen.active    = true;
        g_deviceState.wavegen.channel   = channel;
        g_deviceState.wavegen.waveform  = (WaveformType)waveform;
        g_deviceState.wavegen.freq_hz   = freq_hz;
        g_deviceState.wavegen.amplitude = amplitude;
        g_deviceState.wavegen.offset    = offset;
        g_deviceState.wavegen.mode      = (WavegenMode)mode;
        xSemaphoreGive(g_stateMutex);
    }

    // Wake up the wavegen task
    if (s_wavegenTask) {
        xTaskNotifyGive(s_wavegenTask);
    }

    ESP_LOGI(TAG, "Wavegen start: ch=%d wf=%d freq=%.1f amp=%.2f off=%.2f mode=%d",
             channel, waveform, freq_hz, amplitude, offset, mode);

    // Echo params as response
    size_t pos = 0;
    put_u8(out, &pos, channel);
    put_u8(out, &pos, waveform);
    put_f32(out, &pos, freq_hz);
    put_f32(out, &pos, amplitude);
    put_f32(out, &pos, offset);
    put_u8(out, &pos, mode);
    return (int)pos;
}

static void wavegen_stop_and_reset(void)
{
    uint8_t ch = 0;
    if (xSemaphoreTake(g_stateMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        ch = g_deviceState.wavegen.channel;
        g_deviceState.wavegen.active = false;
        xSemaphoreGive(g_stateMutex);
    }
    // Set channel back to HIGH_IMP
    Command cmd = {};
    cmd.type = CMD_SET_CHANNEL_FUNC;
    cmd.channel = ch;
    cmd.func = CH_FUNC_HIGH_IMP;
    sendCommand(cmd);
}

static int handleStopWavegen(uint16_t seq, uint8_t cmdId, uint8_t *out)
{
    wavegen_stop_and_reset();
    ESP_LOGI(TAG, "Wavegen stopped, channel set to HIGH_IMP");
    return 0;
}

// Public: stop wavegen (called on disconnect/reset)
void bbpStopWavegen(void)
{
    wavegen_stop_and_reset();
}

bool bbpWavegenActive(void)
{
    bool active = false;
    if (xSemaphoreTake(g_stateMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        active = g_deviceState.wavegen.active;
        xSemaphoreGive(g_stateMutex);
    }
    return active;
}

// -----------------------------------------------------------------------------
// Message dispatcher
// -----------------------------------------------------------------------------

static void dispatchMessage(const uint8_t *msg, size_t msgLen)
{
    if (msgLen < BBP_MIN_MSG_SIZE) return;

    // Verify CRC
    uint16_t rxCrc = msg[msgLen - 2] | ((uint16_t)msg[msgLen - 1] << 8);
    uint16_t calcCrc = bbp_crc16(msg, msgLen - 2);
    if (rxCrc != calcCrc) {
        ESP_LOGW(TAG, "CRC mismatch: rx=0x%04X calc=0x%04X", rxCrc, calcCrc);
        return;  // Silently discard
    }

    uint8_t  msgType = msg[0];
    uint16_t seq     = msg[1] | ((uint16_t)msg[2] << 8);
    uint8_t  cmdId   = msg[3];

    if (msgType != BBP_MSG_CMD) {
        // Device only accepts CMD messages from host
        return;
    }

    const uint8_t *payload    = msg + BBP_HEADER_SIZE;
    size_t         payloadLen = msgLen - BBP_HEADER_SIZE - BBP_CRC_SIZE;

    // Response buffer
    uint8_t rspBuf[512];
    int rspLen = 0;

    s_lastFrameMs = millis_now();

    switch (cmdId) {
        // --- Status & Info ---
        case BBP_CMD_GET_STATUS:
            rspLen = handleGetStatus(seq, cmdId, rspBuf);
            break;
        case BBP_CMD_GET_DEVICE_INFO:
            rspLen = handleGetDeviceInfo(seq, cmdId, rspBuf);
            break;
        case BBP_CMD_GET_FAULTS:
            rspLen = handleGetFaults(seq, cmdId, rspBuf);
            break;
        case BBP_CMD_GET_DIAGNOSTICS:
            rspLen = handleGetDiagnostics(seq, cmdId, rspBuf);
            break;

        // --- Self-Test / Calibration ---
        case BBP_CMD_SELFTEST_STATUS:
            rspLen = handleSelftestStatus(seq, cmdId, rspBuf);
            break;
        case BBP_CMD_SELFTEST_MEASURE_SUPPLY:
            rspLen = handleSelftestMeasureSupply(seq, cmdId, payload, payloadLen, rspBuf);
            break;
        case BBP_CMD_SELFTEST_EFUSE_CURRENTS:
            rspLen = handleSelftestEfuseCurrents(seq, cmdId, rspBuf);
            break;
        case BBP_CMD_SELFTEST_AUTO_CAL:
            rspLen = handleSelftestAutoCal(seq, cmdId, payload, payloadLen, rspBuf);
            break;

        // --- Channel Config ---
        case BBP_CMD_SET_CH_FUNC:
            rspLen = handleSetChFunc(seq, cmdId, payload, payloadLen, rspBuf);
            break;
        case BBP_CMD_SET_DAC_CODE:
            rspLen = handleSetDacCode(seq, cmdId, payload, payloadLen, rspBuf);
            break;
        case BBP_CMD_SET_DAC_VOLTAGE:
            rspLen = handleSetDacVoltage(seq, cmdId, payload, payloadLen, rspBuf);
            break;
        case BBP_CMD_SET_DAC_CURRENT:
            rspLen = handleSetDacCurrent(seq, cmdId, payload, payloadLen, rspBuf);
            break;
        case BBP_CMD_SET_ADC_CONFIG:
            rspLen = handleSetAdcConfig(seq, cmdId, payload, payloadLen, rspBuf);
            break;
        case BBP_CMD_SET_DIN_CONFIG:
            rspLen = handleSetDinConfig(seq, cmdId, payload, payloadLen, rspBuf);
            break;
        case BBP_CMD_SET_DO_CONFIG:
            rspLen = handleSetDoConfig(seq, cmdId, payload, payloadLen, rspBuf);
            break;
        case BBP_CMD_SET_DO_STATE:
            rspLen = handleSetDoState(seq, cmdId, payload, payloadLen, rspBuf);
            break;
        case BBP_CMD_SET_VOUT_RANGE:
            rspLen = handleSetVoutRange(seq, cmdId, payload, payloadLen, rspBuf);
            break;
        case BBP_CMD_SET_ILIMIT:
            rspLen = handleSetIlimit(seq, cmdId, payload, payloadLen, rspBuf);
            break;
        case BBP_CMD_SET_AVDD_SEL:
            rspLen = handleSetAvddSel(seq, cmdId, payload, payloadLen, rspBuf);
            break;
        case BBP_CMD_GET_ADC_VALUE:
            rspLen = handleGetAdcValue(seq, cmdId, payload, payloadLen, rspBuf);
            break;
        case BBP_CMD_GET_DAC_READBACK:
            rspLen = handleGetDacReadback(seq, cmdId, payload, payloadLen, rspBuf);
            break;
        case BBP_CMD_SET_RTD_CONFIG:
            rspLen = handleSetRtdConfig(seq, cmdId, payload, payloadLen, rspBuf);
            break;

        // --- Faults ---
        case BBP_CMD_CLEAR_ALL_ALERTS:
            rspLen = handleClearAllAlerts(seq, cmdId, rspBuf);
            break;
        case BBP_CMD_CLEAR_CH_ALERT:
            rspLen = handleClearChAlert(seq, cmdId, payload, payloadLen, rspBuf);
            break;
        case BBP_CMD_SET_ALERT_MASK:
            rspLen = handleSetAlertMask(seq, cmdId, payload, payloadLen, rspBuf);
            break;
        case BBP_CMD_SET_CH_ALERT_MASK:
            rspLen = handleSetChAlertMask(seq, cmdId, payload, payloadLen, rspBuf);
            break;

        // --- Diagnostics ---
        case BBP_CMD_SET_DIAG_CONFIG:
            rspLen = handleSetDiagConfig(seq, cmdId, payload, payloadLen, rspBuf);
            break;

        // --- GPIO ---
        case BBP_CMD_GET_GPIO_STATUS:
            rspLen = handleGetGpioStatus(seq, cmdId, rspBuf);
            break;
        case BBP_CMD_SET_GPIO_CONFIG:
            rspLen = handleSetGpioConfig(seq, cmdId, payload, payloadLen, rspBuf);
            break;
        case BBP_CMD_SET_GPIO_VALUE:
            rspLen = handleSetGpioValue(seq, cmdId, payload, payloadLen, rspBuf);
            break;

        // --- Digital IO (ESP32 GPIO) ---
        case BBP_CMD_DIO_GET_ALL:
            rspLen = handleDioGetAll(seq, cmdId, rspBuf);
            break;
        case BBP_CMD_DIO_CONFIG:
            rspLen = handleDioConfig(seq, cmdId, payload, payloadLen, rspBuf);
            break;
        case BBP_CMD_DIO_WRITE:
            rspLen = handleDioWrite(seq, cmdId, payload, payloadLen, rspBuf);
            break;
        case BBP_CMD_DIO_READ:
            rspLen = handleDioRead(seq, cmdId, payload, payloadLen, rspBuf);
            break;

        // --- UART Bridge ---
        case BBP_CMD_GET_UART_CONFIG:
            rspLen = handleGetUartConfig(seq, cmdId, rspBuf);
            break;
        case BBP_CMD_SET_UART_CONFIG:
            rspLen = handleSetUartConfig(seq, cmdId, payload, payloadLen, rspBuf);
            break;
        case BBP_CMD_GET_UART_PINS:
            rspLen = handleGetUartPins(seq, cmdId, rspBuf);
            break;

        // --- Streaming ---
        case BBP_CMD_START_ADC_STREAM:
            rspLen = handleStartAdcStream(seq, cmdId, payload, payloadLen, rspBuf);
            break;
        case BBP_CMD_STOP_ADC_STREAM:
            rspLen = handleStopAdcStream(seq, cmdId, rspBuf);
            break;
        case BBP_CMD_START_SCOPE_STREAM:
            rspLen = handleStartScopeStream(seq, cmdId, rspBuf);
            break;
        case BBP_CMD_STOP_SCOPE_STREAM:
            rspLen = handleStopScopeStream(seq, cmdId, rspBuf);
            break;

        // --- Waveform Generator ---
        case BBP_CMD_START_WAVEGEN:
            rspLen = handleStartWavegen(seq, cmdId, payload, payloadLen, rspBuf);
            break;
        case BBP_CMD_STOP_WAVEGEN:
            rspLen = handleStopWavegen(seq, cmdId, rspBuf);
            break;

        // --- System ---
        case BBP_CMD_DEVICE_RESET:
            rspLen = handleDeviceReset(seq, cmdId, rspBuf);
            break;

        // --- DS4424 IDAC ---
        case BBP_CMD_IDAC_GET_STATUS:
            rspLen = handleIdacGetStatus(seq, cmdId, rspBuf);
            break;
        case BBP_CMD_IDAC_SET_CODE:
            rspLen = handleIdacSetCode(seq, cmdId, payload, payloadLen, rspBuf);
            break;
        case BBP_CMD_IDAC_SET_VOLTAGE:
            rspLen = handleIdacSetVoltage(seq, cmdId, payload, payloadLen, rspBuf);
            break;
        case BBP_CMD_IDAC_CALIBRATE:
            rspLen = handleIdacCalibrate(seq, cmdId, payload, payloadLen, rspBuf);
            break;
        case BBP_CMD_IDAC_CAL_ADD_POINT:
            rspLen = handleIdacCalAddPoint(seq, cmdId, payload, payloadLen, rspBuf);
            break;
        case BBP_CMD_IDAC_CAL_CLEAR:
            rspLen = handleIdacCalClear(seq, cmdId, payload, payloadLen, rspBuf);
            break;
        case BBP_CMD_IDAC_CAL_SAVE:
            rspLen = handleIdacCalSave(seq, cmdId, rspBuf);
            break;

        // --- PCA9535 GPIO Expander ---
        case BBP_CMD_PCA_GET_STATUS:
            rspLen = handlePcaGetStatus(seq, cmdId, rspBuf);
            break;
        case BBP_CMD_PCA_SET_CONTROL:
            rspLen = handlePcaSetControl(seq, cmdId, payload, payloadLen, rspBuf);
            break;
        case BBP_CMD_PCA_SET_PORT:
            rspLen = handlePcaSetPort(seq, cmdId, payload, payloadLen, rspBuf);
            break;

        // --- HUSB238 USB PD ---
        case BBP_CMD_USBPD_GET_STATUS:
            rspLen = handleUsbpdGetStatus(seq, cmdId, rspBuf);
            break;
        case BBP_CMD_USBPD_SELECT_PDO:
            rspLen = handleUsbpdSelectPdo(seq, cmdId, payload, payloadLen, rspBuf);
            break;
        case BBP_CMD_USBPD_GO:
            rspLen = handleUsbpdGo(seq, cmdId, payload, payloadLen, rspBuf);
            break;

        // --- MUX ---
        case BBP_CMD_MUX_SET_ALL:
            rspLen = handleMuxSetAll(seq, cmdId, payload, payloadLen, rspBuf);
            break;
        case BBP_CMD_MUX_GET_ALL:
            rspLen = handleMuxGetAll(seq, cmdId, rspBuf);
            break;
        case BBP_CMD_MUX_SET_SWITCH:
            rspLen = handleMuxSetSwitch(seq, cmdId, payload, payloadLen, rspBuf);
            break;

        case BBP_CMD_REG_READ:
            rspLen = handleRegRead(seq, cmdId, payload, payloadLen, rspBuf);
            break;
        case BBP_CMD_REG_WRITE:
            rspLen = handleRegWrite(seq, cmdId, payload, payloadLen, rspBuf);
            break;
        case BBP_CMD_SET_SPI_CLOCK:
            rspLen = handleSetSpiClock(seq, cmdId, payload, payloadLen, rspBuf);
            break;
        case BBP_CMD_SET_LSHIFT_OE:
            rspLen = handleSetLshiftOe(seq, cmdId, payload, payloadLen, rspBuf);
            break;
        case BBP_CMD_WIFI_GET_STATUS:
            rspLen = handleWifiGetStatus(seq, cmdId, payload, payloadLen, rspBuf);
            break;
        case BBP_CMD_WIFI_CONNECT:
            rspLen = handleWifiConnect(seq, cmdId, payload, payloadLen, rspBuf);
            break;
        case BBP_CMD_WIFI_SCAN:
            rspLen = handleWifiScan(seq, cmdId, payload, payloadLen, rspBuf);
            break;
        case BBP_CMD_PING:
            rspLen = handlePing(seq, cmdId, payload, payloadLen, rspBuf);
            break;
        case BBP_CMD_DISCONNECT:
            sendResponse(seq, cmdId, NULL, 0);
            bbpExitBinaryMode();
            return;  // Don't send another response

        default:
            sendError(seq, cmdId, BBP_ERR_INVALID_CMD);
            return;
    }

    // Send response if handler succeeded (rspLen >= 0)
    if (rspLen >= 0) {
        sendResponse(seq, cmdId, rspBuf, (size_t)rspLen);
    }
}

// -----------------------------------------------------------------------------
// Streaming: drain ADC ring buffer and send batched events
// -----------------------------------------------------------------------------

static void processAdcStream(void)
{
    if (s_adcStreamMask == 0) return;

    // Count available samples (acquire barrier ensures we see data written before head)
    uint16_t head = s_adcBuf.head;
    __sync_synchronize();
    uint16_t tail = s_adcBuf.tail;
    uint16_t available = (head - tail) & (BBP_ADC_STREAM_BUF_SIZE - 1);
    if (available == 0) return;

    // Drain up to ADC_BATCH_MAX samples
    uint16_t count = (available > ADC_BATCH_MAX) ? ADC_BATCH_MAX : available;
    for (uint16_t i = 0; i < count; i++) {
        s_adcBatch[i] = s_adcBuf.samples[tail & (BBP_ADC_STREAM_BUF_SIZE - 1)];
        tail++;
    }
    s_adcBuf.tail = tail;

    // Count active channels
    uint8_t mask = s_adcStreamMask;
    uint8_t numCh = 0;
    for (uint8_t b = 0; b < 4; b++) {
        if (mask & (1 << b)) numCh++;
    }

    // Build ADC_DATA event payload
    // Header: mask(1) + timestamp(4) + count(2) = 7
    // Samples: count * numCh * 3
    uint8_t evtBuf[700];  // 7 + 50*4*3 = 607 max
    size_t pos = 0;

    put_u8(evtBuf, &pos, mask);
    put_u32(evtBuf, &pos, s_adcBatch[0].timestamp_us);
    put_u16(evtBuf, &pos, count);

    for (uint16_t i = 0; i < count; i++) {
        for (uint8_t ch = 0; ch < 4; ch++) {
            if (mask & (1 << ch)) {
                put_u24(evtBuf, &pos, s_adcBatch[i].raw[ch]);
            }
        }
    }

    sendEvent(BBP_EVT_ADC_DATA, evtBuf, pos);
}

// -----------------------------------------------------------------------------
// Streaming: push new scope buckets
// -----------------------------------------------------------------------------

static void processScopeStream(void)
{
    if (!s_scopeStreamActive) return;

    if (xSemaphoreTake(g_stateMutex, pdMS_TO_TICKS(10)) != pdTRUE) return;

    uint16_t currentSeq = g_deviceState.scope.seq;
    if (currentSeq == s_scopeLastSeq) {
        xSemaphoreGive(g_stateMutex);
        return;
    }

    // How many new buckets? (handle wrap)
    uint16_t newBuckets = currentSeq - s_scopeLastSeq;
    if (newBuckets > SCOPE_BUF_SIZE) newBuckets = SCOPE_BUF_SIZE;

    // Send each new bucket as a SCOPE_DATA event
    for (uint16_t i = 0; i < newBuckets; i++) {
        uint16_t bucketSeq = s_scopeLastSeq + i + 1;
        uint16_t idx = (g_deviceState.scope.head - (currentSeq - bucketSeq) + SCOPE_BUF_SIZE)
                       % SCOPE_BUF_SIZE;
        // Avoid sending partially-written data: only send if this index is valid
        if (idx >= SCOPE_BUF_SIZE) continue;
        const ScopeBucket &b = g_deviceState.scope.buckets[idx];

        uint8_t evtBuf[64];
        size_t pos = 0;
        put_u32(evtBuf, &pos, bucketSeq);
        put_u32(evtBuf, &pos, b.timestamp_ms);
        put_u16(evtBuf, &pos, b.count);

        for (uint8_t ch = 0; ch < 4; ch++) {
            float avg = (b.count > 0) ? (b.vSum[ch] / b.count) : 0.0f;
            put_f32(evtBuf, &pos, avg);
            put_f32(evtBuf, &pos, b.vMin[ch]);
            put_f32(evtBuf, &pos, b.vMax[ch]);
        }

        sendEvent(BBP_EVT_SCOPE_DATA, evtBuf, pos);
    }

    s_scopeLastSeq = currentSeq;
    xSemaphoreGive(g_stateMutex);
}

// -----------------------------------------------------------------------------
// Public API
// -----------------------------------------------------------------------------

void bbpInit(AD74416H *device, AD74416H_SPI *spi)
{
    s_dev = device;
    s_spi = spi;
    s_active = false;
    s_magic_idx = 0;
    s_adcStreamMask = 0;
    s_scopeStreamActive = false;
    s_evtSeq = 0;
    memset(&s_adcBuf, 0, sizeof(s_adcBuf));
    ESP_LOGI(TAG, "BBP initialized (proto v%d, fw v%d.%d.%d)",
             BBP_PROTO_VERSION, BBP_FW_VERSION_MAJOR,
             BBP_FW_VERSION_MINOR, BBP_FW_VERSION_PATCH);
}

bool bbpIsActive(void)
{
    return s_active;
}

bool bbpDetectHandshake(uint8_t byte)
{
    if (byte == s_magic[s_magic_idx]) {
        s_magic_idx++;
        if (s_magic_idx == BBP_MAGIC_LEN) {
            s_magic_idx = 0;

            // Send handshake response
            uint8_t rsp[BBP_HANDSHAKE_RSP_LEN] = {
                BBP_MAGIC_0, BBP_MAGIC_1, BBP_MAGIC_2, BBP_MAGIC_3,
                BBP_PROTO_VERSION,
                BBP_FW_VERSION_MAJOR, BBP_FW_VERSION_MINOR, BBP_FW_VERSION_PATCH
            };
            usb_cdc_cli_write(rsp, BBP_HANDSHAKE_RSP_LEN);
            usb_cdc_cli_flush();

            // Suppress ALL log output to prevent ESP_LOG from corrupting
            // the binary stream on CDC #0
            esp_log_level_set("*", ESP_LOG_NONE);

            // Enter binary mode
            s_active = true;
            s_rxLen = 0;
            s_evtSeq = 0;
            s_lastFrameMs = millis_now();
            s_adcStreamMask = 0;
            s_scopeStreamActive = false;

            return true;
        }
    } else {
        s_magic_idx = 0;
        // Check if this byte starts a new match
        if (byte == s_magic[0]) {
            s_magic_idx = 1;
        }
    }
    return false;
}

void bbpExitBinaryMode(void)
{
    s_adcStreamMask = 0;
    s_scopeStreamActive = false;
    bbpStopWavegen();  // Stop wavegen on disconnect
    s_active = false;
    s_rxLen = 0;
    s_magic_idx = 0;

    // Restore log output for CLI mode
    esp_log_level_set("*", ESP_LOG_INFO);

    // NOTE: Do NOT write "[CLI Ready]" text to CDC #0 here. The desktop app
    // uses CDC #0 exclusively for binary BBP frames; injecting ASCII text
    // corrupts the FrameAccumulator, causes CRC mismatches, and triggers a
    // spurious disconnection. The desktop detects binary-mode exit via timeout.
    ESP_LOGI(TAG, "Binary mode deactivated, CLI ready");
}

void bbpProcess(void)
{
    if (!s_active) return;

    // Check idle timeout
    if (millis_now() - s_lastFrameMs > BBP_IDLE_TIMEOUT_MS) {
        ESP_LOGW(TAG, "Idle timeout, reverting to CLI");
        bbpExitBinaryMode();
        return;
    }

    // --- Read incoming bytes and extract COBS frames ---
    uint8_t rxChunk[128];
    uint32_t avail = usb_cdc_cli_available();
    while (avail > 0) {
        uint32_t toRead = (avail > sizeof(rxChunk)) ? sizeof(rxChunk) : avail;
        uint32_t n = usb_cdc_cli_read(rxChunk, toRead);
        if (n == 0) break;

        for (uint32_t i = 0; i < n; i++) {
            uint8_t byte = rxChunk[i];

            if (byte == BBP_FRAME_DELIMITER) {
                // End of frame - decode and dispatch
                if (s_rxLen > 0) {
                    uint8_t decoded[BBP_MAX_PAYLOAD];
                    size_t decodedLen = bbp_cobs_decode(s_rxBuf, s_rxLen, decoded);
                    if (decodedLen >= BBP_MIN_MSG_SIZE && decodedLen <= BBP_MAX_PAYLOAD) {
                        dispatchMessage(decoded, decodedLen);
                    }
                    s_rxLen = 0;
                }
            } else {
                // Accumulate COBS-encoded bytes
                if (s_rxLen < RX_BUF_SIZE) {
                    s_rxBuf[s_rxLen++] = byte;
                } else {
                    // Frame too large, discard
                    s_rxLen = 0;
                }
            }
        }
        avail = usb_cdc_cli_available();
    }

    // --- Process outgoing stream data ---
    processAdcStream();
    processScopeStream();
}

void bbpPushAdcSample(const uint32_t raw[4], uint32_t timestamp_us)
{
    // Lock-free SPSC: only the producer (ADC task) writes head
    uint16_t head = s_adcBuf.head;
    uint16_t next = (head + 1) & (BBP_ADC_STREAM_BUF_SIZE - 1);

    // Check if buffer is full (would overwrite tail)
    if (next == s_adcBuf.tail) {
        return;  // Drop sample (backpressure)
    }

    BbpAdcSample &s = s_adcBuf.samples[head];
    s.raw[0] = raw[0];
    s.raw[1] = raw[1];
    s.raw[2] = raw[2];
    s.raw[3] = raw[3];
    s.timestamp_us = timestamp_us;

    // Memory barrier to ensure sample data is written before head advances
    __sync_synchronize();
    s_adcBuf.head = next;
}

uint8_t bbpAdcStreamMask(void)
{
    return s_adcStreamMask;
}

bool bbpScopeStreamActive(void)
{
    return s_scopeStreamActive;
}
