// =============================================================================
// webserver.cpp - HTTP API server for AD74416H controller (ESP-IDF httpd)
// =============================================================================

#include "webserver.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "esp_http_server.h"
#include "cJSON.h"
#include "esp_log.h"
#include "esp_spiffs.h"
#include "esp_timer.h"

#include "tasks.h"
#include "config.h"
#include "ad74416h.h"
#include "ad74416h_spi.h"
#include "ad74416h_regs.h"
#include "uart_bridge.h"

extern AD74416H_SPI spiDriver;

static const char* TAG = "webserver";

static httpd_handle_t s_server = NULL;

// -----------------------------------------------------------------------------
// Helper: CORS headers
// -----------------------------------------------------------------------------

static void set_cors_headers(httpd_req_t *req)
{
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Methods", "GET, POST, OPTIONS");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Headers", "Content-Type");
}

// -----------------------------------------------------------------------------
// Helper: send a cJSON object as an HTTP response (deletes root after send)
// -----------------------------------------------------------------------------

static esp_err_t send_json(httpd_req_t *req, cJSON *root, int code = 200)
{
    char *body = cJSON_PrintUnformatted(root);
    set_cors_headers(req);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");

    if (code != 200) {
        char status[16];
        snprintf(status, sizeof(status), "%d", code);
        httpd_resp_set_status(req, status);
    }

    httpd_resp_sendstr(req, body);
    free(body);
    cJSON_Delete(root);
    return ESP_OK;
}

// -----------------------------------------------------------------------------
// Helper: send JSON error
// -----------------------------------------------------------------------------

static esp_err_t send_error(httpd_req_t *req, int code, const char *msg)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "error", msg);
    return send_json(req, root, code);
}

// -----------------------------------------------------------------------------
// Helper: receive and parse JSON body from request
// -----------------------------------------------------------------------------

static cJSON* recv_json_body(httpd_req_t *req)
{
    int total = req->content_len;
    if (total <= 0 || total > 1024) return NULL;
    char *buf = (char*)malloc(total + 1);
    if (!buf) return NULL;
    int received = 0;
    while (received < total) {
        int ret = httpd_req_recv(req, buf + received, total - received);
        if (ret <= 0) { free(buf); return NULL; }
        received += ret;
    }
    buf[total] = '\0';
    cJSON *root = cJSON_Parse(buf);
    free(buf);
    return root;
}

// -----------------------------------------------------------------------------
// Helper: extract channel number from URI   /api/channel/X/...
// -----------------------------------------------------------------------------

static int extract_channel(const char* uri)
{
    const char *prefix = "/api/channel/";
    const char *p = strstr(uri, prefix);
    if (!p) return -1;
    p += strlen(prefix);
    if (*p >= '0' && *p <= '3') return *p - '0';
    return -1;
}

// -----------------------------------------------------------------------------
// Helper: extract GPIO number from URI   /api/gpio/X/...
// -----------------------------------------------------------------------------

static int extract_gpio(const char* uri)
{
    const char *prefix = "/api/gpio/";
    const char *p = strstr(uri, prefix);
    if (!p) return -1;
    p += strlen(prefix);
    if (*p >= '0' && *p <= '5') return *p - '0';
    return -1;
}

// -----------------------------------------------------------------------------
// Helper: extract number from /api/faults/clear/X or /api/faults/mask/X
// -----------------------------------------------------------------------------

static int extract_fault_channel(const char* uri, const char* prefix)
{
    const char *p = strstr(uri, prefix);
    if (!p) return -1;
    p += strlen(prefix);
    if (*p >= '0' && *p <= '3') return *p - '0';
    return -1;
}

// -----------------------------------------------------------------------------
// channelFunctionToString
// -----------------------------------------------------------------------------

const char* channelFunctionToString(ChannelFunction f)
{
    switch (f) {
        case CH_FUNC_HIGH_IMP:          return "HIGH_IMP";
        case CH_FUNC_VOUT:              return "VOUT";
        case CH_FUNC_IOUT:              return "IOUT";
        case CH_FUNC_VIN:               return "VIN";
        case CH_FUNC_IIN_EXT_PWR:       return "IIN_EXT_PWR";
        case CH_FUNC_IIN_LOOP_PWR:      return "IIN_LOOP_PWR";
        case CH_FUNC_RES_MEAS:          return "RES_MEAS";
        case CH_FUNC_DIN_LOGIC:         return "DIN_LOGIC";
        case CH_FUNC_DIN_LOOP:          return "DIN_LOOP";
        case CH_FUNC_IOUT_HART:         return "IOUT_HART";
        case CH_FUNC_IIN_EXT_PWR_HART:  return "IIN_EXT_PWR_HART";
        case CH_FUNC_IIN_LOOP_PWR_HART: return "IIN_LOOP_PWR_HART";
        default:                        return "UNKNOWN";
    }
}

// -----------------------------------------------------------------------------
// Diagnostic source name / unit helpers
// -----------------------------------------------------------------------------

static const char* diagSourceName(uint8_t source)
{
    switch (source) {
        case 0:  return "AGND";
        case 1:  return "TEMP";
        case 2:  return "DVCC";
        case 3:  return "AVCC";
        case 4:  return "LDO1V8";
        case 5:  return "AVDD_HI";
        case 6:  return "AVDD_LO";
        case 7:  return "AVSS";
        case 8:  return "LVIN";
        case 9:  return "DO_VDD";
        case 10: return "VSENSEP";
        case 11: return "VSENSEN";
        case 12: return "DO_CURRENT";
        case 13: return "AVDD";
        default: return "UNKNOWN";
    }
}

static const char* diagSourceUnit(uint8_t source)
{
    switch (source) {
        case 1:  return "C";       // Temperature in degrees Celsius
        default: return "V";       // All other sources are voltages
    }
}

// -----------------------------------------------------------------------------
// GPIO mode name helper
// -----------------------------------------------------------------------------

static const char* gpioModeName(uint8_t mode) {
    static const char* names[] = {"HIGH_IMP","OUTPUT","INPUT","DIN_OUT","DO_EXT"};
    return mode < 5 ? names[mode] : "?";
}

// =============================================================================
// Route handler implementations
// =============================================================================

// GET /
static esp_err_t handle_root(httpd_req_t *req)
{
    FILE *f = fopen("/spiffs/index.html", "r");
    if (!f) {
        set_cors_headers(req);
        httpd_resp_send_404(req);
        return ESP_FAIL;
    }
    set_cors_headers(req);
    httpd_resp_set_type(req, "text/html");
    char buf[512];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
        httpd_resp_send_chunk(req, buf, n);
    }
    httpd_resp_send_chunk(req, NULL, 0);  // end chunked response
    fclose(f);
    return ESP_OK;
}

