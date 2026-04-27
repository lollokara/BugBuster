// =============================================================================
// bbp.cpp - BugBuster Binary Protocol implementation
//
// COBS-framed binary protocol over USB CDC #0.
// Handles command dispatch, response building, and ADC/scope streaming.
// =============================================================================

#include "bbp.h"
#include "bbp_codec.h"
#include "autorun.h"
#include "usb_cdc.h"
#include "tasks.h"
#include "config.h"
#include "quicksetup.h"
#include "bbp_adapter.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "esp_mac.h"
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
// Sticky: set on first successful handshake, never cleared until reboot.
// Once a host has spoken BBP on CDC #0, we treat that interface as binary-only
// and suppress any ASCII CLI output forever. This prevents log/prompt text
// from corrupting the stream after a transient binary-mode exit.
static bool     s_cdcClaimed = false;
static uint16_t s_evtSeq = 0;          // Event sequence counter

// Handshake detection state
static uint8_t  s_magic_idx = 0;
static const uint8_t s_magic[BBP_MAGIC_LEN] = {
    BBP_MAGIC_0, BBP_MAGIC_1, BBP_MAGIC_2, BBP_MAGIC_3
};

// ADC stream state
static uint8_t  s_adcStreamMask = 0;    // 0 = inactive

// Scope stream state
static bool     s_scopeStreamActive = false;
static uint16_t s_scopeLastSeq = 0;     // Last scope seq sent

// ADC stream ring buffer (lock-free SPSC)
static BbpAdcStreamBuf s_adcBuf = {};

// RX frame accumulator
#define RX_BUF_SIZE  (BBP_COBS_MAX + 16)
static uint8_t  s_rxBuf[RX_BUF_SIZE];
static uint16_t s_rxLen = 0;

// Single-threaded scratch buffers for BBP decode/dispatch. Keeping them at
// file scope avoids burning another 2 KB of stack every time the desktop
// sends a command.
static uint8_t  s_decodedBuf[BBP_MAX_PAYLOAD];
static uint8_t  s_rspBuf[BBP_MAX_PAYLOAD];

// TX work buffers — protected by s_txMutex since sendMsg can be called from
// multiple tasks (BBP command task + event publishers: alert task, HAT task,
// main ISR deferred task). Without this mutex the shared buffers were being
// clobbered mid-frame, producing corrupt CRCs on the wire (notably on large
// responses like WIFI_GET_STATUS).
static uint8_t  s_msgBuf[BBP_MAX_PAYLOAD];  // Raw message assembly
static uint8_t  s_cobsBuf[BBP_COBS_MAX];    // COBS-encoded output
static SemaphoreHandle_t s_txMutex = nullptr;

// ADC batch buffer for streaming
#define ADC_BATCH_MAX  50
static BbpAdcSample s_adcBatch[ADC_BATCH_MAX];

// Timeout: if no valid frame within 5s after handshake, revert
static uint32_t s_lastFrameMs = 0;
#define BBP_IDLE_TIMEOUT_MS  60000   // 60s — pytest + LA captures can pause longer than 5s

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
// Send a raw COBS-framed message over CDC #0
// -----------------------------------------------------------------------------

static void sendFrame(const uint8_t *msg, size_t msgLen)
{
    size_t cobsLen = bbp_cobs_encode(msg, msgLen, s_cobsBuf);
    s_cobsBuf[cobsLen] = BBP_FRAME_DELIMITER;
    usb_cdc_cli_write(s_cobsBuf, cobsLen + 1);
}