// GET /api/status
static esp_err_t handle_get_status(httpd_req_t *req)
{
    cJSON *root = cJSON_CreateObject();

    if (xSemaphoreTake(g_stateMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        cJSON_AddBoolToObject(root, "spiOk", g_deviceState.spiOk);
        cJSON_AddNumberToObject(root, "dieTemp", g_deviceState.dieTemperature);
        cJSON_AddNumberToObject(root, "alertStatus", g_deviceState.alertStatus);
        cJSON_AddNumberToObject(root, "alertMask", g_deviceState.alertMask);
        cJSON_AddNumberToObject(root, "supplyAlertStatus", g_deviceState.supplyAlertStatus);
        cJSON_AddNumberToObject(root, "supplyAlertMask", g_deviceState.supplyAlertMask);
        cJSON_AddNumberToObject(root, "liveStatus", g_deviceState.liveStatus);

        cJSON *channels = cJSON_AddArrayToObject(root, "channels");
        for (uint8_t ch = 0; ch < AD74416H_NUM_CHANNELS; ch++) {
            const ChannelState& cs = g_deviceState.channels[ch];
            cJSON *obj = cJSON_CreateObject();
            cJSON_AddNumberToObject(obj, "id", ch);
            cJSON_AddStringToObject(obj, "function", channelFunctionToString(cs.function));
            cJSON_AddNumberToObject(obj, "adcRaw", cs.adcRawCode);
            cJSON_AddNumberToObject(obj, "adcValue", cs.adcValue);
            cJSON_AddNumberToObject(obj, "adcRange", (int)cs.adcRange);
            cJSON_AddNumberToObject(obj, "adcRate", (int)cs.adcRate);
            cJSON_AddNumberToObject(obj, "adcMux", (int)cs.adcMux);
            cJSON_AddNumberToObject(obj, "dacCode", cs.dacCode);
            cJSON_AddNumberToObject(obj, "dacValue", cs.dacValue);
            cJSON_AddBoolToObject(obj, "dinState", cs.dinState);
            cJSON_AddNumberToObject(obj, "dinCounter", cs.dinCounter);
            cJSON_AddBoolToObject(obj, "doState", cs.doState);
            cJSON_AddNumberToObject(obj, "channelAlert", cs.channelAlertStatus);
            cJSON_AddNumberToObject(obj, "channelAlertMask", cs.channelAlertMask);
            cJSON_AddItemToArray(channels, obj);
        }

        xSemaphoreGive(g_stateMutex);
    } else {
        cJSON_Delete(root);
        return send_error(req, 503, "State mutex timeout");
    }

    return send_json(req, root);
}

// GET /api/channel/?/adc
static esp_err_t handle_get_channel_adc(httpd_req_t *req)
{
    int ch = extract_channel(req->uri);
    if (ch < 0) return send_error(req, 400, "Channel must be 0-3");

    cJSON *root = cJSON_CreateObject();
    if (xSemaphoreTake(g_stateMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        const ChannelState& cs = g_deviceState.channels[ch];
        cJSON_AddNumberToObject(root, "id", ch);
        cJSON_AddNumberToObject(root, "adcRaw", cs.adcRawCode);
        cJSON_AddNumberToObject(root, "adcValue", cs.adcValue);
        cJSON_AddNumberToObject(root, "adcRange", (int)cs.adcRange);
        cJSON_AddNumberToObject(root, "adcRate", (int)cs.adcRate);
        cJSON_AddNumberToObject(root, "adcMux", (int)cs.adcMux);
        xSemaphoreGive(g_stateMutex);
    } else {
        cJSON_Delete(root);
        return send_error(req, 503, "State mutex timeout");
    }

    return send_json(req, root);
}

// GET /api/faults
static esp_err_t handle_get_faults(httpd_req_t *req)
{
    cJSON *root = cJSON_CreateObject();
    if (xSemaphoreTake(g_stateMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        cJSON_AddNumberToObject(root, "alertStatus", g_deviceState.alertStatus);
        cJSON_AddNumberToObject(root, "alertMask", g_deviceState.alertMask);
        cJSON_AddNumberToObject(root, "supplyAlertStatus", g_deviceState.supplyAlertStatus);
        cJSON_AddNumberToObject(root, "supplyAlertMask", g_deviceState.supplyAlertMask);

        cJSON *channels = cJSON_AddArrayToObject(root, "channels");
        for (uint8_t ch = 0; ch < AD74416H_NUM_CHANNELS; ch++) {
            cJSON *obj = cJSON_CreateObject();
            cJSON_AddNumberToObject(obj, "id", ch);
            cJSON_AddNumberToObject(obj, "channelAlert", g_deviceState.channels[ch].channelAlertStatus);
            cJSON_AddNumberToObject(obj, "channelAlertMask", g_deviceState.channels[ch].channelAlertMask);
            cJSON_AddItemToArray(channels, obj);
        }
        xSemaphoreGive(g_stateMutex);
    } else {
        cJSON_Delete(root);
        return send_error(req, 503, "State mutex timeout");
    }
    return send_json(req, root);
}

// GET /api/scope?since=<seq>  – returns downsampled scope buckets
// Each bucket: [timestamp_ms, ch0_min, ch0_max, ch1_min, ch1_max, ...]
static esp_err_t handle_get_scope(httpd_req_t *req)
{
    uint16_t since_seq = 0;
    char qbuf[32] = {};
    if (httpd_req_get_url_query_str(req, qbuf, sizeof(qbuf)) == ESP_OK) {
        char val[16] = {};
        if (httpd_query_key_value(qbuf, "since", val, sizeof(val)) == ESP_OK) {
            since_seq = (uint16_t)atoi(val);
        }
    }

    cJSON *root = cJSON_CreateObject();
    if (xSemaphoreTake(g_stateMutex, pdMS_TO_TICKS(20)) == pdTRUE) {
        const ScopeBuffer& sb = g_deviceState.scope;
        uint16_t cur_seq = sb.seq;
        uint16_t head    = sb.head;

        uint16_t avail = (uint16_t)(cur_seq - since_seq);
        if (avail > SCOPE_BUF_SIZE) avail = SCOPE_BUF_SIZE;
        if (since_seq == 0) avail = 0; // first call: sync only

        cJSON_AddNumberToObject(root, "seq", cur_seq);
        cJSON *samples = cJSON_AddArrayToObject(root, "s");

        uint16_t start = (head + SCOPE_BUF_SIZE - avail) % SCOPE_BUF_SIZE;
        for (uint16_t n = 0; n < avail; n++) {
            uint16_t idx = (start + n) % SCOPE_BUF_SIZE;
            const ScopeBucket& bk = sb.buckets[idx];
            // Format: [t, ch0avg, ch1avg, ch2avg, ch3avg, ch0min, ch0max, ch1min, ch1max, ...]
            cJSON *pt = cJSON_CreateArray();
            cJSON_AddItemToArray(pt, cJSON_CreateNumber(bk.timestamp_ms));
            float invCount = (bk.count > 0) ? (1.0f / bk.count) : 0.0f;
            for (uint8_t ch = 0; ch < 4; ch++) {
                cJSON_AddItemToArray(pt, cJSON_CreateNumber(bk.vSum[ch] * invCount));
            }
            for (uint8_t ch = 0; ch < 4; ch++) {
                cJSON_AddItemToArray(pt, cJSON_CreateNumber(bk.vMin[ch]));
                cJSON_AddItemToArray(pt, cJSON_CreateNumber(bk.vMax[ch]));
            }
            cJSON_AddItemToArray(samples, pt);
        }

        xSemaphoreGive(g_stateMutex);
    }
    return send_json(req, root);
}

// GET /api/diagnostics
static esp_err_t handle_get_diagnostics(httpd_req_t *req)
{
    cJSON *root = cJSON_CreateObject();

    if (xSemaphoreTake(g_stateMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        cJSON *slots = cJSON_AddArrayToObject(root, "slots");
        for (uint8_t i = 0; i < 4; i++) {
            const DiagState& ds = g_deviceState.diag[i];
            cJSON *obj = cJSON_CreateObject();
            cJSON_AddNumberToObject(obj, "slot", i);
            cJSON_AddNumberToObject(obj, "source", ds.source);
            cJSON_AddStringToObject(obj, "sourceName", diagSourceName(ds.source));
            cJSON_AddNumberToObject(obj, "raw", ds.rawCode);
            cJSON_AddNumberToObject(obj, "value", ds.value);
            cJSON_AddStringToObject(obj, "unit", diagSourceUnit(ds.source));
            cJSON_AddItemToArray(slots, obj);
        }
        xSemaphoreGive(g_stateMutex);
    } else {
        cJSON_Delete(root);
        return send_error(req, 503, "State mutex timeout");
    }

    return send_json(req, root);
}

// GET /api/device/info
static esp_err_t handle_get_device_info(httpd_req_t *req)
{
    uint16_t rev = 0, id0 = 0, id1 = 0;
    spiDriver.readRegister(REG_SILICON_REV, &rev);
    spiDriver.readRegister(REG_SILICON_ID0, &id0);
    spiDriver.readRegister(REG_SILICON_ID1, &id1);

    bool spiOk = false;
    if (xSemaphoreTake(g_stateMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        spiOk = g_deviceState.spiOk;
        xSemaphoreGive(g_stateMutex);
    }

    char id0Str[8], id1Str[8];
    snprintf(id0Str, sizeof(id0Str), "0x%04X", id0);
    snprintf(id1Str, sizeof(id1Str), "0x%04X", id1);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "siliconRev", (int)rev);
    cJSON_AddStringToObject(root, "siliconId0", id0Str);
    cJSON_AddStringToObject(root, "siliconId1", id1Str);
    cJSON_AddBoolToObject(root, "spiOk", spiOk);
    return send_json(req, root);
}

// GET /api/gpio
static esp_err_t handle_get_gpio(httpd_req_t *req)
{
    cJSON *root = cJSON_CreateObject();

    if (xSemaphoreTake(g_stateMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        cJSON *gpios = cJSON_AddArrayToObject(root, "gpios");
        for (uint8_t g = 0; g < AD74416H_NUM_GPIOS; g++) {
            const GpioState& gs = g_deviceState.gpio[g];
            cJSON *obj = cJSON_CreateObject();
            cJSON_AddNumberToObject(obj, "pin", g);
            char name[2] = { (char)('A' + g), '\0' };
            cJSON_AddStringToObject(obj, "name", name);
            cJSON_AddNumberToObject(obj, "mode", gs.mode);
            cJSON_AddStringToObject(obj, "modeName", gpioModeName(gs.mode));
            cJSON_AddBoolToObject(obj, "output", gs.outputVal);
            cJSON_AddBoolToObject(obj, "input", gs.inputVal);
            cJSON_AddBoolToObject(obj, "pulldown", gs.pulldown);
            cJSON_AddItemToArray(gpios, obj);
        }
        xSemaphoreGive(g_stateMutex);
    } else {
        cJSON_Delete(root);
        return send_error(req, 503, "State mutex timeout");
    }

    return send_json(req, root);
}

// GET /api/channel/?/dac/readback
static esp_err_t handle_get_dac_readback(httpd_req_t *req)
{
    int ch = extract_channel(req->uri);
    if (ch < 0) return send_error(req, 400, "Channel must be 0-3");

    uint16_t activeCode = 0;
    spiDriver.readRegister(AD74416H_REG_DAC_ACTIVE(ch), &activeCode);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "channel", ch);
    cJSON_AddNumberToObject(root, "activeCode", (int)activeCode);
    return send_json(req, root);
}

// -------------------------------------------------------------------------
// POST handlers
// -------------------------------------------------------------------------

// POST /api/channel/?/function
static esp_err_t handle_post_channel_function(httpd_req_t *req)
{
    int ch = extract_channel(req->uri);
    if (ch < 0) return send_error(req, 400, "Channel must be 0-3");

    cJSON *doc = recv_json_body(req);
    if (!doc) return send_error(req, 400, "Invalid JSON");

    cJSON *funcItem = cJSON_GetObjectItem(doc, "function");
    if (!funcItem || !cJSON_IsNumber(funcItem)) {
        cJSON_Delete(doc);
        return send_error(req, 400, "Missing 'function' field");
    }

    Command cmd{};
    cmd.type    = CMD_SET_CHANNEL_FUNC;
    cmd.channel = (uint8_t)ch;
    cmd.func    = (ChannelFunction)funcItem->valueint;
    sendCommand(cmd);

    cJSON_Delete(doc);

    cJSON *resp = cJSON_CreateObject();
    cJSON_AddBoolToObject(resp, "ok", true);
    cJSON_AddNumberToObject(resp, "channel", ch);
    cJSON_AddNumberToObject(resp, "function", (int)cmd.func);
    return send_json(req, resp);
}

// POST /api/channel/?/dac
static esp_err_t handle_post_dac(httpd_req_t *req)
{
    int ch = extract_channel(req->uri);
    if (ch < 0) return send_error(req, 400, "Channel must be 0-3");

    cJSON *doc = recv_json_body(req);
    if (!doc) return send_error(req, 400, "Invalid JSON");

    Command cmd{};
    cmd.channel = (uint8_t)ch;

    cJSON *codeItem      = cJSON_GetObjectItem(doc, "code");
    cJSON *voltageItem   = cJSON_GetObjectItem(doc, "voltage");
    cJSON *currentItem   = cJSON_GetObjectItem(doc, "current_mA");

    if (codeItem && cJSON_IsNumber(codeItem)) {
        cmd.type    = CMD_SET_DAC_CODE;
        cmd.dacCode = (uint16_t)codeItem->valueint;
    } else if (voltageItem && cJSON_IsNumber(voltageItem)) {
        cmd.type = CMD_SET_DAC_VOLTAGE;
        cJSON *bipolarItem = cJSON_GetObjectItem(doc, "bipolar");
        bool bipolar = bipolarItem ? cJSON_IsTrue(bipolarItem) : false;
        float v = (float)voltageItem->valuedouble;

        if (bipolar) {
            Command rangeCmd{};
            rangeCmd.type     = CMD_SET_VOUT_RANGE;
            rangeCmd.channel  = (uint8_t)ch;
            rangeCmd.boolVal  = true;
            sendCommand(rangeCmd);
            cmd.floatVal = (v >= 0.0f) ? -0.0001f - v : v;
        } else {
            cmd.floatVal = v;
        }
    } else if (currentItem && cJSON_IsNumber(currentItem)) {
        cmd.type     = CMD_SET_DAC_CURRENT;
        cmd.floatVal = (float)currentItem->valuedouble;
    } else {
        cJSON_Delete(doc);
        return send_error(req, 400, "Body must have 'code', 'voltage', or 'current_mA'");
    }

    sendCommand(cmd);
    cJSON_Delete(doc);

    cJSON *resp = cJSON_CreateObject();
    cJSON_AddBoolToObject(resp, "ok", true);
    cJSON_AddNumberToObject(resp, "channel", ch);
    return send_json(req, resp);
}

// POST /api/channel/?/adc/config
static esp_err_t handle_post_adc_config(httpd_req_t *req)
{
    int ch = extract_channel(req->uri);
    if (ch < 0) return send_error(req, 400, "Channel must be 0-3");

    cJSON *doc = recv_json_body(req);
    if (!doc) return send_error(req, 400, "Invalid JSON");

    Command cmd{};
    cmd.type           = CMD_ADC_CONFIG;
    cmd.channel        = (uint8_t)ch;
    cJSON *muxItem   = cJSON_GetObjectItem(doc, "mux");
    cJSON *rangeItem = cJSON_GetObjectItem(doc, "range");
    cJSON *rateItem  = cJSON_GetObjectItem(doc, "rate");
    cmd.adcCfg.mux     = (AdcConvMux)(muxItem   ? muxItem->valueint   : 0);
    cmd.adcCfg.range   = (AdcRange)  (rangeItem ? rangeItem->valueint : 0);
    cmd.adcCfg.rate    = (AdcRate)   (rateItem  ? rateItem->valueint  : 0);
    sendCommand(cmd);
    cJSON_Delete(doc);

    cJSON *resp = cJSON_CreateObject();
    cJSON_AddBoolToObject(resp, "ok", true);
    cJSON_AddNumberToObject(resp, "channel", ch);
    return send_json(req, resp);
}

// POST /api/channel/?/din/config
static esp_err_t handle_post_din_config(httpd_req_t *req)
{
    int ch = extract_channel(req->uri);
    if (ch < 0) return send_error(req, 400, "Channel must be 0-3");

    cJSON *doc = recv_json_body(req);
    if (!doc) return send_error(req, 400, "Invalid JSON");

    Command cmd{};
    cmd.type              = CMD_DIN_CONFIG;
    cmd.channel           = (uint8_t)ch;

    cJSON *threshItem     = cJSON_GetObjectItem(doc, "thresh");
    cJSON *threshModeItem = cJSON_GetObjectItem(doc, "threshMode");
    cJSON *debounceItem   = cJSON_GetObjectItem(doc, "debounce");
    cJSON *sinkItem       = cJSON_GetObjectItem(doc, "sink");
    cJSON *sinkRangeItem  = cJSON_GetObjectItem(doc, "sinkRange");
    cJSON *ocDetItem      = cJSON_GetObjectItem(doc, "ocDet");
    cJSON *scDetItem      = cJSON_GetObjectItem(doc, "scDet");

    cmd.dinCfg.thresh     = (uint8_t)(threshItem   ? threshItem->valueint   : 0);
    cmd.dinCfg.threshMode = threshModeItem ? cJSON_IsTrue(threshModeItem) : false;
    cmd.dinCfg.debounce   = (uint8_t)(debounceItem ? debounceItem->valueint : 0);
    cmd.dinCfg.sink       = (uint8_t)(sinkItem     ? sinkItem->valueint     : 0);
    cmd.dinCfg.sinkRange  = sinkRangeItem ? cJSON_IsTrue(sinkRangeItem) : false;
    cmd.dinCfg.ocDet      = ocDetItem     ? cJSON_IsTrue(ocDetItem)     : false;
    cmd.dinCfg.scDet      = scDetItem     ? cJSON_IsTrue(scDetItem)     : false;
    sendCommand(cmd);
    cJSON_Delete(doc);

    cJSON *resp = cJSON_CreateObject();
    cJSON_AddBoolToObject(resp, "ok", true);
    cJSON_AddNumberToObject(resp, "channel", ch);
    return send_json(req, resp);
}

// POST /api/channel/?/do/config
static esp_err_t handle_post_do_config(httpd_req_t *req)
{
    int ch = extract_channel(req->uri);
    if (ch < 0) return send_error(req, 400, "Channel must be 0-3");

    cJSON *doc = recv_json_body(req);
    if (!doc) return send_error(req, 400, "Invalid JSON");

    Command cmd{};
    cmd.type              = CMD_DO_CONFIG;
    cmd.channel           = (uint8_t)ch;

    cJSON *modeItem       = cJSON_GetObjectItem(doc, "mode");
    cJSON *srcSelItem     = cJSON_GetObjectItem(doc, "srcSelGpio");
    cJSON *t1Item         = cJSON_GetObjectItem(doc, "t1");
    cJSON *t2Item         = cJSON_GetObjectItem(doc, "t2");

    cmd.doCfg.mode        = (uint8_t)(modeItem   ? modeItem->valueint   : 0);
    cmd.doCfg.srcSelGpio  = srcSelItem ? cJSON_IsTrue(srcSelItem) : false;
    cmd.doCfg.t1          = (uint8_t)(t1Item ? t1Item->valueint : 0);
    cmd.doCfg.t2          = (uint8_t)(t2Item ? t2Item->valueint : 0);
    sendCommand(cmd);
    cJSON_Delete(doc);

    cJSON *resp = cJSON_CreateObject();
    cJSON_AddBoolToObject(resp, "ok", true);
    cJSON_AddNumberToObject(resp, "channel", ch);
    return send_json(req, resp);
}

// POST /api/channel/?/do/set
static esp_err_t handle_post_do_set(httpd_req_t *req)
{
    int ch = extract_channel(req->uri);
    if (ch < 0) return send_error(req, 400, "Channel must be 0-3");

    cJSON *doc = recv_json_body(req);
    if (!doc) return send_error(req, 400, "Invalid JSON");

    cJSON *onItem = cJSON_GetObjectItem(doc, "on");
    Command cmd{};
    cmd.type     = CMD_DO_SET;
    cmd.channel  = (uint8_t)ch;
    cmd.boolVal  = onItem ? cJSON_IsTrue(onItem) : false;
    sendCommand(cmd);
    cJSON_Delete(doc);

    cJSON *resp = cJSON_CreateObject();
    cJSON_AddBoolToObject(resp, "ok", true);
    cJSON_AddNumberToObject(resp, "channel", ch);
    cJSON_AddBoolToObject(resp, "on", cmd.boolVal);
    return send_json(req, resp);
}

// POST /api/channel/?/vout/range
static esp_err_t handle_post_vout_range(httpd_req_t *req)
{
    int ch = extract_channel(req->uri);
    if (ch < 0) return send_error(req, 400, "Channel must be 0-3");

    cJSON *doc = recv_json_body(req);
    if (!doc) return send_error(req, 400, "Invalid JSON");

    cJSON *bipolarItem = cJSON_GetObjectItem(doc, "bipolar");
    Command cmd{};
    cmd.type     = CMD_SET_VOUT_RANGE;
    cmd.channel  = (uint8_t)ch;
    cmd.boolVal  = bipolarItem ? cJSON_IsTrue(bipolarItem) : false;
    sendCommand(cmd);
    cJSON_Delete(doc);

    cJSON *resp = cJSON_CreateObject();
    cJSON_AddBoolToObject(resp, "ok", true);
    cJSON_AddNumberToObject(resp, "channel", ch);
    cJSON_AddBoolToObject(resp, "bipolar", cmd.boolVal);
    return send_json(req, resp);
}

// POST /api/channel/?/ilimit
static esp_err_t handle_post_current_limit(httpd_req_t *req)
{
    int ch = extract_channel(req->uri);
    if (ch < 0) return send_error(req, 400, "Channel must be 0-3");

    cJSON *doc = recv_json_body(req);
    if (!doc) return send_error(req, 400, "Invalid JSON");

    cJSON *limitItem = cJSON_GetObjectItem(doc, "limit8mA");
    Command cmd{};
    cmd.type     = CMD_SET_CURRENT_LIMIT;
    cmd.channel  = (uint8_t)ch;
    cmd.boolVal  = limitItem ? cJSON_IsTrue(limitItem) : false;
    sendCommand(cmd);
    cJSON_Delete(doc);

    cJSON *resp = cJSON_CreateObject();
    cJSON_AddBoolToObject(resp, "ok", true);
    cJSON_AddNumberToObject(resp, "channel", ch);
    cJSON_AddBoolToObject(resp, "limit8mA", cmd.boolVal);
    return send_json(req, resp);
}

// POST /api/channel/?/avdd
static esp_err_t handle_post_avdd_select(httpd_req_t *req)
{
    int ch = extract_channel(req->uri);
    if (ch < 0) return send_error(req, 400, "Channel must be 0-3");

    cJSON *doc = recv_json_body(req);
    if (!doc) return send_error(req, 400, "Invalid JSON");

    cJSON *selItem = cJSON_GetObjectItem(doc, "select");
    if (!selItem || !cJSON_IsNumber(selItem)) {
        cJSON_Delete(doc);
        return send_error(req, 400, "Missing 'select' field");
    }

    int sel = selItem->valueint;
    if (sel < 0 || sel > 3) {
        cJSON_Delete(doc);
        return send_error(req, 400, "Select must be 0-3");
    }

    Command cmd{};
    cmd.type    = CMD_SET_AVDD_SELECT;
    cmd.channel = (uint8_t)ch;
    cmd.avddSel = (uint8_t)sel;
    sendCommand(cmd);
    cJSON_Delete(doc);

    cJSON *resp = cJSON_CreateObject();
    cJSON_AddBoolToObject(resp, "ok", true);
    cJSON_AddNumberToObject(resp, "channel", ch);
    cJSON_AddNumberToObject(resp, "select", sel);
    return send_json(req, resp);
}

// POST /api/device/reset
static esp_err_t handle_post_device_reset(httpd_req_t *req)
{
    // Consume body (may be empty)
    recv_json_body(req);

    Command cmd{};
    cmd.type = CMD_CLEAR_ALERTS;
    sendCommand(cmd);

    for (uint8_t ch = 0; ch < AD74416H_NUM_CHANNELS; ch++) {
        Command funcCmd{};
        funcCmd.type    = CMD_SET_CHANNEL_FUNC;
        funcCmd.channel = ch;
        funcCmd.func    = CH_FUNC_HIGH_IMP;
        sendCommand(funcCmd);
    }

    cJSON *resp = cJSON_CreateObject();
    cJSON_AddBoolToObject(resp, "ok", true);
    return send_json(req, resp);
}

// POST /api/faults/clear
static esp_err_t handle_post_clear_all_faults(httpd_req_t *req)
{
    recv_json_body(req);

    Command cmd{};
    cmd.type = CMD_CLEAR_ALERTS;
    sendCommand(cmd);

    cJSON *resp = cJSON_CreateObject();
    cJSON_AddBoolToObject(resp, "ok", true);
    return send_json(req, resp);
}

// POST /api/faults/clear/*
static esp_err_t handle_post_clear_channel_fault(httpd_req_t *req)
{
    int ch = extract_fault_channel(req->uri, "/api/faults/clear/");
    if (ch < 0) return send_error(req, 400, "Channel must be 0-3");

    recv_json_body(req);

    Command cmd{};
    cmd.type    = CMD_CLEAR_CHANNEL_ALERT;
    cmd.channel = (uint8_t)ch;
    sendCommand(cmd);

    cJSON *resp = cJSON_CreateObject();
    cJSON_AddBoolToObject(resp, "ok", true);
    cJSON_AddNumberToObject(resp, "channel", ch);
    return send_json(req, resp);
}

// POST /api/faults/mask
static esp_err_t handle_post_faults_mask(httpd_req_t *req)
{
    cJSON *doc = recv_json_body(req);
    if (!doc) return send_error(req, 400, "Invalid JSON");

    cJSON *alertItem  = cJSON_GetObjectItem(doc, "alertMask");
    cJSON *supplyItem = cJSON_GetObjectItem(doc, "supplyMask");

    Command alertCmd{};
    alertCmd.type    = CMD_SET_ALERT_MASK;
    alertCmd.maskVal = (uint16_t)(alertItem ? alertItem->valueint : 0);
    sendCommand(alertCmd);

    Command supplyCmd{};
    supplyCmd.type    = CMD_SET_SUPPLY_ALERT_MASK;
    supplyCmd.maskVal = (uint16_t)(supplyItem ? supplyItem->valueint : 0);
    sendCommand(supplyCmd);

    cJSON_Delete(doc);

    cJSON *resp = cJSON_CreateObject();
    cJSON_AddBoolToObject(resp, "ok", true);
    return send_json(req, resp);
}

// POST /api/faults/mask/*
static esp_err_t handle_post_channel_fault_mask(httpd_req_t *req)
{
    int ch = extract_fault_channel(req->uri, "/api/faults/mask/");
    if (ch < 0) return send_error(req, 400, "Channel must be 0-3");

    cJSON *doc = recv_json_body(req);
    if (!doc) return send_error(req, 400, "Invalid JSON");

    cJSON *maskItem = cJSON_GetObjectItem(doc, "mask");
    Command cmd{};
    cmd.type    = CMD_SET_CH_ALERT_MASK;
    cmd.channel = (uint8_t)ch;
    cmd.maskVal = (uint16_t)(maskItem ? maskItem->valueint : 0);
    sendCommand(cmd);
    cJSON_Delete(doc);

    cJSON *resp = cJSON_CreateObject();
    cJSON_AddBoolToObject(resp, "ok", true);
    cJSON_AddNumberToObject(resp, "channel", ch);
    return send_json(req, resp);
}

// POST /api/diagnostics/config
static esp_err_t handle_post_diag_config(httpd_req_t *req)
{
    cJSON *doc = recv_json_body(req);
    if (!doc) return send_error(req, 400, "Invalid JSON");

    cJSON *slotItem   = cJSON_GetObjectItem(doc, "slot");
    cJSON *sourceItem = cJSON_GetObjectItem(doc, "source");

    if (!slotItem || !cJSON_IsNumber(slotItem) ||
        !sourceItem || !cJSON_IsNumber(sourceItem)) {
        cJSON_Delete(doc);
        return send_error(req, 400, "Missing 'slot' or 'source' field");
    }

    int slot   = slotItem->valueint;
    int source = sourceItem->valueint;

    if (slot < 0 || slot > 3) {
        cJSON_Delete(doc);
        return send_error(req, 400, "Slot must be 0-3");
    }
    if (source < 0 || source > 13) {
        cJSON_Delete(doc);
        return send_error(req, 400, "Source must be 0-13");
    }

    Command cmd{};
    cmd.type             = CMD_DIAG_CONFIG;
    cmd.diagCfg.slot     = (uint8_t)slot;
    cmd.diagCfg.source   = (uint8_t)source;
    sendCommand(cmd);
    cJSON_Delete(doc);

    cJSON *resp = cJSON_CreateObject();
    cJSON_AddBoolToObject(resp, "ok", true);
    cJSON_AddNumberToObject(resp, "slot", slot);
    cJSON_AddNumberToObject(resp, "source", source);
    return send_json(req, resp);
}

// POST /api/gpio/?/config
static esp_err_t handle_post_gpio_config(httpd_req_t *req)
{
    int g = extract_gpio(req->uri);
    if (g < 0) return send_error(req, 400, "GPIO must be 0-5");

    cJSON *doc = recv_json_body(req);
    if (!doc) return send_error(req, 400, "Invalid JSON");

    cJSON *modeItem = cJSON_GetObjectItem(doc, "mode");
    if (!modeItem || !cJSON_IsNumber(modeItem)) {
        cJSON_Delete(doc);
        return send_error(req, 400, "Missing 'mode' field");
    }

    cJSON *pulldownItem = cJSON_GetObjectItem(doc, "pulldown");

    Command cmd{};
    cmd.type             = CMD_GPIO_CONFIG;
    cmd.gpioCfg.gpio     = (uint8_t)g;
    cmd.gpioCfg.mode     = (uint8_t)modeItem->valueint;
    cmd.gpioCfg.pulldown = pulldownItem ? cJSON_IsTrue(pulldownItem) : false;
    sendCommand(cmd);
    cJSON_Delete(doc);

    cJSON *resp = cJSON_CreateObject();
    cJSON_AddBoolToObject(resp, "ok", true);
    cJSON_AddNumberToObject(resp, "gpio", g);
    cJSON_AddNumberToObject(resp, "mode", (int)cmd.gpioCfg.mode);
    cJSON_AddBoolToObject(resp, "pulldown", cmd.gpioCfg.pulldown);
    return send_json(req, resp);
}

// POST /api/gpio/?/set
static esp_err_t handle_post_gpio_set(httpd_req_t *req)
{
    int g = extract_gpio(req->uri);
    if (g < 0) return send_error(req, 400, "GPIO must be 0-5");

    cJSON *doc = recv_json_body(req);
    if (!doc) return send_error(req, 400, "Invalid JSON");

    cJSON *valueItem = cJSON_GetObjectItem(doc, "value");
    Command cmd{};
    cmd.type          = CMD_GPIO_SET;
    cmd.gpioSet.gpio  = (uint8_t)g;
    cmd.gpioSet.value = valueItem ? cJSON_IsTrue(valueItem) : false;
    sendCommand(cmd);
    cJSON_Delete(doc);

    cJSON *resp = cJSON_CreateObject();
    cJSON_AddBoolToObject(resp, "ok", true);
    cJSON_AddNumberToObject(resp, "gpio", g);
    cJSON_AddBoolToObject(resp, "value", cmd.gpioSet.value);
    return send_json(req, resp);
}

// =============================================================================
// Dispatchers for parameterized routes (IDF wildcard only works at end of URI)
// =============================================================================

// Helper: check if URI has a given suffix after /api/channel/X
static const char* channel_suffix(const char* uri)
{
    // URI format: /api/channel/X/suffix  where X is a digit
    const char *p = strstr(uri, "/api/channel/");
    if (!p) return NULL;
    p += 14; // skip past "/api/channel/X"
    if (*p == '/') return p + 1; // return suffix after the slash
    if (*p == '\0') return "";
    return NULL;
}

// GET /api/channel/* dispatcher
static esp_err_t handle_channel_get_dispatch(httpd_req_t *req)
{
    const char *suffix = channel_suffix(req->uri);
    if (!suffix) return send_error(req, 400, "Invalid channel URL");

    if (strcmp(suffix, "adc") == 0)          return handle_get_channel_adc(req);
    if (strcmp(suffix, "dac/readback") == 0)  return handle_get_dac_readback(req);

    return send_error(req, 404, "Unknown channel GET endpoint");
}

// POST /api/channel/* dispatcher
static esp_err_t handle_channel_post_dispatch(httpd_req_t *req)
{
    const char *suffix = channel_suffix(req->uri);
    if (!suffix) return send_error(req, 400, "Invalid channel URL");

    if (strcmp(suffix, "function") == 0)      return handle_post_channel_function(req);
    if (strcmp(suffix, "dac") == 0)           return handle_post_dac(req);
    if (strcmp(suffix, "adc/config") == 0)    return handle_post_adc_config(req);
    if (strcmp(suffix, "din/config") == 0)    return handle_post_din_config(req);
    if (strcmp(suffix, "do/config") == 0)     return handle_post_do_config(req);
    if (strcmp(suffix, "do/set") == 0)        return handle_post_do_set(req);
    if (strcmp(suffix, "vout/range") == 0)    return handle_post_vout_range(req);
    if (strcmp(suffix, "ilimit") == 0)        return handle_post_current_limit(req);
    if (strcmp(suffix, "avdd") == 0)          return handle_post_avdd_select(req);

    return send_error(req, 404, "Unknown channel POST endpoint");
}

// Helper: check suffix after /api/gpio/X
static const char* gpio_suffix(const char* uri)
{
    const char *p = strstr(uri, "/api/gpio/");
    if (!p) return NULL;
    p += 11; // skip past "/api/gpio/X"
    if (*p == '/') return p + 1;
    if (*p == '\0') return "";
    return NULL;
}

// POST /api/gpio/* dispatcher
static esp_err_t handle_gpio_post_dispatch(httpd_req_t *req)
{
    const char *suffix = gpio_suffix(req->uri);
    if (!suffix) return send_error(req, 400, "Invalid GPIO URL");

    if (strcmp(suffix, "config") == 0)  return handle_post_gpio_config(req);
    if (strcmp(suffix, "set") == 0)     return handle_post_gpio_set(req);

    return send_error(req, 404, "Unknown GPIO POST endpoint");
}

// =============================================================================
// UART Bridge endpoints
// =============================================================================

// GET /api/uart/config
static esp_err_t handle_get_uart_config(httpd_req_t *req)
{
    cJSON *root = cJSON_CreateObject();
    cJSON *bridges = cJSON_AddArrayToObject(root, "bridges");

    for (int id = 0; id < CDC_BRIDGE_COUNT; id++) {
        UartBridgeConfig cfg;
        if (uart_bridge_get_config(id, &cfg)) {
            cJSON *obj = cJSON_CreateObject();
            cJSON_AddNumberToObject(obj, "id", id);
            cJSON_AddNumberToObject(obj, "uartNum", cfg.uart_num);
            cJSON_AddNumberToObject(obj, "txPin", cfg.tx_pin);
            cJSON_AddNumberToObject(obj, "rxPin", cfg.rx_pin);
            cJSON_AddNumberToObject(obj, "baudrate", cfg.baudrate);
            cJSON_AddNumberToObject(obj, "dataBits", cfg.data_bits);
            cJSON_AddNumberToObject(obj, "parity", cfg.parity);
            cJSON_AddNumberToObject(obj, "stopBits", cfg.stop_bits);
            cJSON_AddBoolToObject(obj, "enabled", cfg.enabled);
            cJSON_AddBoolToObject(obj, "connected", uart_bridge_is_connected(id));
            cJSON_AddItemToArray(bridges, obj);
        }
    }

    return send_json(req, root);
}

// GET /api/uart/pins
static esp_err_t handle_get_uart_pins(httpd_req_t *req)
{
    cJSON *root = cJSON_CreateObject();
    int pins[48];
    int count = uart_bridge_get_available_pins(pins, 48);
    cJSON *arr = cJSON_AddArrayToObject(root, "available");
    for (int i = 0; i < count; i++) {
        cJSON_AddItemToArray(arr, cJSON_CreateNumber(pins[i]));
    }
    return send_json(req, root);
}

// POST /api/uart/*/config
static esp_err_t handle_post_uart_config(httpd_req_t *req)
{
    // Extract bridge ID from URI: /api/uart/X/config
    const char *p = strstr(req->uri, "/api/uart/");
    if (!p) return send_error(req, 400, "Invalid URI");
    p += 10; // skip "/api/uart/"
    int id = (*p >= '0' && *p <= '9') ? (*p - '0') : -1;
    if (id < 0 || id >= CDC_BRIDGE_COUNT) return send_error(req, 400, "Invalid bridge ID");

    cJSON *doc = recv_json_body(req);
    if (!doc) return send_error(req, 400, "Invalid JSON");

    UartBridgeConfig cfg;
    uart_bridge_get_config(id, &cfg);

    cJSON *item;
    if ((item = cJSON_GetObjectItem(doc, "uartNum")) && cJSON_IsNumber(item))
        cfg.uart_num = (uint8_t)item->valueint;
    if ((item = cJSON_GetObjectItem(doc, "txPin")) && cJSON_IsNumber(item))
        cfg.tx_pin = item->valueint;
    if ((item = cJSON_GetObjectItem(doc, "rxPin")) && cJSON_IsNumber(item))
        cfg.rx_pin = item->valueint;
    if ((item = cJSON_GetObjectItem(doc, "baudrate")) && cJSON_IsNumber(item))
        cfg.baudrate = (uint32_t)item->valueint;
    if ((item = cJSON_GetObjectItem(doc, "dataBits")) && cJSON_IsNumber(item))
        cfg.data_bits = (uint8_t)item->valueint;
    if ((item = cJSON_GetObjectItem(doc, "parity")) && cJSON_IsNumber(item))
        cfg.parity = (uint8_t)item->valueint;
    if ((item = cJSON_GetObjectItem(doc, "stopBits")) && cJSON_IsNumber(item))
        cfg.stop_bits = (uint8_t)item->valueint;
    if ((item = cJSON_GetObjectItem(doc, "enabled")))
        cfg.enabled = cJSON_IsTrue(item);

    cJSON_Delete(doc);

    // Validate
    if (cfg.uart_num > 2) return send_error(req, 400, "uartNum must be 0-2");
    if (cfg.baudrate < 300 || cfg.baudrate > 3000000) return send_error(req, 400, "baudrate out of range");

    uart_bridge_set_config(id, &cfg);

    cJSON *resp = cJSON_CreateObject();
    cJSON_AddBoolToObject(resp, "ok", true);
    cJSON_AddNumberToObject(resp, "id", id);
    return send_json(req, resp);
}

// POST /api/uart/* dispatcher
static esp_err_t handle_uart_post_dispatch(httpd_req_t *req)
{
    const char *p = strstr(req->uri, "/api/uart/");
    if (!p) return send_error(req, 400, "Invalid URI");
    p += 10;
    // Skip the digit
    if (*p >= '0' && *p <= '9') p++;
    if (*p == '/') p++;

    if (strcmp(p, "config") == 0) return handle_post_uart_config(req);
    return send_error(req, 404, "Unknown UART POST endpoint");
}

// OPTIONS /api/* - CORS preflight
static esp_err_t handle_options(httpd_req_t *req)
{
    set_cors_headers(req);
    httpd_resp_set_status(req, "204");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

// =============================================================================
// Server init / stop
// =============================================================================

void initWebServer(void)
{
    if (s_server) {
        ESP_LOGW(TAG, "Web server already running");
        return;
    }

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers = 35;
    config.uri_match_fn     = httpd_uri_match_wildcard;
    config.stack_size       = 8192;

    esp_err_t ret = httpd_start(&s_server, &config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start HTTP server: %s", esp_err_to_name(ret));
        return;
    }

    ESP_LOGI(TAG, "HTTP server started on port %d", config.server_port);

    // ----- GET routes -----

    httpd_uri_t uri_root = {
        .uri = "/", .method = HTTP_GET, .handler = handle_root, .user_ctx = NULL
    };
    httpd_register_uri_handler(s_server, &uri_root);

    httpd_uri_t uri_status = {
        .uri = "/api/status", .method = HTTP_GET, .handler = handle_get_status, .user_ctx = NULL
    };
    httpd_register_uri_handler(s_server, &uri_status);

    httpd_uri_t uri_faults = {
        .uri = "/api/faults", .method = HTTP_GET, .handler = handle_get_faults, .user_ctx = NULL
    };
    httpd_register_uri_handler(s_server, &uri_faults);

    httpd_uri_t uri_ch_get = {
        .uri = "/api/channel/*", .method = HTTP_GET, .handler = handle_channel_get_dispatch, .user_ctx = NULL
    };
    httpd_register_uri_handler(s_server, &uri_ch_get);

    httpd_uri_t uri_diagnostics = {
        .uri = "/api/diagnostics", .method = HTTP_GET, .handler = handle_get_diagnostics, .user_ctx = NULL
    };
    httpd_register_uri_handler(s_server, &uri_diagnostics);

    httpd_uri_t uri_device_info = {
        .uri = "/api/device/info", .method = HTTP_GET, .handler = handle_get_device_info, .user_ctx = NULL
    };
    httpd_register_uri_handler(s_server, &uri_device_info);

    httpd_uri_t uri_scope = {
        .uri = "/api/scope", .method = HTTP_GET, .handler = handle_get_scope, .user_ctx = NULL
    };
    httpd_register_uri_handler(s_server, &uri_scope);

    httpd_uri_t uri_gpio = {
        .uri = "/api/gpio", .method = HTTP_GET, .handler = handle_get_gpio, .user_ctx = NULL
    };
    httpd_register_uri_handler(s_server, &uri_gpio);

    // ----- POST routes -----

    httpd_uri_t uri_ch_post = {
        .uri = "/api/channel/*", .method = HTTP_POST, .handler = handle_channel_post_dispatch, .user_ctx = NULL
    };
    httpd_register_uri_handler(s_server, &uri_ch_post);

    httpd_uri_t uri_post_faults_clear = {
        .uri = "/api/faults/clear", .method = HTTP_POST, .handler = handle_post_clear_all_faults, .user_ctx = NULL
    };
    httpd_register_uri_handler(s_server, &uri_post_faults_clear);

    httpd_uri_t uri_post_faults_clear_ch = {
        .uri = "/api/faults/clear/*", .method = HTTP_POST, .handler = handle_post_clear_channel_fault, .user_ctx = NULL
    };
    httpd_register_uri_handler(s_server, &uri_post_faults_clear_ch);

    httpd_uri_t uri_post_faults_mask = {
        .uri = "/api/faults/mask", .method = HTTP_POST, .handler = handle_post_faults_mask, .user_ctx = NULL
    };
    httpd_register_uri_handler(s_server, &uri_post_faults_mask);

    httpd_uri_t uri_post_faults_mask_ch = {
        .uri = "/api/faults/mask/*", .method = HTTP_POST, .handler = handle_post_channel_fault_mask, .user_ctx = NULL
    };
    httpd_register_uri_handler(s_server, &uri_post_faults_mask_ch);

    httpd_uri_t uri_post_diag_cfg = {
        .uri = "/api/diagnostics/config", .method = HTTP_POST, .handler = handle_post_diag_config, .user_ctx = NULL
    };
    httpd_register_uri_handler(s_server, &uri_post_diag_cfg);

    httpd_uri_t uri_post_device_reset = {
        .uri = "/api/device/reset", .method = HTTP_POST, .handler = handle_post_device_reset, .user_ctx = NULL
    };
    httpd_register_uri_handler(s_server, &uri_post_device_reset);

    httpd_uri_t uri_gpio_post = {
        .uri = "/api/gpio/*", .method = HTTP_POST, .handler = handle_gpio_post_dispatch, .user_ctx = NULL
    };
    httpd_register_uri_handler(s_server, &uri_gpio_post);

    // ----- UART bridge routes -----

    httpd_uri_t uri_uart_config = {
        .uri = "/api/uart/config", .method = HTTP_GET, .handler = handle_get_uart_config, .user_ctx = NULL
    };
    httpd_register_uri_handler(s_server, &uri_uart_config);

    httpd_uri_t uri_uart_pins = {
        .uri = "/api/uart/pins", .method = HTTP_GET, .handler = handle_get_uart_pins, .user_ctx = NULL
    };
    httpd_register_uri_handler(s_server, &uri_uart_pins);

    httpd_uri_t uri_uart_post = {
        .uri = "/api/uart/*", .method = HTTP_POST, .handler = handle_uart_post_dispatch, .user_ctx = NULL
    };
    httpd_register_uri_handler(s_server, &uri_uart_post);

    // ----- OPTIONS (CORS preflight) -----

    httpd_uri_t uri_options = {
        .uri = "/api/*", .method = HTTP_OPTIONS, .handler = handle_options, .user_ctx = NULL
    };
    httpd_register_uri_handler(s_server, &uri_options);

    ESP_LOGI(TAG, "All URI handlers registered");
}

void stopWebServer(void)
{
    if (s_server) {
        httpd_stop(s_server);
        s_server = NULL;
        ESP_LOGI(TAG, "HTTP server stopped");
    }
}