// Build and send a response/event/error message.
// Serialized through s_txMutex because this function uses shared static
// buffers (s_msgBuf, s_cobsBuf) and is called from multiple FreeRTOS tasks.
static void sendMsg(uint8_t msgType, uint16_t seq, uint8_t cmdId,
                    const uint8_t *payload, size_t payloadLen)
{
    if (s_txMutex && xSemaphoreTake(s_txMutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        ESP_LOGW(TAG, "sendMsg: tx mutex timeout (msgType=0x%02X cmd=0x%02X)", msgType, cmdId);
        return;
    }

    size_t pos = 0;
    bbp_put_u8(s_msgBuf, &pos, msgType);
    bbp_put_u16(s_msgBuf, &pos, seq);
    bbp_put_u8(s_msgBuf, &pos, cmdId);
    if (payload && payloadLen > 0) {
        if (pos + payloadLen + 2 > BBP_MAX_PAYLOAD) {
            ESP_LOGW(TAG, "sendMsg: payload too large (%u + %u > %u)", (unsigned)pos, (unsigned)payloadLen, BBP_MAX_PAYLOAD);
            if (s_txMutex) xSemaphoreGive(s_txMutex);
            return;
        }
        memcpy(s_msgBuf + pos, payload, payloadLen);
        pos += payloadLen;
    }
    uint16_t crc = bbp_crc16(s_msgBuf, pos);
    bbp_put_u16(s_msgBuf, &pos, crc);
    sendFrame(s_msgBuf, pos);

    if (s_txMutex) xSemaphoreGive(s_txMutex);
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
    uint16_t seq = (uint16_t)__atomic_fetch_add(&s_evtSeq, 1, __ATOMIC_RELAXED);
    sendMsg(BBP_MSG_EVT, seq, evtId, payload, payloadLen);
}

// Public wrapper for sending events from other modules
void bbpSendEvent(uint8_t evtId, const uint8_t *payload, size_t len)
{
    if (!s_active) return;
    sendEvent(evtId, payload, len);
}

// -----------------------------------------------------------------------------
// Command handlers
// Each returns the response payload length written into `out`, or -1 on error.
// `out` points to a buffer of at least BBP_MAX_PAYLOAD bytes.
// -----------------------------------------------------------------------------

bool bbp_dac_read_active(uint8_t ch, uint16_t *code_out)
{
    if (!s_dev || ch >= 4) return false;
    *code_out = s_dev->getDacActive(ch);
    return true;
}

// Thin public wrappers over s_spi for registry handlers that need raw SPI
// access (cmd_misc.cpp: REG_READ/WRITE/WATCHDOG/SPI_CLOCK,
//         cmd_status.cpp: GET_DEVICE_INFO).
bool bbp_spi_read_reg(uint8_t addr, uint16_t *value_out)
{
    if (!s_spi || !value_out) return false;
    return s_spi->readRegister(addr, value_out);
}

bool bbp_spi_write_reg(uint8_t addr, uint16_t value)
{
    if (!s_spi) return false;
    return s_spi->writeRegister(addr, value);
}

bool bbp_spi_set_clock(uint32_t hz)
{
    if (!s_spi) return false;
    return s_spi->setClockSpeed(hz);
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

    size_t  rsp_len = 0;

    s_lastFrameMs = millis_now();
    autorun_note_inbound();

    // Registry adapter: intercepts all 120 registered opcodes.
    int arc = bbp_adapter_dispatch(cmdId, payload, payloadLen, s_rspBuf, &rsp_len);
    if (arc == 0) {
        sendResponse(seq, cmdId, s_rspBuf, rsp_len);
        return;
    } else if (arc > 0) {
        sendError(seq, cmdId, (uint8_t)arc);
        return;
    }
    // arc == -1: opcode not in registry — handle the few non-registry cases below.

    switch (cmdId) {
        case BBP_CMD_DISCONNECT:
            sendResponse(seq, cmdId, NULL, 0);
            bbpExitBinaryMode();
            return;

        default:
            sendError(seq, cmdId, BBP_ERR_INVALID_CMD);
            return;
    }
}

// -----------------------------------------------------------------------------
// Streaming: drain ADC ring buffer and send batched events
// -----------------------------------------------------------------------------

static void processAdcStream(void)
{
    if (s_adcStreamMask == 0) return;

    // Acquire head to see producer's latest progress; read our own tail relaxed
    uint16_t head = __atomic_load_n(&s_adcBuf.head, __ATOMIC_ACQUIRE);
    uint16_t tail = __atomic_load_n(&s_adcBuf.tail, __ATOMIC_RELAXED);
    uint16_t available = (head - tail) & (BBP_ADC_STREAM_BUF_SIZE - 1);
    if (available == 0) return;

    // Drain up to ADC_BATCH_MAX samples
    uint16_t count = (available > ADC_BATCH_MAX) ? ADC_BATCH_MAX : available;
    for (uint16_t i = 0; i < count; i++) {
        s_adcBatch[i] = s_adcBuf.samples[tail & (BBP_ADC_STREAM_BUF_SIZE - 1)];
        tail++;
    }
    // Release tail so producer sees our progress
    __atomic_store_n(&s_adcBuf.tail, tail, __ATOMIC_RELEASE);

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

    bbp_put_u8(evtBuf, &pos, mask);
    bbp_put_u32(evtBuf, &pos, s_adcBatch[0].timestamp_us);
    bbp_put_u16(evtBuf, &pos, count);

    for (uint16_t i = 0; i < count; i++) {
        for (uint8_t ch = 0; ch < 4; ch++) {
            if (mask & (1 << ch)) {
                bbp_put_u24(evtBuf, &pos, s_adcBatch[i].raw[ch]);
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
        bbp_put_u32(evtBuf, &pos, bucketSeq);
        bbp_put_u32(evtBuf, &pos, b.timestamp_ms);
        bbp_put_u16(evtBuf, &pos, b.count);

        for (uint8_t ch = 0; ch < 4; ch++) {
            float avg = (b.count > 0) ? (b.vSum[ch] / b.count) : 0.0f;
            bbp_put_f32(evtBuf, &pos, avg);
            bbp_put_f32(evtBuf, &pos, b.vMin[ch]);
            bbp_put_f32(evtBuf, &pos, b.vMax[ch]);
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
    if (!s_txMutex) {
        s_txMutex = xSemaphoreCreateMutex();
    }
    quicksetup_init();
    ESP_LOGI(TAG, "BBP initialized (proto v%d, fw v%d.%d.%d)",
             BBP_PROTO_VERSION, BBP_FW_VERSION_MAJOR,
             BBP_FW_VERSION_MINOR, BBP_FW_VERSION_PATCH);
}

bool bbpIsActive(void)
{
    return s_active;
}

bool bbpCdcClaimed(void)
{
    return s_cdcClaimed;
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
            esp_read_mac(&rsp[8], ESP_MAC_WIFI_STA);
            usb_cdc_cli_write(rsp, BBP_HANDSHAKE_RSP_LEN);
            usb_cdc_cli_flush();

            // Suppress ALL log output to prevent ESP_LOG from corrupting
            // the binary stream on CDC #0
            esp_log_level_set("*", ESP_LOG_NONE);

            // Enter binary mode
            s_active = true;
            s_cdcClaimed = true;   // Sticky — CDC #0 is now binary-only for the rest of boot
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

    // CRITICAL: Do NOT call ESP_LOGI here and do NOT restore log level yet.
    // Any text written to CDC #0 (via ESP_LOG → stdout → CDC) while a host
    // may still be reading binary frames will corrupt the stream and cause
    // CRC mismatches. Logs stay suppressed until the host explicitly sends
    // a new CLI character (handled elsewhere).
    //
    // The previous version of this function wrote "Binary mode deactivated,
    // CLI ready" via ESP_LOGI, which produced exactly the corruption it
    // warned against.
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

            // Detect re-handshake: if a new host connects and sends the magic
            // while we're still in binary mode (e.g., DTR didn't drop), reset
            // and re-enter binary mode cleanly.
            if (bbpDetectHandshake(byte)) {
                ESP_LOGW(TAG, "Re-handshake detected in binary mode — resetting");
                return;  // bbpDetectHandshake already set s_active, sent response
            }

            if (byte == BBP_FRAME_DELIMITER) {
                // End of frame - decode and dispatch
                if (s_rxLen > 0) {
                    size_t decodedLen = bbp_cobs_decode(s_rxBuf, s_rxLen, s_decodedBuf);
                    if (decodedLen >= BBP_MIN_MSG_SIZE && decodedLen <= BBP_MAX_PAYLOAD) {
                        dispatchMessage(s_decodedBuf, decodedLen);
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
    uint16_t head = __atomic_load_n(&s_adcBuf.head, __ATOMIC_RELAXED);
    uint16_t next = (head + 1) & (BBP_ADC_STREAM_BUF_SIZE - 1);

    // Check if buffer is full — acquire tail to see consumer's latest progress
    uint16_t tail = __atomic_load_n(&s_adcBuf.tail, __ATOMIC_ACQUIRE);
    if (next == tail) {
        return;  // Drop sample (backpressure)
    }

    BbpAdcSample &s = s_adcBuf.samples[head];
    s.raw[0] = raw[0];
    s.raw[1] = raw[1];
    s.raw[2] = raw[2];
    s.raw[3] = raw[3];
    s.timestamp_us = timestamp_us;

    // Release barrier: ensure sample data is visible before head advances
    __atomic_store_n(&s_adcBuf.head, next, __ATOMIC_RELEASE);
}

uint8_t bbpAdcStreamMask(void)
{
    return s_adcStreamMask;
}

void bbpStartAdcStream(uint8_t mask, uint8_t div, uint16_t *rate_out)
{
    if (div == 0) div = 1;

    // Reset ring buffer
    s_adcBuf.head  = 0;
    s_adcBuf.tail  = 0;

    s_adcStreamMask = mask;

    // Estimate effective sample rate from fastest active channel
    uint16_t effectiveRate = 20; // default SPS
    if (xSemaphoreTake(g_stateMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        for (uint8_t ch = 0; ch < 4; ch++) {
            if (mask & (1u << ch)) {
                AdcRate r = g_deviceState.channels[ch].adcRate;
                uint16_t sps = 20;
                switch (r) {
                    case ADC_RATE_10SPS_H:   sps = 10;   break;
                    case ADC_RATE_20SPS:
                    case ADC_RATE_20SPS_H:   sps = 20;   break;
                    case ADC_RATE_200SPS_H1:
                    case ADC_RATE_200SPS_H:  sps = 200;  break;
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

    ESP_LOGI(TAG, "ADC stream started: mask=0x%02X div=%d rate=%d", mask, div, effectiveRate);

    if (rate_out) *rate_out = effectiveRate;
}

void bbpStopAdcStream(void)
{
    s_adcStreamMask = 0;
    ESP_LOGI(TAG, "ADC stream stopped");
}

bool bbpScopeStreamActive(void)
{
    return s_scopeStreamActive;
}

void bbpStartScopeStream(void)
{
    // Sync to current scope sequence so we don't re-send stale frames
    if (xSemaphoreTake(g_stateMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        s_scopeLastSeq = g_deviceState.scope.seq;
        xSemaphoreGive(g_stateMutex);
    }
    s_scopeStreamActive = true;
    ESP_LOGI(TAG, "Scope stream started");
}

void bbpStopScopeStream(void)
{
    s_scopeStreamActive = false;
    ESP_LOGI(TAG, "Scope stream stopped");
}

// Task handle is defined in tasks.cpp
extern TaskHandle_t s_wavegenTask;

void bbpStartWavegen(uint8_t channel, uint8_t waveform,
                     float freq_hz, float amplitude, float offset,
                     uint8_t mode)
{
    // Apply channel function synchronously before waking wavegen task.
    // (Wavegen task is priority 3, command processor priority 2 — direct call
    // avoids the scheduler race that would let wavegen write to an unconfigured
    // channel.)
    tasks_apply_channel_function(channel, (mode == 1) ? CH_FUNC_IOUT : CH_FUNC_VOUT);

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

    if (s_wavegenTask) {
        xTaskNotifyGive(s_wavegenTask);
    }

    ESP_LOGI(TAG, "Wavegen start: ch=%d wf=%d freq=%.1f amp=%.2f off=%.2f mode=%d",
             channel, waveform, freq_hz, amplitude, offset, mode);
}
