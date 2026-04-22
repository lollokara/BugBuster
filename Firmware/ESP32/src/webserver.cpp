// =============================================================================
// webserver.cpp - HTTP API server for AD74416H controller (ESP-IDF httpd)
// =============================================================================

#include "webserver.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "esp_http_server.h"
#include "esp_mac.h"
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
#include "i2c_bus.h"
#include "ds4424.h"
#include "husb238.h"
#include "pca9535.h"
#include "hat.h"
#include "adgs2414d.h"
#include "dio.h"
#include "selftest.h"
#include "bbp.h"
#include "auth.h"
#include "board_profile.h"
#include "wifi_manager.h"
#include "esp_wifi.h"
#include "esp_ota_ops.h"
#include "esp_app_format.h"
#include "bbp.h"

extern AD74416H_SPI spiDriver;

static const char* TAG = "webserver";

static httpd_handle_t s_server = NULL;

static const char* http_status_string(int code)
{
    switch (code) {
        case 200: return "200 OK";
        case 400: return "400 Bad Request";
        case 404: return "404 Not Found";
        case 405: return "405 Method Not Allowed";
        case 500: return "500 Internal Server Error";
        case 503: return "503 Service Unavailable";
        default:  return NULL;
    }
}

static bool is_valid_channel_function(int func)
{
    switch (func) {
        case CH_FUNC_HIGH_IMP:
        case CH_FUNC_VOUT:
        case CH_FUNC_IOUT:
        case CH_FUNC_VIN:
        case CH_FUNC_IIN_EXT_PWR:
        case CH_FUNC_IIN_LOOP_PWR:
        case CH_FUNC_RES_MEAS:
        case CH_FUNC_DIN_LOGIC:
        case CH_FUNC_DIN_LOOP:
        case CH_FUNC_IOUT_HART:
        case CH_FUNC_IIN_EXT_PWR_HART:
        case CH_FUNC_IIN_LOOP_PWR_HART:
            return true;
        default:
            return false;
    }
}

static void add_number_alias(cJSON *obj, const char *camel, const char *snake, double value)
{
    cJSON_AddNumberToObject(obj, camel, value);
    cJSON_AddNumberToObject(obj, snake, value);
}

static void add_bool_alias(cJSON *obj, const char *camel, const char *snake, bool value)
{
    cJSON_AddBoolToObject(obj, camel, value);
    cJSON_AddBoolToObject(obj, snake, value);
}

// -----------------------------------------------------------------------------
// Helper: CORS headers
// -----------------------------------------------------------------------------

static void set_cors_headers(httpd_req_t *req)
{
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Methods", "GET, POST, OPTIONS");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Headers", "Content-Type, " ADMIN_TOKEN_HEADER);
}

// -----------------------------------------------------------------------------
// Helper: Check Admin Authentication
// -----------------------------------------------------------------------------

static esp_err_t check_admin_auth(httpd_req_t *req)
{
    char token[65] = {0};
    esp_err_t err = httpd_req_get_hdr_value_str(req, ADMIN_TOKEN_HEADER, token, sizeof(token));
    
    if (err == ESP_OK && auth_verify_token(token)) {
        return ESP_OK;
    }
    
    ESP_LOGW(TAG, "Unauthorized access attempt to %s", req->uri);
    return ESP_FAIL;
}

// -----------------------------------------------------------------------------
// Helper: send a cJSON object as an HTTP response (deletes root after send)
// -----------------------------------------------------------------------------

static esp_err_t send_json(httpd_req_t *req, cJSON *root, int code = 200)
{
    char *body = cJSON_PrintUnformatted(root);
    if (!body) {
        cJSON_Delete(root);
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "JSON encode failed");
    }
    set_cors_headers(req);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");

    if (code != 200) {
        const char *status = http_status_string(code);
        if (status) {
            httpd_resp_set_status(req, status);
        }
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

static esp_err_t handle_http_error(httpd_req_t *req, httpd_err_code_t error)
{
    if (error == HTTPD_404_NOT_FOUND) {
        set_cors_headers(req);
        return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Not found");
    }
    if (error == HTTPD_405_METHOD_NOT_ALLOWED) {
        set_cors_headers(req);
        return httpd_resp_send_err(req, HTTPD_405_METHOD_NOT_ALLOWED, "Method not allowed");
    }

    return httpd_resp_send_err(req, error, NULL);
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
// Helper: strict JSON field validation
// -----------------------------------------------------------------------------

#define VALIDATE_JSON_FIELD(obj, key, type, error_msg) \
    do { \
        cJSON *item = cJSON_GetObjectItem(obj, key); \
        if (!item || !cJSON_Is##type(item)) { \
            cJSON_Delete(obj); \
            return send_error(req, 400, error_msg); \
        } \
    } while(0)

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
    httpd_resp_set_hdr(req, "Cache-Control", "no-store, no-cache, must-revalidate");
    char buf[512];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
        httpd_resp_send_chunk(req, buf, n);
    }
    httpd_resp_send_chunk(req, NULL, 0);  // end chunked response
    fclose(f);
    return ESP_OK;
}

// Map filename extension → MIME type for static asset serving.
static const char *mime_from_path(const char *path)
{
    const char *dot = strrchr(path, '.');
    if (!dot) return "application/octet-stream";
    if (strcmp(dot, ".html") == 0) return "text/html; charset=utf-8";
    if (strcmp(dot, ".js")   == 0) return "application/javascript; charset=utf-8";
    if (strcmp(dot, ".mjs")  == 0) return "application/javascript; charset=utf-8";
    if (strcmp(dot, ".css")  == 0) return "text/css; charset=utf-8";
    if (strcmp(dot, ".json") == 0) return "application/json; charset=utf-8";
    if (strcmp(dot, ".svg")  == 0) return "image/svg+xml";
    if (strcmp(dot, ".ico")  == 0) return "image/x-icon";
    if (strcmp(dot, ".png")  == 0) return "image/png";
    if (strcmp(dot, ".jpg")  == 0 || strcmp(dot, ".jpeg") == 0) return "image/jpeg";
    if (strcmp(dot, ".woff") == 0) return "font/woff";
    if (strcmp(dot, ".woff2") == 0) return "font/woff2";
    if (strcmp(dot, ".ttf")  == 0) return "font/ttf";
    if (strcmp(dot, ".map")  == 0) return "application/json; charset=utf-8";
    if (strcmp(dot, ".wasm") == 0) return "application/wasm";
    if (strcmp(dot, ".txt")  == 0) return "text/plain; charset=utf-8";
    return "application/octet-stream";
}

// GET /assets/* — serve Vite-built static bundle from SPIFFS/LittleFS.
//
// The browser loads hashed bundles (app-<hash>.js, index-<hash>.css,
// fonts/*.woff2) emitted by `pnpm build` into Firmware/ESP32/data/assets/.
// Path traversal via ".." is rejected. Hashed filenames get long-lived
// Cache-Control; index.html (served by handle_root) stays no-cache so
// fresh bundles are picked up immediately after an `uploadfs`.
static esp_err_t handle_static_asset(httpd_req_t *req)
{
    const char *uri = req->uri;

    // Strip query string, if any.
    const char *q = strchr(uri, '?');
    size_t uri_len = q ? (size_t)(q - uri) : strlen(uri);

    // Must start with "/assets/".
    const char *prefix = "/assets/";
    size_t prefix_len = strlen(prefix);
    if (uri_len <= prefix_len || strncmp(uri, prefix, prefix_len) != 0) {
        set_cors_headers(req);
        return httpd_resp_send_404(req);
    }

    // Reject path traversal.
    if (strstr(uri, "..") != NULL) {
        set_cors_headers(req);
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Bad path");
    }

    // Build /spiffs<uri> path (bounded).
    char fs_path[160];
    if (uri_len + sizeof("/spiffs") > sizeof(fs_path)) {
        set_cors_headers(req);
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Path too long");
    }
    int written = snprintf(fs_path, sizeof(fs_path), "/spiffs%.*s",
                           (int)uri_len, uri);
    if (written <= 0 || (size_t)written >= sizeof(fs_path)) {
        set_cors_headers(req);
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Path too long");
    }

    FILE *f = fopen(fs_path, "rb");
    if (!f) {
        ESP_LOGW(TAG, "Asset not found: %s", fs_path);
        set_cors_headers(req);
        return httpd_resp_send_404(req);
    }

    set_cors_headers(req);
    httpd_resp_set_type(req, mime_from_path(fs_path));
    // Hashed filenames are cache-safe; tune to taste.
    httpd_resp_set_hdr(req, "Cache-Control", "public, max-age=31536000, immutable");

    char buf[1024];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
        if (httpd_resp_send_chunk(req, buf, n) != ESP_OK) {
            fclose(f);
            return ESP_FAIL;
        }
    }
    httpd_resp_send_chunk(req, NULL, 0);
    fclose(f);
    return ESP_OK;
}

// GET /api/status
static esp_err_t handle_get_status(httpd_req_t *req)
{
    cJSON *root = cJSON_CreateObject();

    if (xSemaphoreTake(g_stateMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        add_bool_alias(root, "spiOk", "spi_ok", g_deviceState.spiOk);
        cJSON_AddNumberToObject(root, "dieTemp", g_deviceState.dieTemperature);
        cJSON_AddNumberToObject(root, "die_temp_c", g_deviceState.dieTemperature);
        add_number_alias(root, "alertStatus", "alert_status", g_deviceState.alertStatus);
        add_number_alias(root, "alertMask", "alert_mask", g_deviceState.alertMask);
        add_number_alias(root, "supplyAlertStatus", "supply_alert_status", g_deviceState.supplyAlertStatus);
        add_number_alias(root, "supplyAlertMask", "supply_alert_mask", g_deviceState.supplyAlertMask);
        add_number_alias(root, "liveStatus", "live_status", g_deviceState.liveStatus);

        cJSON *channels = cJSON_AddArrayToObject(root, "channels");
        for (uint8_t ch = 0; ch < AD74416H_NUM_CHANNELS; ch++) {
            const ChannelState& cs = g_deviceState.channels[ch];
            cJSON *obj = cJSON_CreateObject();
            cJSON_AddNumberToObject(obj, "id", ch);
            cJSON_AddStringToObject(obj, "function", channelFunctionToString(cs.function));
            add_number_alias(obj, "functionCode", "function_code", (int)cs.function);
            add_number_alias(obj, "adcRaw", "adc_raw", cs.adcRawCode);
            add_number_alias(obj, "adcValue", "adc_value", cs.adcValue);
            add_number_alias(obj, "adcRange", "adc_range", (int)cs.adcRange);
            add_number_alias(obj, "adcRate", "adc_rate", (int)cs.adcRate);
            add_number_alias(obj, "adcMux", "adc_mux", (int)cs.adcMux);
            add_number_alias(obj, "dacCode", "dac_code", cs.dacCode);
            add_number_alias(obj, "dacValue", "dac_value", cs.dacValue);
            add_bool_alias(obj, "dinState", "din_state", cs.dinState);
            add_number_alias(obj, "dinCounter", "din_counter", cs.dinCounter);
            add_bool_alias(obj, "doState", "do_state", cs.doState);
            add_number_alias(obj, "channelAlert", "channel_alert", cs.channelAlertStatus);
            add_number_alias(obj, "channelAlertMask", "channel_alert_mask", cs.channelAlertMask);
            add_number_alias(obj, "rtdExcitationUa", "rtd_excitation_ua", cs.rtdExcitationUa);
            cJSON_AddItemToArray(channels, obj);
        }

        // Diagnostic slots (sync with BBP GET_STATUS)
        cJSON *diagnostics = cJSON_AddArrayToObject(root, "diagnostics");
        for (uint8_t d = 0; d < 4; d++) {
            cJSON *dobj = cJSON_CreateObject();
            cJSON_AddNumberToObject(dobj, "source", g_deviceState.diag[d].source);
            cJSON_AddNumberToObject(dobj, "rawCode", g_deviceState.diag[d].rawCode);
            cJSON_AddNumberToObject(dobj, "value", g_deviceState.diag[d].value);
            cJSON_AddItemToArray(diagnostics, dobj);
        }

        // MUX switch states (sync with BBP GET_STATUS)
        cJSON *muxStates = cJSON_AddArrayToObject(root, "muxStates");
        uint8_t muxApiStates[ADGS_API_MAIN_DEVICES] = {};
        adgs_get_api_states(muxApiStates);
        for (uint8_t m = 0; m < ADGS_API_MAIN_DEVICES; m++) {
            cJSON_AddItemToArray(muxStates, cJSON_CreateNumber(muxApiStates[m]));
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
        cJSON_AddNumberToObject(root, "raw_code", cs.adcRawCode);
        cJSON_AddNumberToObject(root, "value", cs.adcValue);
        cJSON_AddNumberToObject(root, "range", (int)cs.adcRange);
        cJSON_AddNumberToObject(root, "rate", (int)cs.adcRate);
        cJSON_AddNumberToObject(root, "mux", (int)cs.adcMux);
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
        add_number_alias(root, "alertStatus", "alert_status", g_deviceState.alertStatus);
        add_number_alias(root, "alertMask", "alert_mask", g_deviceState.alertMask);
        add_number_alias(root, "supplyAlertStatus", "supply_alert_status", g_deviceState.supplyAlertStatus);
        add_number_alias(root, "supplyAlertMask", "supply_alert_mask", g_deviceState.supplyAlertMask);

        cJSON *channels = cJSON_AddArrayToObject(root, "channels");
        for (uint8_t ch = 0; ch < AD74416H_NUM_CHANNELS; ch++) {
            cJSON *obj = cJSON_CreateObject();
            cJSON_AddNumberToObject(obj, "id", ch);
            cJSON_AddNumberToObject(obj, "channelAlert", g_deviceState.channels[ch].channelAlertStatus);
            cJSON_AddNumberToObject(obj, "channelAlertMask", g_deviceState.channels[ch].channelAlertMask);
            cJSON_AddNumberToObject(obj, "alert", g_deviceState.channels[ch].channelAlertStatus);
            cJSON_AddNumberToObject(obj, "mask", g_deviceState.channels[ch].channelAlertMask);
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
    cJSON *samples = cJSON_AddArrayToObject(root, "samples");
    cJSON_AddItemReferenceToObject(root, "s", samples);

    if (xSemaphoreTake(g_stateMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        const ScopeBuffer& sb = g_deviceState.scope;
        uint16_t cur_seq = sb.seq;
        uint16_t head    = sb.head;

        uint16_t avail = (uint16_t)(cur_seq - since_seq);
        if (avail > SCOPE_BUF_SIZE) avail = SCOPE_BUF_SIZE;
        if (avail > 32) avail = 32;

        cJSON_AddNumberToObject(root, "seq", cur_seq);

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
    } else {
        cJSON_AddNumberToObject(root, "seq", 0);
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

    // Expose the primary station MAC so HTTP clients can key their pairing
    // store on the same identifier the USB handshake provides (BBP v4 §3.1).
    // Without this field the desktop app keyed pairing on "00:00:00:00:00:00"
    // and demanded USB re-authorisation on every HTTP connect.
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    char macStr[18];
    snprintf(macStr, sizeof(macStr), "%02x:%02x:%02x:%02x:%02x:%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "siliconRev", (int)rev);
    cJSON_AddNumberToObject(root, "silicon_rev", (int)rev);
    cJSON_AddStringToObject(root, "siliconId0", id0Str);
    cJSON_AddStringToObject(root, "siliconId1", id1Str);
    cJSON_AddNumberToObject(root, "silicon_id0", (int)id0);
    cJSON_AddNumberToObject(root, "silicon_id1", (int)id1);
    cJSON_AddStringToObject(root, "macAddress", macStr);
    cJSON_AddStringToObject(root, "mac_address", macStr);
    add_bool_alias(root, "spiOk", "spi_ok", spiOk);

    return send_json(req, root);
}

// GET /api/gpio
static esp_err_t handle_get_gpio(httpd_req_t *req)
{
    cJSON *root = cJSON_CreateArray();

    if (xSemaphoreTake(g_stateMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        for (uint8_t g = 0; g < AD74416H_NUM_GPIOS; g++) {
            const GpioState& gs = g_deviceState.gpio[g];
            cJSON *obj = cJSON_CreateObject();
            cJSON_AddNumberToObject(obj, "id", g);
            cJSON_AddNumberToObject(obj, "pin", g);
            char name[2] = { (char)('A' + g), '\0' };
            cJSON_AddStringToObject(obj, "name", name);
            cJSON_AddNumberToObject(obj, "mode", gs.mode);
            cJSON_AddStringToObject(obj, "modeName", gpioModeName(gs.mode));
            cJSON_AddBoolToObject(obj, "output", gs.outputVal);
            cJSON_AddBoolToObject(obj, "input", gs.inputVal);
            cJSON_AddBoolToObject(obj, "pulldown", gs.pulldown);
            cJSON_AddItemToArray(root, obj);
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
    cJSON_AddNumberToObject(root, "code", (int)activeCode);
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

    VALIDATE_JSON_FIELD(doc, "function", Number, "Missing field 'function'");

    int func = cJSON_GetObjectItem(doc, "function")->valueint;
    if (!is_valid_channel_function(func)) {
        cJSON_Delete(doc);
        return send_error(req, 400, "Invalid function");
    }

    tasks_apply_channel_function((uint8_t)ch, (ChannelFunction)func);
    cJSON_Delete(doc);

    cJSON *resp = cJSON_CreateObject();
    cJSON_AddBoolToObject(resp, "ok", true);
    cJSON_AddNumberToObject(resp, "channel", ch);
    cJSON_AddNumberToObject(resp, "function", func);
    return send_json(req, resp);
}

// POST /api/channel/?/dac
static esp_err_t handle_post_dac(httpd_req_t *req)
{
    int ch = extract_channel(req->uri);
    if (ch < 0) return send_error(req, 400, "Channel must be 0-3");

    cJSON *doc = recv_json_body(req);
    if (!doc) return send_error(req, 400, "Invalid JSON");

    cJSON *codeItem      = cJSON_GetObjectItem(doc, "code");
    cJSON *voltageItem   = cJSON_GetObjectItem(doc, "voltage");
    cJSON *currentItem   = cJSON_GetObjectItem(doc, "current_mA");

    if (codeItem && cJSON_IsNumber(codeItem)) {
        if (!tasks_apply_dac_code((uint8_t)ch, (uint16_t)codeItem->valueint)) {
            cJSON_Delete(doc);
            return send_error(req, 400, "Failed to set DAC code");
        }
    } else if (voltageItem && cJSON_IsNumber(voltageItem)) {
        cJSON *bipolarItem = cJSON_GetObjectItem(doc, "bipolar");
        bool bipolar = bipolarItem ? cJSON_IsTrue(bipolarItem) : false;
        if (!tasks_apply_dac_voltage((uint8_t)ch, (float)voltageItem->valuedouble, bipolar)) {
            cJSON_Delete(doc);
            return send_error(req, 400, "Failed to set DAC voltage");
        }
    } else if (currentItem && cJSON_IsNumber(currentItem)) {
        if (!tasks_apply_dac_current((uint8_t)ch, (float)currentItem->valuedouble)) {
            cJSON_Delete(doc);
            return send_error(req, 400, "Failed to set DAC current");
        }
    } else {
        cJSON_Delete(doc);
        return send_error(req, 400, "Missing field 'code', 'voltage' or 'current_mA' as number");
    }

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

    VALIDATE_JSON_FIELD(doc, "mux", Number, "Missing field 'mux'");
    VALIDATE_JSON_FIELD(doc, "range", Number, "Missing field 'range'");
    VALIDATE_JSON_FIELD(doc, "rate", Number, "Missing field 'rate'");

    Command cmd{};
    cmd.type           = CMD_ADC_CONFIG;
    cmd.channel        = (uint8_t)ch;
    cmd.adcCfg.mux     = (AdcConvMux)cJSON_GetObjectItem(doc, "mux")->valueint;
    cmd.adcCfg.range   = (AdcRange)cJSON_GetObjectItem(doc, "range")->valueint;
    cmd.adcCfg.rate    = (AdcRate)cJSON_GetObjectItem(doc, "rate")->valueint;
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
    bool bipolar = bipolarItem ? cJSON_IsTrue(bipolarItem) : false;
    if (!tasks_apply_vout_range((uint8_t)ch, bipolar)) {
        cJSON_Delete(doc);
        return send_error(req, 400, "Failed to set VOUT range");
    }
    cJSON_Delete(doc);

    cJSON *resp = cJSON_CreateObject();
    cJSON_AddBoolToObject(resp, "ok", true);
    cJSON_AddNumberToObject(resp, "channel", ch);
    cJSON_AddBoolToObject(resp, "bipolar", bipolar);
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
    if (check_admin_auth(req) != ESP_OK) return send_error(req, 401, "Admin token required");

    // Consume body (may be empty)
    cJSON *body = recv_json_body(req);
    if (body) cJSON_Delete(body);

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
    if (check_admin_auth(req) != ESP_OK) return send_error(req, 401, "Admin token required");

    cJSON *body = recv_json_body(req);
    if (body) cJSON_Delete(body);

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
    if (check_admin_auth(req) != ESP_OK) return send_error(req, 401, "Admin token required");

    int ch = extract_fault_channel(req->uri, "/api/faults/clear/");
    if (ch < 0) return send_error(req, 400, "Channel must be 0-3");

    cJSON *body = recv_json_body(req);
    if (body) cJSON_Delete(body);

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
    if (check_admin_auth(req) != ESP_OK) return send_error(req, 401, "Admin token required");

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
    if (check_admin_auth(req) != ESP_OK) return send_error(req, 401, "Admin token required");

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
    if (check_admin_auth(req) != ESP_OK) return send_error(req, 401, "Admin token required");

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

    int mode = modeItem->valueint;
    bool pulldown = pulldownItem ? cJSON_IsTrue(pulldownItem) : false;
    if (!tasks_apply_gpio_config((uint8_t)g, (GpioSelect)mode, pulldown)) {
        cJSON_Delete(doc);
        return send_error(req, 400, "Invalid GPIO config");
    }
    cJSON_Delete(doc);

    cJSON *resp = cJSON_CreateObject();
    cJSON_AddBoolToObject(resp, "ok", true);
    cJSON_AddNumberToObject(resp, "gpio", g);
    cJSON_AddNumberToObject(resp, "mode", mode);
    cJSON_AddBoolToObject(resp, "pulldown", pulldown);
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
    bool value = valueItem ? cJSON_IsTrue(valueItem) : false;
    if (!tasks_apply_gpio_output((uint8_t)g, value)) {
        cJSON_Delete(doc);
        return send_error(req, 400, "Invalid GPIO value");
    }
    cJSON_Delete(doc);

    cJSON *resp = cJSON_CreateObject();
    cJSON_AddBoolToObject(resp, "ok", true);
    cJSON_AddNumberToObject(resp, "gpio", g);
    cJSON_AddBoolToObject(resp, "value", value);
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

// POST /api/channel/?/rtd/config
static esp_err_t handle_post_rtd_config(httpd_req_t *req)
{
    int ch = extract_channel(req->uri);
    if (ch < 0) return send_error(req, 400, "Channel must be 0-3");

    cJSON *doc = recv_json_body(req);
    if (!doc) return send_error(req, 400, "Invalid JSON");

    // 2-wire RTD only. RTD_CURRENT bit maps to 0 = 500 µA, 1 = 1 mA.
    // Keep a tolerant excitation_ua parser so stale callers that still send
    // legacy low-current values (125/250) resolve to the 500 µA setting.
    cJSON *curItem = cJSON_GetObjectItem(doc, "current");
    cJSON *uaItem  = cJSON_GetObjectItem(doc, "excitation_ua");

    uint8_t current = 1; // default 1 mA (RTD_CURRENT bit set)
    if (curItem && cJSON_IsNumber(curItem)) {
        // Accept either the raw bit value (0/1) or stale µA-style values.
        int cur = curItem->valueint;
        current = (cur == 0 || cur == 1) ? ((cur != 0) ? 1 : 0)
                                         : ((cur >= 750) ? 1 : 0);
    } else if (uaItem && cJSON_IsNumber(uaItem)) {
        current = (uaItem->valueint >= 750) ? 1 : 0;
    }
    cJSON_Delete(doc);

    Command cmd{};
    cmd.type          = CMD_SET_RTD_CONFIG;
    cmd.channel       = (uint8_t)ch;
    cmd.rtdCfg.current = current;
    if (!sendCommand(cmd)) return send_error(req, 503, "Command queue full");

    cJSON *resp = cJSON_CreateObject();
    cJSON_AddBoolToObject(resp, "ok", true);
    cJSON_AddNumberToObject(resp, "channel", ch);
    cJSON_AddNumberToObject(resp, "current", current);
    cJSON_AddNumberToObject(resp, "excitation_ua", current ? 1000 : 500);
    return send_json(req, resp);
}

// POST /api/channel/* dispatcher
static esp_err_t handle_channel_post_dispatch(httpd_req_t *req)
{
    if (check_admin_auth(req) != ESP_OK) return send_error(req, 401, "Admin token required");

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
    if (strcmp(suffix, "rtd/config") == 0)   return handle_post_rtd_config(req);

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
    if (check_admin_auth(req) != ESP_OK) return send_error(req, 401, "Admin token required");

    const char *suffix = gpio_suffix(req->uri);
    if (!suffix) return send_error(req, 400, "Invalid GPIO URL");

    if (strcmp(suffix, "config") == 0)  return handle_post_gpio_config(req);
    if (strcmp(suffix, "set") == 0)     return handle_post_gpio_set(req);

    return send_error(req, 404, "Unknown GPIO POST endpoint");
}

// =============================================================================
// Digital IO (ESP32 GPIO) endpoints
// =============================================================================

// GET /api/dio — read all 12 IO states
static esp_err_t handle_get_dio(httpd_req_t *req)
{
    dio_poll_inputs();
    const DioState *all = dio_get_all();

    cJSON *root = cJSON_CreateObject();
    cJSON *ios  = cJSON_AddArrayToObject(root, "ios");

    for (int i = 0; i < DIO_NUM_IOS; i++) {
        cJSON *obj = cJSON_CreateObject();
        cJSON_AddNumberToObject(obj, "io",     i + 1);
        cJSON_AddNumberToObject(obj, "gpio",   all[i].gpio_num);
        cJSON_AddNumberToObject(obj, "mode",   all[i].mode);
        const char *mname = all[i].mode == DIO_MODE_INPUT  ? "input"  :
                            all[i].mode == DIO_MODE_OUTPUT ? "output" : "disabled";
        cJSON_AddStringToObject(obj, "modeName", mname);
        cJSON_AddBoolToObject(obj, "output", all[i].output_level);
        cJSON_AddBoolToObject(obj, "input",  all[i].input_level);
        cJSON_AddItemToArray(ios, obj);
    }

    return send_json(req, root);
}

// POST /api/dio/{n}/config  body: {"mode": 1}   (0=disabled, 1=input, 2=output)
static esp_err_t handle_post_dio_config(httpd_req_t *req)
{
    // Extract IO number from URI: /api/dio/3/config -> 3
    const char *p = strstr(req->uri, "/api/dio/");
    if (!p) return send_error(req, 400, "Invalid DIO URI");
    int io = atoi(p + 9);

    cJSON *body = recv_json_body(req);
    if (!body) return send_error(req, 400, "Invalid JSON");

    VALIDATE_JSON_FIELD(body, "mode", Number, "Missing field 'mode'");
    int mode = cJSON_GetObjectItem(body, "mode")->valueint;
    cJSON_Delete(body);

    if (!dio_configure((uint8_t)io, (uint8_t)mode)) {
        return send_error(req, 400, "Invalid IO or mode");
    }

    cJSON *resp = cJSON_CreateObject();
    cJSON_AddNumberToObject(resp, "io", io);
    cJSON_AddNumberToObject(resp, "mode", mode);
    cJSON_AddBoolToObject(resp, "ok", true);
    return send_json(req, resp);
}

// POST /api/dio/{n}/set  body: {"value": true}
static esp_err_t handle_post_dio_set(httpd_req_t *req)
{
    const char *p = strstr(req->uri, "/api/dio/");
    if (!p) return send_error(req, 400, "Invalid DIO URI");
    int io = atoi(p + 9);

    cJSON *body = recv_json_body(req);
    if (!body) return send_error(req, 400, "Invalid JSON");

    bool value = cJSON_IsTrue(cJSON_GetObjectItem(body, "value"));
    cJSON_Delete(body);

    if (!dio_write((uint8_t)io, value)) {
        return send_error(req, 400, "IO not configured as output");
    }

    cJSON *resp = cJSON_CreateObject();
    cJSON_AddNumberToObject(resp, "io", io);
    cJSON_AddBoolToObject(resp, "value", value);
    cJSON_AddBoolToObject(resp, "ok", true);
    return send_json(req, resp);
}

// GET /api/dio/{n} — read single IO
static esp_err_t handle_get_dio_single(httpd_req_t *req)
{
    const char *p = strstr(req->uri, "/api/dio/");
    if (!p) return send_error(req, 400, "Invalid DIO URI");
    int io = atoi(p + 9);

    DioState st;
    if (!dio_get_state((uint8_t)io, &st)) {
        return send_error(req, 400, "Invalid IO number");
    }

    bool level = false;
    if (st.mode == DIO_MODE_INPUT)       level = dio_read((uint8_t)io);
    else if (st.mode == DIO_MODE_OUTPUT) level = st.output_level;

    cJSON *resp = cJSON_CreateObject();
    cJSON_AddNumberToObject(resp, "io", io);
    cJSON_AddNumberToObject(resp, "gpio", st.gpio_num);
    cJSON_AddNumberToObject(resp, "mode", st.mode);
    cJSON_AddBoolToObject(resp, "value", level);
    return send_json(req, resp);
}

// POST /api/dio/* dispatcher
static esp_err_t handle_dio_post_dispatch(httpd_req_t *req)
{
    if (check_admin_auth(req) != ESP_OK) return send_error(req, 401, "Admin token required");

    const char *p = strstr(req->uri, "/api/dio/");
    if (!p) return send_error(req, 400, "Invalid DIO URI");
    // Find the action after /api/dio/N/
    p += 9; // skip "/api/dio/"
    while (*p >= '0' && *p <= '9') p++; // skip IO number
    if (*p == '/') p++;

    if (strcmp(p, "config") == 0) return handle_post_dio_config(req);
    if (strcmp(p, "set") == 0)    return handle_post_dio_set(req);

    return send_error(req, 404, "Unknown DIO POST endpoint");
}

// =============================================================================
// Self-Test / Calibration endpoints
// =============================================================================

// GET /api/selftest — boot result + cal status
static esp_err_t handle_get_selftest(httpd_req_t *req)
{
    const SelftestBootResult *boot = selftest_get_boot_result();
    const SelftestCalResult  *cal  = selftest_get_cal_result();

    cJSON *root = cJSON_CreateObject();
    cJSON *b = cJSON_AddObjectToObject(root, "boot");
    cJSON_AddBoolToObject(b, "ran", boot->ran);
    cJSON_AddBoolToObject(b, "passed", boot->passed);
    cJSON_AddNumberToObject(b, "vadj1V", boot->vadj1_v);
    cJSON_AddNumberToObject(b, "vadj2V", boot->vadj2_v);
    cJSON_AddNumberToObject(b, "vlogicV", boot->vlogic_v);

    cJSON *c = cJSON_AddObjectToObject(root, "calibration");
    cJSON_AddNumberToObject(c, "status", cal->status);
    cJSON_AddNumberToObject(c, "channel", cal->channel);
    cJSON_AddNumberToObject(c, "points", cal->points_collected);
    cJSON_AddNumberToObject(c, "lastVoltageV", cal->last_measured_v);
    cJSON_AddNumberToObject(c, "errorMv", cal->error_mv);

    return send_json(req, root);
}

// GET /api/selftest/supply/{rail} — measure a supply rail (0=VADJ1, 1=VADJ2, 2=3V3_ADJ)
static esp_err_t handle_get_selftest_supply(httpd_req_t *req)
{
    const char *p = strstr(req->uri, "/api/selftest/supply/");
    if (!p) return send_error(req, 400, "Invalid URI");
    int rail = atoi(p + 21);

    float voltage = selftest_measure_supply((uint8_t)rail);

    cJSON *resp = cJSON_CreateObject();
    const char *names[] = {"VADJ1", "VADJ2", "3V3_ADJ"};
    cJSON_AddStringToObject(resp, "rail", (rail < 3) ? names[rail] : "unknown");
    cJSON_AddNumberToObject(resp, "voltage", voltage);
    cJSON_AddBoolToObject(resp, "ok", voltage >= 0);
    return send_json(req, resp);
}

// GET /api/selftest/efuse — all e-fuse currents
static esp_err_t handle_get_selftest_efuse(httpd_req_t *req)
{
    selftest_monitor_step();
    const SelftestEfuseCurrents *ec = selftest_get_efuse_currents();

    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "available", ec->available);
    cJSON_AddNumberToObject(root, "timestampMs", ec->timestamp_ms);
    cJSON *arr = cJSON_AddArrayToObject(root, "efuses");
    for (int i = 0; i < SELFTEST_EFUSE_COUNT; i++) {
        cJSON *obj = cJSON_CreateObject();
        cJSON_AddNumberToObject(obj, "efuse", i + 1);
        cJSON_AddNumberToObject(obj, "currentA", ec->current_a[i]);
        cJSON_AddItemToArray(arr, obj);
    }
    return send_json(req, root);
}

// POST /api/selftest/calibrate body: {"channel": 1}
static esp_err_t handle_post_selftest_calibrate(httpd_req_t *req)
{
    if (check_admin_auth(req) != ESP_OK) return send_error(req, 401, "Admin token required");

    cJSON *body = recv_json_body(req);
    if (!body) return send_error(req, 400, "Invalid JSON");

    int ch = cJSON_GetObjectItem(body, "channel")->valueint;
    cJSON_Delete(body);

    bool ok = selftest_start_auto_calibrate((uint8_t)ch);
    if (!ok) {
        return send_error(req, 409, "Calibration blocked (busy or interlock)");
    }

    const SelftestCalResult *cal = selftest_get_cal_result();
    cJSON *resp = cJSON_CreateObject();
    cJSON_AddNumberToObject(resp, "status", cal->status);
    cJSON_AddNumberToObject(resp, "channel", cal->channel);
    cJSON_AddNumberToObject(resp, "points", cal->points_collected);
    cJSON_AddNumberToObject(resp, "lastVoltageV", cal->last_measured_v);
    cJSON_AddNumberToObject(resp, "errorMv", cal->error_mv);
    cJSON_AddBoolToObject(resp, "ok", true);
    return send_json(req, resp);
}

// GET /api/selftest/supplies — measure internal ADC supplies
static esp_err_t handle_get_selftest_supplies(httpd_req_t *req)
{
    const SelftestInternalSupplies *s = selftest_measure_internal_supplies();

    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "valid", s->valid);
    cJSON_AddBoolToObject(root, "suppliesOk", s->supplies_ok);
    cJSON_AddNumberToObject(root, "avddHiV", s->avdd_hi_v);
    cJSON_AddNumberToObject(root, "dvccV", s->dvcc_v);
    cJSON_AddNumberToObject(root, "avccV", s->avcc_v);
    cJSON_AddNumberToObject(root, "avssV", s->avss_v);
    cJSON_AddNumberToObject(root, "tempC", s->temp_c);
    return send_json(req, root);
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
    if (check_admin_auth(req) != ESP_OK) return send_error(req, 401, "Admin token required");

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
// I2C Device Handlers - DS4424 / HUSB238 / PCA9535
// =============================================================================

// GET /api/idac - Get all IDAC status
static esp_err_t handle_get_idac(httpd_req_t *req)
{
    cJSON *root = cJSON_CreateObject();
    const DS4424State *st = ds4424_get_state();
    cJSON_AddBoolToObject(root, "present", st->present);

    cJSON *channels = cJSON_AddArrayToObject(root, "channels");
    for (uint8_t ch = 0; ch < 3; ch++) {
        cJSON *obj = cJSON_CreateObject();
        cJSON_AddNumberToObject(obj, "id", ch);
        cJSON_AddNumberToObject(obj, "code", st->state[ch].dac_code);
        cJSON_AddNumberToObject(obj, "targetV", st->state[ch].target_v);
        cJSON_AddNumberToObject(obj, "midpointV", st->config[ch].midpoint_v);
        cJSON_AddNumberToObject(obj, "vMin", st->config[ch].v_min);
        cJSON_AddNumberToObject(obj, "vMax", st->config[ch].v_max);
        cJSON_AddNumberToObject(obj, "stepMv", ds4424_step_mv(ch));
        cJSON_AddBoolToObject(obj, "calibrated", st->cal[ch].valid);
        const char *names[] = {"LevelShift", "V_ADJ1", "V_ADJ2"};
        cJSON_AddStringToObject(obj, "name", names[ch]);
        cJSON_AddItemToArray(channels, obj);
    }
    return send_json(req, root);
}

// POST /api/idac/code  body: {"ch":0, "code":-10}
static esp_err_t handle_post_idac_code(httpd_req_t *req)
{
    cJSON *body = recv_json_body(req);
    if (!body) return send_error(req, 400, "Invalid JSON");

    int ch = cJSON_GetObjectItem(body, "ch") ? cJSON_GetObjectItem(body, "ch")->valueint : -1;
    int code = cJSON_GetObjectItem(body, "code") ? cJSON_GetObjectItem(body, "code")->valueint : 0;
    cJSON_Delete(body);

    if (ch < 0 || ch > 2) return send_error(req, 400, "ch must be 0-2");
    if (code < -127 || code > 127) return send_error(req, 400, "code must be -127..127");

    if (!ds4424_set_code(ch, (int8_t)code)) return send_error(req, 500, "I2C write failed");

    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "ch", ch);
    cJSON_AddNumberToObject(root, "code", code);
    cJSON_AddNumberToObject(root, "voltage", ds4424_code_to_voltage(ch, (int8_t)code));
    return send_json(req, root);
}

// POST /api/idac/voltage  body: {"ch":0, "voltage":3.3}
static esp_err_t handle_post_idac_voltage(httpd_req_t *req)
{
    cJSON *body = recv_json_body(req);
    if (!body) return send_error(req, 400, "Invalid JSON");

    int ch = cJSON_GetObjectItem(body, "ch") ? cJSON_GetObjectItem(body, "ch")->valueint : -1;
    double voltage = cJSON_GetObjectItem(body, "voltage") ? cJSON_GetObjectItem(body, "voltage")->valuedouble : 0;
    cJSON_Delete(body);

    if (ch < 0 || ch > 2) return send_error(req, 400, "ch must be 0-2");

    if (!ds4424_set_voltage(ch, (float)voltage)) return send_error(req, 500, "Failed to set voltage");

    const DS4424State *st = ds4424_get_state();
    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "ch", ch);
    cJSON_AddNumberToObject(root, "code", st->state[ch].dac_code);
    cJSON_AddNumberToObject(root, "voltage", st->state[ch].target_v);
    return send_json(req, root);
}

// POST /api/idac dispatch
static esp_err_t handle_idac_post_dispatch(httpd_req_t *req)
{
    if (check_admin_auth(req) != ESP_OK) return send_error(req, 401, "Admin token required");

    if (strstr(req->uri, "/api/idac/code")) return handle_post_idac_code(req);
    if (strstr(req->uri, "/api/idac/voltage")) return handle_post_idac_voltage(req);
    return send_error(req, 404, "Unknown IDAC endpoint");
}

// GET /api/usbpd - Get USB PD status
static esp_err_t handle_get_usbpd(httpd_req_t *req)
{
    husb238_update();
    const Husb238State *st = husb238_get_state();
    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "present", st->present);
    cJSON_AddBoolToObject(root, "attached", st->attached);
    cJSON_AddStringToObject(root, "cc", st->cc_direction ? "CC2" : "CC1");
    cJSON_AddNumberToObject(root, "voltageV", st->voltage_v);
    cJSON_AddNumberToObject(root, "currentA", st->current_a);
    cJSON_AddNumberToObject(root, "powerW", st->power_w);
    cJSON_AddNumberToObject(root, "pdResponse", st->pd_response);

    cJSON *pdos = cJSON_AddArrayToObject(root, "sourcePdos");
    struct { const char *name; float v; Husb238PdoInfo pdo; } list[] = {
        {"5V",  5.0f,  st->pdo_5v},  {"9V",  9.0f,  st->pdo_9v},
        {"12V", 12.0f, st->pdo_12v}, {"15V", 15.0f, st->pdo_15v},
        {"18V", 18.0f, st->pdo_18v}, {"20V", 20.0f, st->pdo_20v}
    };
    for (int i = 0; i < 6; i++) {
        cJSON *obj = cJSON_CreateObject();
        cJSON_AddStringToObject(obj, "voltage", list[i].name);
        cJSON_AddBoolToObject(obj, "detected", list[i].pdo.detected);
        cJSON_AddNumberToObject(obj, "maxCurrentA", husb238_decode_current(list[i].pdo.max_current));
        cJSON_AddNumberToObject(obj, "maxPowerW", list[i].v * husb238_decode_current(list[i].pdo.max_current));
        cJSON_AddItemToArray(pdos, obj);
    }
    cJSON_AddNumberToObject(root, "selectedPdo", st->selected_pdo);
    return send_json(req, root);
}

// POST /api/usbpd/select  body: {"voltage": 20}
static esp_err_t handle_post_usbpd_select(httpd_req_t *req)
{
    cJSON *body = recv_json_body(req);
    if (!body) return send_error(req, 400, "Invalid JSON");
    int v = cJSON_GetObjectItem(body, "voltage") ? cJSON_GetObjectItem(body, "voltage")->valueint : 0;
    cJSON_Delete(body);

    Husb238Voltage voltage;
    switch (v) {
        case 5: voltage = HUSB238_V_5V; break;
        case 9: voltage = HUSB238_V_9V; break;
        case 12: voltage = HUSB238_V_12V; break;
        case 15: voltage = HUSB238_V_15V; break;
        case 18: voltage = HUSB238_V_18V; break;
        case 20: voltage = HUSB238_V_20V; break;
        default: return send_error(req, 400, "Invalid voltage (5/9/12/15/18/20)");
    }

    husb238_select_pdo(voltage);
    husb238_go_command(HUSB238_GO_SELECT_PDO);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "selectedVoltage", v);
    cJSON_AddStringToObject(root, "status", "negotiating");
    return send_json(req, root);
}

// POST /api/usbpd dispatch
static esp_err_t handle_usbpd_post_dispatch(httpd_req_t *req)
{
    if (check_admin_auth(req) != ESP_OK) return send_error(req, 401, "Admin token required");

    if (strstr(req->uri, "/api/usbpd/select")) return handle_post_usbpd_select(req);
    if (strstr(req->uri, "/api/usbpd/caps")) {
        husb238_get_src_cap();
        cJSON *root = cJSON_CreateObject();
        cJSON_AddStringToObject(root, "status", "caps_requested");
        return send_json(req, root);
    }
    return send_error(req, 404, "Unknown USB PD endpoint");
}

// GET /api/ioexp - Get PCA9535 status
static esp_err_t handle_get_ioexp(httpd_req_t *req)
{
    pca9535_update();
    const PCA9535State *st = pca9535_get_state();
    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "present", st->present);

    // Raw registers
    cJSON_AddNumberToObject(root, "input0", st->input0);
    cJSON_AddNumberToObject(root, "input1", st->input1);
    cJSON_AddNumberToObject(root, "output0", st->output0);
    cJSON_AddNumberToObject(root, "output1", st->output1);

    // Power good
    cJSON *pg = cJSON_AddObjectToObject(root, "powerGood");
    cJSON_AddBoolToObject(pg, "logic", st->logic_pg);
    cJSON_AddBoolToObject(pg, "vadj1", st->vadj1_pg);
    cJSON_AddBoolToObject(pg, "vadj2", st->vadj2_pg);

    // Enables
    cJSON *en = cJSON_AddObjectToObject(root, "enables");
    cJSON_AddBoolToObject(en, "vadj1", st->vadj1_en);
    cJSON_AddBoolToObject(en, "vadj2", st->vadj2_en);
    cJSON_AddBoolToObject(en, "analog15v", st->en_15v);
    cJSON_AddBoolToObject(en, "mux", st->en_mux);
    cJSON_AddBoolToObject(en, "usbHub", st->en_usb_hub);

    // E-Fuses
    cJSON *efuses = cJSON_AddArrayToObject(root, "efuses");
    for (int i = 0; i < 4; i++) {
        cJSON *obj = cJSON_CreateObject();
        cJSON_AddNumberToObject(obj, "id", i + 1);
        cJSON_AddBoolToObject(obj, "enabled", st->efuse_en[i]);
        cJSON_AddBoolToObject(obj, "fault", st->efuse_flt[i]);
        cJSON_AddItemToArray(efuses, obj);
    }
    return send_json(req, root);
}

// POST /api/ioexp/control  body: {"control":"vadj1", "on":true}
static esp_err_t handle_post_ioexp_control(httpd_req_t *req)
{
    cJSON *body = recv_json_body(req);
    if (!body) return send_error(req, 400, "Invalid JSON");

    cJSON *ctrl_item = cJSON_GetObjectItem(body, "control");
    const char *ctrl_name = (ctrl_item && cJSON_IsString(ctrl_item)) ? ctrl_item->valuestring : NULL;
    cJSON *on_item = cJSON_GetObjectItem(body, "on");
    bool on = on_item ? cJSON_IsTrue(on_item) : false;

    if (!ctrl_name) { cJSON_Delete(body); return send_error(req, 400, "Missing 'control' field"); }

    PcaControl ctrl;
    if (strcmp(ctrl_name, "vadj1") == 0)       ctrl = PCA_CTRL_VADJ1_EN;
    else if (strcmp(ctrl_name, "vadj2") == 0)  ctrl = PCA_CTRL_VADJ2_EN;
    else if (strcmp(ctrl_name, "15v") == 0)     ctrl = PCA_CTRL_15V_EN;
    else if (strcmp(ctrl_name, "mux") == 0)     ctrl = PCA_CTRL_MUX_EN;
    else if (strcmp(ctrl_name, "usb") == 0)     ctrl = PCA_CTRL_USB_HUB_EN;
    else if (strcmp(ctrl_name, "efuse1") == 0)  ctrl = PCA_CTRL_EFUSE1_EN;
    else if (strcmp(ctrl_name, "efuse2") == 0)  ctrl = PCA_CTRL_EFUSE2_EN;
    else if (strcmp(ctrl_name, "efuse3") == 0)  ctrl = PCA_CTRL_EFUSE3_EN;
    else if (strcmp(ctrl_name, "efuse4") == 0)  ctrl = PCA_CTRL_EFUSE4_EN;
    else { cJSON_Delete(body); return send_error(req, 400, "Unknown control name"); }
    cJSON_Delete(body);

    if (!pca9535_set_control(ctrl, on)) return send_error(req, 500, "I2C write failed");

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "control", pca9535_control_name(ctrl));
    cJSON_AddBoolToObject(root, "on", on);
    return send_json(req, root);
}

// GET /api/ioexp/faults - Get PCA9535 fault log
static esp_err_t handle_get_ioexp_faults(httpd_req_t *req)
{
    cJSON *root = cJSON_CreateObject();
    cJSON *arr = cJSON_AddArrayToObject(root, "faults");

    if (xSemaphoreTake(g_stateMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        uint8_t count = g_deviceState.pcaFaultLogCount;
        cJSON_AddNumberToObject(root, "count", count);
        for (uint8_t i = 0; i < count && i < DeviceState::PCA_FAULT_LOG_SIZE; i++) {
            uint8_t idx = (g_deviceState.pcaFaultLogHead - count + i + DeviceState::PCA_FAULT_LOG_SIZE)
                          % DeviceState::PCA_FAULT_LOG_SIZE;
            const auto &entry = g_deviceState.pcaFaultLog[idx];
            cJSON *item = cJSON_CreateObject();
            cJSON_AddNumberToObject(item, "type", entry.type);
            cJSON_AddNumberToObject(item, "channel", entry.channel);
            cJSON_AddNumberToObject(item, "timestamp_ms", entry.timestamp_ms);
            const char *typeStr[] = {"efuse_trip", "efuse_clear", "pg_lost", "pg_restored"};
            if (entry.type < 4) cJSON_AddStringToObject(item, "typeName", typeStr[entry.type]);
            cJSON_AddItemToArray(arr, item);
        }
        xSemaphoreGive(g_stateMutex);
    } else {
        cJSON_AddNumberToObject(root, "count", 0);
    }

    return send_json(req, root);
}

// POST /api/ioexp/fault_config  body: {"auto_disable":true, "log_events":true}
static esp_err_t handle_post_ioexp_fault_config(httpd_req_t *req)
{
    char buf[128];
    int ret = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (ret <= 0) return send_error(req, 400, "Empty body");
    buf[ret] = '\0';

    cJSON *json = cJSON_Parse(buf);
    if (!json) return send_error(req, 400, "Invalid JSON");

    PcaFaultConfig cfg;
    cfg.auto_disable_efuse = cJSON_IsTrue(cJSON_GetObjectItem(json, "auto_disable"));
    cfg.log_events = cJSON_IsTrue(cJSON_GetObjectItem(json, "log_events"));
    cJSON_Delete(json);

    pca9535_set_fault_config(&cfg);

    cJSON *rsp = cJSON_CreateObject();
    cJSON_AddBoolToObject(rsp, "auto_disable", cfg.auto_disable_efuse);
    cJSON_AddBoolToObject(rsp, "log_events", cfg.log_events);
    return send_json(req, rsp);
}

// POST /api/ioexp dispatch
static esp_err_t handle_ioexp_post_dispatch(httpd_req_t *req)
{
    if (check_admin_auth(req) != ESP_OK) return send_error(req, 401, "Admin token required");

    if (strstr(req->uri, "/api/ioexp/control")) return handle_post_ioexp_control(req);
    if (strstr(req->uri, "/api/ioexp/fault_config")) return handle_post_ioexp_fault_config(req);
    return send_error(req, 404, "Unknown IO Expander endpoint");
}

// =============================================================================
// HAT Expansion Board API
// =============================================================================

// GET /api/hat - Get HAT status
static esp_err_t handle_get_hat(httpd_req_t *req)
{
    const HatState *hs = hat_get_state();
    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "detected", hs->detected);
    cJSON_AddBoolToObject(root, "connected", hs->connected);
    cJSON_AddNumberToObject(root, "type", hs->type);
    cJSON_AddStringToObject(root, "typeName", hat_type_name(hs->type));
    cJSON_AddNumberToObject(root, "detectVoltage", hs->detect_voltage);
    cJSON_AddNumberToObject(root, "detect_voltage", hs->detect_voltage);
    cJSON_AddNumberToObject(root, "fwMajor", hs->fw_version_major);
    cJSON_AddNumberToObject(root, "fwMinor", hs->fw_version_minor);
    cJSON_AddBoolToObject(root, "configConfirmed", hs->config_confirmed);
    cJSON_AddBoolToObject(root, "config_confirmed", hs->config_confirmed);

    cJSON *pins = cJSON_AddArrayToObject(root, "pinConfig");
    cJSON *pin_config = cJSON_AddArrayToObject(root, "pin_config");
    for (int i = 0; i < HAT_NUM_EXT_PINS; i++) {
        cJSON *pin = cJSON_CreateObject();
        cJSON_AddNumberToObject(pin, "pin", i);
        cJSON_AddNumberToObject(pin, "function", hs->pin_config[i]);
        cJSON_AddStringToObject(pin, "functionName", hat_func_name(hs->pin_config[i]));
        cJSON_AddItemToArray(pins, pin);
        cJSON_AddItemToArray(pin_config, cJSON_CreateNumber(hs->pin_config[i]));
    }

    return send_json(req, root);
}

// GET /api/hat/la/status
static esp_err_t handle_get_hat_la_status(httpd_req_t *req)
{
    HatLaStatus st = {};
    if (!hat_la_get_status(&st)) {
        return send_error(req, 503, "HAT not responding or disconnected");
    }

    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "state", st.state);
    cJSON_AddStringToObject(root, "stateName", hat_la_state_name(st.state));
    cJSON_AddNumberToObject(root, "samplesCaptured", st.samples_captured);
    cJSON_AddNumberToObject(root, "totalSamples", st.total_samples);
    cJSON_AddNumberToObject(root, "actualRateHz", st.actual_rate_hz);
    cJSON_AddNumberToObject(root, "channels", st.channels);
    cJSON_AddBoolToObject(root, "usbConnected", st.usb_connected);
    cJSON_AddBoolToObject(root, "usbMounted", st.usb_mounted);
    cJSON_AddNumberToObject(root, "stopReason", st.stream_stop_reason);
    cJSON_AddStringToObject(root, "stopReasonName", hat_la_stop_reason_name(st.stream_stop_reason));
    cJSON_AddNumberToObject(root, "overrunCount", st.stream_overrun_count);
    cJSON_AddNumberToObject(root, "shortWriteCount", st.stream_short_write_count);
    cJSON_AddBoolToObject(root, "usbRearmPending", st.usb_rearm_pending);

    return send_json(req, root);
}

// POST /api/hat/config  body: {"pin":0, "function":1} or {"pins":[1,2,3,4]}
static esp_err_t handle_post_hat_config(httpd_req_t *req)
{
    cJSON *doc = recv_json_body(req);
    if (!doc) return send_error(req, 400, "Invalid JSON");

    cJSON *pinItem   = cJSON_GetObjectItem(doc, "pin");
    cJSON *funcItem  = cJSON_GetObjectItem(doc, "function");
    cJSON *pinsArray = cJSON_GetObjectItem(doc, "pins");

    bool ok = false;

    if (pinsArray && cJSON_IsArray(pinsArray)) {
        if (cJSON_GetArraySize(pinsArray) != HAT_NUM_EXT_PINS) {
            cJSON_Delete(doc);
            return send_error(req, 400, "Field 'pins' must have 4 elements");
        }
        // Set all pins at once
        HatPinFunction config[HAT_NUM_EXT_PINS];
        for (int i = 0; i < HAT_NUM_EXT_PINS; i++) {
            cJSON *item = cJSON_GetArrayItem(pinsArray, i);
            if (!item || !cJSON_IsNumber(item)) {
                cJSON_Delete(doc);
                return send_error(req, 400, "Field 'pins' elements must be numbers");
            }
            config[i] = (HatPinFunction)item->valueint;
        }
        ok = hat_set_all_pins(config);
    } else if (pinItem && funcItem && cJSON_IsNumber(pinItem) && cJSON_IsNumber(funcItem)) {
        // Set single pin
        ok = hat_set_pin((uint8_t)pinItem->valueint, (HatPinFunction)funcItem->valueint);
    } else {
        cJSON_Delete(doc);
        return send_error(req, 400, "Missing {pin, function} as Numbers OR {pins} as Array");
    }

    cJSON_Delete(doc);

    cJSON *rsp = cJSON_CreateObject();
    cJSON_AddBoolToObject(rsp, "ok", ok);
    cJSON_AddBoolToObject(rsp, "confirmed", ok);
    return send_json(req, rsp);
}

// POST /api/hat/reset
static esp_err_t handle_post_hat_reset(httpd_req_t *req)
{
    bool ok = hat_reset();
    cJSON *rsp = cJSON_CreateObject();
    cJSON_AddBoolToObject(rsp, "ok", ok);
    return send_json(req, rsp);
}

// POST /api/hat/detect
static esp_err_t handle_post_hat_detect(httpd_req_t *req)
{
    HatType type = hat_detect();
    const HatState *hs = hat_get_state();
    if (hs->detected && !hs->connected) {
        hat_connect();
    }
    cJSON *rsp = cJSON_CreateObject();
    cJSON_AddBoolToObject(rsp, "detected", hs->detected);
    cJSON_AddBoolToObject(rsp, "connected", hs->connected);
    cJSON_AddNumberToObject(rsp, "type", type);
    cJSON_AddStringToObject(rsp, "typeName", hat_type_name(type));
    cJSON_AddNumberToObject(rsp, "detectVoltage", hs->detect_voltage);
    cJSON_AddNumberToObject(rsp, "detect_voltage", hs->detect_voltage);
    return send_json(req, rsp);
}

// POST /api/hat dispatch
static esp_err_t handle_hat_post_dispatch(httpd_req_t *req)
{
    if (check_admin_auth(req) != ESP_OK) return send_error(req, 401, "Admin token required");

    if (strstr(req->uri, "/api/hat/config")) return handle_post_hat_config(req);
    if (strstr(req->uri, "/api/hat/reset")) return handle_post_hat_reset(req);
    if (strstr(req->uri, "/api/hat/detect")) return handle_post_hat_detect(req);
    return send_error(req, 404, "Unknown HAT endpoint");
}

// GET /api/debug - Combined debug status of all I2C devices
static esp_err_t handle_get_debug(httpd_req_t *req)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "i2cBusOk", i2c_bus_ready());

    // DS4424
    cJSON *idac = cJSON_AddObjectToObject(root, "ds4424");
    cJSON_AddBoolToObject(idac, "present", ds4424_present());
    if (ds4424_present()) {
        const DS4424State *st = ds4424_get_state();
        for (uint8_t ch = 0; ch < 3; ch++) {
            char key[8]; snprintf(key, sizeof(key), "ch%d", ch);
            cJSON *obj = cJSON_AddObjectToObject(idac, key);
            cJSON_AddNumberToObject(obj, "code", st->state[ch].dac_code);
            cJSON_AddNumberToObject(obj, "targetV", st->state[ch].target_v);
        }
    }

    // HUSB238
    cJSON *pd = cJSON_AddObjectToObject(root, "husb238");
    cJSON_AddBoolToObject(pd, "present", husb238_present());
    if (husb238_present()) {
        husb238_update();
        const Husb238State *st = husb238_get_state();
        cJSON_AddBoolToObject(pd, "attached", st->attached);
        cJSON_AddNumberToObject(pd, "voltageV", st->voltage_v);
        cJSON_AddNumberToObject(pd, "currentA", st->current_a);
    }

    // PCA9535
    cJSON *io = cJSON_AddObjectToObject(root, "pca9535");
    cJSON_AddBoolToObject(io, "present", pca9535_present());
    if (pca9535_present()) {
        pca9535_update();
        const PCA9535State *st = pca9535_get_state();
        cJSON_AddNumberToObject(io, "input0", st->input0);
        cJSON_AddNumberToObject(io, "input1", st->input1);
        cJSON_AddNumberToObject(io, "output0", st->output0);
        cJSON_AddNumberToObject(io, "output1", st->output1);
    }

    return send_json(req, root);
}

// =============================================================================
// Waveform Generator Web Handlers
// =============================================================================

// POST /api/wavegen/start
static esp_err_t handle_post_wavegen_start(httpd_req_t *req)
{
    cJSON *body = recv_json_body(req);
    if (!body) return send_error(req, 400, "Invalid JSON");

    int ch = cJSON_GetObjectItem(body, "channel") ? cJSON_GetObjectItem(body, "channel")->valueint : 0;
    int wf = cJSON_GetObjectItem(body, "waveform") ? cJSON_GetObjectItem(body, "waveform")->valueint : 0;
    double freq = cJSON_GetObjectItem(body, "freq_hz") ? cJSON_GetObjectItem(body, "freq_hz")->valuedouble : 1.0;
    double amp = cJSON_GetObjectItem(body, "amplitude") ? cJSON_GetObjectItem(body, "amplitude")->valuedouble : 5.0;
    double off = cJSON_GetObjectItem(body, "offset") ? cJSON_GetObjectItem(body, "offset")->valuedouble : 0.0;
    int mode = cJSON_GetObjectItem(body, "mode") ? cJSON_GetObjectItem(body, "mode")->valueint : 0;
    cJSON_Delete(body);

    if (ch > 3) return send_error(req, 400, "Invalid channel");
    if (wf > 3) return send_error(req, 400, "Invalid waveform");
    if (freq < 0.01 || freq > 100.0) return send_error(req, 400, "Frequency out of range");

    // Apply channel function synchronously — see bbp.cpp handleStartWavegen for rationale.
    tasks_apply_channel_function((uint8_t)ch, (mode == 1) ? CH_FUNC_IOUT : CH_FUNC_VOUT);

    // Set wavegen state
    if (xSemaphoreTake(g_stateMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        g_deviceState.wavegen.active    = true;
        g_deviceState.wavegen.channel   = (uint8_t)ch;
        g_deviceState.wavegen.waveform  = (WaveformType)wf;
        g_deviceState.wavegen.freq_hz   = (float)freq;
        g_deviceState.wavegen.amplitude = (float)amp;
        g_deviceState.wavegen.offset    = (float)off;
        g_deviceState.wavegen.mode      = (WavegenMode)mode;
        xSemaphoreGive(g_stateMutex);
    }

    // Wake wavegen task
    extern TaskHandle_t s_wavegenTask;
    if (s_wavegenTask) {
        xTaskNotifyGive(s_wavegenTask);
    }

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "status", "started");
    return send_json(req, root);
}

// POST /api/wavegen/stop
static esp_err_t handle_post_wavegen_stop(httpd_req_t *req)
{
    bbpStopWavegen();  // Full stop: sets active=false AND resets channel to HIGH_IMP

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "status", "stopped");
    return send_json(req, root);
}

// POST /api/wavegen/* dispatcher
static esp_err_t handle_wavegen_post_dispatch(httpd_req_t *req)
{
    if (check_admin_auth(req) != ESP_OK) return send_error(req, 401, "Admin token required");

    if (strstr(req->uri, "/api/wavegen/start")) return handle_post_wavegen_start(req);
    if (strstr(req->uri, "/api/wavegen/stop")) return handle_post_wavegen_stop(req);
    return send_error(req, 404, "Unknown wavegen endpoint");
}

// =============================================================================
// MUX Switch Web Handlers
// =============================================================================

// GET /api/mux
static esp_err_t handle_get_mux(httpd_req_t *req)
{
    cJSON *root = cJSON_CreateObject();
    uint8_t states[ADGS_API_MAIN_DEVICES] = {};
    adgs_get_all_states(states);
    adgs_get_api_states(states);
    cJSON *arr = cJSON_AddArrayToObject(root, "states");
    for (int i = 0; i < ADGS_API_MAIN_DEVICES; i++) {
        cJSON_AddItemToArray(arr, cJSON_CreateNumber(states[i]));
    }
    cJSON_AddNumberToObject(root, "numDevices", ADGS_API_MAIN_DEVICES);
    cJSON_AddNumberToObject(root, "physicalDevices", ADGS_MAIN_DEVICES);
    return send_json(req, root);
}

// POST /api/mux/switch  body: {"device":0, "switch":0, "closed":true}
static esp_err_t handle_post_mux_switch(httpd_req_t *req)
{
    cJSON *body = recv_json_body(req);
    if (!body) return send_error(req, 400, "Invalid JSON");
    int dev = cJSON_GetObjectItem(body, "device") ? cJSON_GetObjectItem(body, "device")->valueint : -1;
    int sw = cJSON_GetObjectItem(body, "switch") ? cJSON_GetObjectItem(body, "switch")->valueint : -1;
    bool closed = cJSON_GetObjectItem(body, "closed") ? cJSON_IsTrue(cJSON_GetObjectItem(body, "closed")) : false;
    cJSON_Delete(body);
    if (dev < 0 || dev >= ADGS_API_MAIN_DEVICES || sw < 0 || sw >= ADGS_NUM_SWITCHES)
        return send_error(req, 400, "Invalid device/switch");

    if (!adgs_set_api_switch_safe((uint8_t)dev, (uint8_t)sw, closed)) {
        return send_error(req, 400, "Invalid device/switch");
    }
    if (xSemaphoreTake(g_stateMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        adgs_get_all_states(g_deviceState.muxState);
        xSemaphoreGive(g_stateMutex);
    }

    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "device", dev);
    cJSON_AddNumberToObject(root, "switch", sw);
    cJSON_AddBoolToObject(root, "closed", closed);
    return send_json(req, root);
}

// POST /api/mux/all  body: {"states":[0,0,0,0]}
static esp_err_t handle_post_mux_all(httpd_req_t *req)
{
    cJSON *body = recv_json_body(req);
    if (!body) return send_error(req, 400, "Invalid JSON");
    cJSON *arr = cJSON_GetObjectItem(body, "states");
    if (!arr || !cJSON_IsArray(arr)) { cJSON_Delete(body); return send_error(req, 400, "Missing states array"); }
    if (cJSON_GetArraySize(arr) < ADGS_API_MAIN_DEVICES) {
        cJSON_Delete(body);
        return send_error(req, 400, "states must contain 4 device bytes");
    }
    uint8_t states[ADGS_API_MAIN_DEVICES] = {};
    for (int i = 0; i < ADGS_API_MAIN_DEVICES && i < cJSON_GetArraySize(arr); i++) {
        states[i] = (uint8_t)cJSON_GetArrayItem(arr, i)->valueint;
    }
    cJSON_Delete(body);
    adgs_set_api_all_safe(states);
    if (xSemaphoreTake(g_stateMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        adgs_get_all_states(g_deviceState.muxState);
        xSemaphoreGive(g_stateMutex);
    }
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "status", "ok");
    return send_json(req, root);
}

static esp_err_t handle_mux_post_dispatch(httpd_req_t *req)
{
    if (check_admin_auth(req) != ESP_OK) return send_error(req, 401, "Admin token required");

    if (strstr(req->uri, "/api/mux/switch")) return handle_post_mux_switch(req);
    if (strstr(req->uri, "/api/mux/all")) return handle_post_mux_all(req);
    return send_error(req, 404, "Unknown MUX endpoint");
}

// =============================================================================
// IDAC Calibration Web Handlers
// =============================================================================

// POST /api/idac/cal/point  body: {"ch":0, "code":-10, "measuredV":5.23}
static esp_err_t handle_post_cal_point(httpd_req_t *req)
{
    cJSON *body = recv_json_body(req);
    if (!body) return send_error(req, 400, "Invalid JSON");
    int ch = cJSON_GetObjectItem(body, "ch") ? cJSON_GetObjectItem(body, "ch")->valueint : -1;
    int code = cJSON_GetObjectItem(body, "code") ? cJSON_GetObjectItem(body, "code")->valueint : 0;
    double v = cJSON_GetObjectItem(body, "measuredV") ? cJSON_GetObjectItem(body, "measuredV")->valuedouble : 0;
    cJSON_Delete(body);
    if (ch < 0 || ch > 2) return send_error(req, 400, "ch must be 0-2");
    ds4424_cal_add_point(ch, (int8_t)code, (float)v);
    const DS4424State *st = ds4424_get_state();
    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "count", st->cal[ch].count);
    cJSON_AddBoolToObject(root, "valid", st->cal[ch].valid);
    return send_json(req, root);
}

// POST /api/idac/cal/clear  body: {"ch":0}
static esp_err_t handle_post_cal_clear(httpd_req_t *req)
{
    cJSON *body = recv_json_body(req);
    if (!body) return send_error(req, 400, "Invalid JSON");
    int ch = cJSON_GetObjectItem(body, "ch") ? cJSON_GetObjectItem(body, "ch")->valueint : -1;
    cJSON_Delete(body);
    if (ch < 0 || ch > 2) return send_error(req, 400, "ch must be 0-2");
    ds4424_cal_clear(ch);
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "status", "cleared");
    return send_json(req, root);
}

// POST /api/idac/cal/save
static esp_err_t handle_post_cal_save(httpd_req_t *req)
{
    bool ok = ds4424_cal_save();
    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "success", ok);
    return send_json(req, root);
}

static esp_err_t handle_cal_post_dispatch(httpd_req_t *req)
{
    if (check_admin_auth(req) != ESP_OK) return send_error(req, 401, "Admin token required");

    if (strstr(req->uri, "/api/idac/cal/point")) return handle_post_cal_point(req);
    if (strstr(req->uri, "/api/idac/cal/clear")) return handle_post_cal_clear(req);
    if (strstr(req->uri, "/api/idac/cal/save")) return handle_post_cal_save(req);
    return send_error(req, 404, "Unknown calibration endpoint");
}

// POST /api/lshift/oe  body: {"on":true}
static esp_err_t handle_post_lshift_oe(httpd_req_t *req)
{
    if (check_admin_auth(req) != ESP_OK) return send_error(req, 401, "Admin token required");

    cJSON *body = recv_json_body(req);
    if (!body) return send_error(req, 400, "Invalid JSON");
    bool on = cJSON_GetObjectItem(body, "on") ? cJSON_IsTrue(cJSON_GetObjectItem(body, "on")) : false;
    cJSON_Delete(body);
    pin_write(PIN_LSHIFT_OE, on ? 1 : 0);
    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "on", on);
    return send_json(req, root);
}

// =============================================================================
// WiFi Status & Connect
// =============================================================================

// GET /api/wifi
static esp_err_t handle_get_wifi(httpd_req_t *req)
{
    cJSON *root = cJSON_CreateObject();

    cJSON_AddBoolToObject(root, "connected", wifi_is_connected());
    cJSON_AddStringToObject(root, "staSSID", wifi_get_sta_ssid());
    cJSON_AddStringToObject(root, "sta_ssid", wifi_get_sta_ssid());
    cJSON_AddStringToObject(root, "staIP", wifi_get_sta_ip());
    cJSON_AddStringToObject(root, "sta_ip", wifi_get_sta_ip());
    cJSON_AddNumberToObject(root, "rssi", wifi_get_rssi());

    // Get AP SSID from ESP-IDF config
    wifi_config_t ap_cfg = {};
    esp_wifi_get_config(WIFI_IF_AP, &ap_cfg);
    cJSON_AddStringToObject(root, "apSSID", (const char *)ap_cfg.ap.ssid);
    cJSON_AddStringToObject(root, "ap_ssid", (const char *)ap_cfg.ap.ssid);
    cJSON_AddStringToObject(root, "apIP", wifi_get_ap_ip());
    cJSON_AddStringToObject(root, "ap_ip", wifi_get_ap_ip());
    cJSON_AddStringToObject(root, "apMAC", wifi_get_ap_mac());
    cJSON_AddStringToObject(root, "ap_mac", wifi_get_ap_mac());

    return send_json(req, root);
}

// POST /api/wifi/connect  body: {"ssid":"...", "password":"..."}
static esp_err_t handle_post_wifi_connect(httpd_req_t *req)
{
    if (check_admin_auth(req) != ESP_OK) {
        return send_error(req, 401, "Admin token required");
    }

    cJSON *body = recv_json_body(req);
    if (!body) return send_error(req, 400, "Invalid JSON");

    cJSON *j_ssid = cJSON_GetObjectItem(body, "ssid");
    cJSON *j_pass = cJSON_GetObjectItem(body, "password");
    if (!j_ssid || !cJSON_IsString(j_ssid)) {
        cJSON_Delete(body);
        return send_error(req, 400, "Missing ssid");
    }
    const char *ssid = j_ssid->valuestring;
    const char *pass = (j_pass && cJSON_IsString(j_pass)) ? j_pass->valuestring : "";
    cJSON_Delete(body);

    ESP_LOGI(TAG, "WiFi connect request to '%s'", ssid);
    bool ok = wifi_connect(ssid, pass);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "success", ok);
    cJSON_AddStringToObject(root, "ip", ok ? wifi_get_sta_ip() : "");
    return send_json(req, root);
}

// GET /api/wifi/scan
static esp_err_t handle_get_wifi_scan(httpd_req_t *req)
{
    wifi_scan_result_t results[20];
    int count = wifi_scan(results, 20);

    cJSON *root = cJSON_CreateObject();
    cJSON *arr = cJSON_AddArrayToObject(root, "networks");
    for (int i = 0; i < count; i++) {
        cJSON *item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "ssid", results[i].ssid);
        cJSON_AddNumberToObject(item, "rssi", results[i].rssi);
        cJSON_AddNumberToObject(item, "auth", results[i].auth);
        cJSON_AddItemToArray(arr, item);
    }
    return send_json(req, root);
}

// GET /api/device/version
static esp_err_t handle_get_version(httpd_req_t *req)
{
    const esp_app_desc_t *app = esp_app_get_description();
    const esp_partition_t *running = esp_ota_get_running_partition();
    const esp_partition_t *next = esp_ota_get_next_update_partition(NULL);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "version", app->version);
    cJSON_AddStringToObject(root, "date", app->date);
    cJSON_AddStringToObject(root, "time", app->time);
    cJSON_AddStringToObject(root, "idfVersion", app->idf_ver);
    cJSON_AddNumberToObject(root, "fwMajor", BBP_FW_VERSION_MAJOR);
    cJSON_AddNumberToObject(root, "fwMinor", BBP_FW_VERSION_MINOR);
    cJSON_AddNumberToObject(root, "fwPatch", BBP_FW_VERSION_PATCH);
    cJSON_AddNumberToObject(root, "protoVersion", BBP_PROTO_VERSION);
    if (running) {
        cJSON_AddStringToObject(root, "partition", running->label);
        cJSON_AddNumberToObject(root, "partitionSize", running->size);
    }
    if (next) {
        cJSON_AddStringToObject(root, "nextPartition", next->label);
        cJSON_AddNumberToObject(root, "nextPartitionSize", next->size);
    }
    return send_json(req, root);
}

// POST /api/ota/upload — OTA firmware update (binary body = firmware.bin)
static esp_err_t handle_ota_upload(httpd_req_t *req)
{
    if (check_admin_auth(req) != ESP_OK) {
        return send_error(req, 401, "Admin token required");
    }

    ESP_LOGI(TAG, "OTA upload started, content_len=%d", req->content_len);

    if (req->content_len <= 0 || req->content_len > 2 * 1024 * 1024) {
        return send_error(req, 400, "Invalid firmware size (max 2MB)");
    }

    const esp_partition_t *update_partition = esp_ota_get_next_update_partition(NULL);
    if (!update_partition) {
        return send_error(req, 500, "No OTA partition available");
    }

    ESP_LOGI(TAG, "Writing to partition '%s' at 0x%lx, size %lu",
             update_partition->label, (unsigned long)update_partition->address,
             (unsigned long)update_partition->size);

    esp_ota_handle_t ota_handle;
    esp_err_t err = esp_ota_begin(update_partition, OTA_WITH_SEQUENTIAL_WRITES, &ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_begin failed: %s", esp_err_to_name(err));
        return send_error(req, 500, esp_err_to_name(err));
    }

    // Stream firmware data in chunks
    char *buf = (char*)malloc(4096);
    if (!buf) {
        esp_ota_abort(ota_handle);
        return send_error(req, 500, "Out of memory");
    }

    int remaining = req->content_len;
    int total_written = 0;
    bool failed = false;

    while (remaining > 0) {
        int to_read = (remaining > 4096) ? 4096 : remaining;
        int received = httpd_req_recv(req, buf, to_read);
        if (received <= 0) {
            if (received == HTTPD_SOCK_ERR_TIMEOUT) continue;
            ESP_LOGE(TAG, "OTA receive error at %d/%d bytes", total_written, req->content_len);
            failed = true;
            break;
        }

        err = esp_ota_write(ota_handle, buf, received);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "OTA write failed at %d bytes: %s", total_written, esp_err_to_name(err));
            failed = true;
            break;
        }

        remaining -= received;
        total_written += received;

        // Log progress every 64KB
        if (total_written % (64 * 1024) < 4096) {
            ESP_LOGI(TAG, "OTA progress: %d / %d bytes (%d%%)",
                     total_written, req->content_len,
                     total_written * 100 / req->content_len);
        }
    }

    free(buf);

    if (failed) {
        esp_ota_abort(ota_handle);
        return send_error(req, 500, "OTA write failed");
    }

    err = esp_ota_end(ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "OTA end failed: %s", esp_err_to_name(err));
        return send_error(req, 500, esp_err_to_name(err));
    }

    err = esp_ota_set_boot_partition(update_partition);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Set boot partition failed: %s", esp_err_to_name(err));
        return send_error(req, 500, esp_err_to_name(err));
    }

    ESP_LOGI(TAG, "OTA success! %d bytes written to '%s'. Rebooting...",
             total_written, update_partition->label);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "success", true);
    cJSON_AddNumberToObject(root, "bytesWritten", total_written);
    cJSON_AddStringToObject(root, "partition", update_partition->label);
    esp_err_t ret = send_json(req, root);

    // Reboot after a short delay to let the HTTP response flush
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();

    return ret;
}

// =============================================================================
// Board profile + pairing handlers
// =============================================================================

// GET /api/board → { active: string|null, available: [ BoardProfile... ] }
static esp_err_t handle_get_board(httpd_req_t *req)
{
    cJSON *root = cJSON_CreateObject();

    const BoardProfile *active = board_profile_get_active();
    if (active) {
        cJSON_AddStringToObject(root, "active", active->id);
    } else {
        cJSON_AddNullToObject(root, "active");
    }

    cJSON *arr = cJSON_AddArrayToObject(root, "available");
    for (uint8_t i = 0; i < board_profile_count(); i++) {
        const BoardProfile *p = board_profile_at(i);
        if (!p) continue;
        cJSON *obj = cJSON_CreateObject();
        cJSON_AddStringToObject(obj, "id", p->id);
        cJSON_AddStringToObject(obj, "name", p->name);
        cJSON_AddStringToObject(obj, "description", p->description);
        cJSON *rails = cJSON_AddObjectToObject(obj, "rails");
        cJSON *vlogic = cJSON_AddObjectToObject(rails, "vlogic");
        cJSON_AddNumberToObject(vlogic, "value", p->vlogic);
        cJSON_AddBoolToObject(vlogic, "locked", p->vlogicLocked);
        cJSON *vadj1 = cJSON_AddObjectToObject(rails, "vadj1");
        cJSON_AddNumberToObject(vadj1, "value", p->vadj1);
        cJSON_AddBoolToObject(vadj1, "locked", p->vadj1Locked);
        cJSON *vadj2 = cJSON_AddObjectToObject(rails, "vadj2");
        cJSON_AddNumberToObject(vadj2, "value", p->vadj2);
        cJSON_AddBoolToObject(vadj2, "locked", p->vadj2Locked);
        cJSON_AddNumberToObject(obj, "pinCount", p->pinCount);
        cJSON_AddItemToArray(arr, obj);
    }

    return send_json(req, root);
}

// POST /api/board/select  { "boardId": "..." }
static esp_err_t handle_post_board_select(httpd_req_t *req)
{
    if (check_admin_auth(req) != ESP_OK) {
        return send_error(req, 401, "Unauthorized");
    }

    cJSON *body = recv_json_body(req);
    if (!body) return send_error(req, 400, "Invalid JSON body");

    cJSON *idItem = cJSON_GetObjectItem(body, "boardId");
    if (!idItem || !cJSON_IsString(idItem)) {
        cJSON_Delete(body);
        return send_error(req, 400, "Missing boardId");
    }

    const char *id = idItem->valuestring;
    bool ok = board_profile_select(id);
    cJSON_Delete(body);

    if (!ok) {
        return send_error(req, 404, "Unknown boardId");
    }

    cJSON *root = cJSON_CreateObject();
    const BoardProfile *active = board_profile_get_active();
    cJSON_AddStringToObject(root, "active", active ? active->id : "");
    cJSON_AddBoolToObject(root, "ok", true);
    return send_json(req, root);
}

// GET /api/pairing/info → { macAddress, tokenFingerprint, transport: "http" }
// Never exposes the token itself — only sha256(token)[:8] hex for UX.
static esp_err_t handle_get_pairing_info(httpd_req_t *req)
{
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    char macStr[18];
    snprintf(macStr, sizeof(macStr), "%02x:%02x:%02x:%02x:%02x:%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    char fingerprint[17] = {0};
    bool haveFp = auth_token_fingerprint(fingerprint);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "macAddress", macStr);
    if (haveFp) {
        cJSON_AddStringToObject(root, "tokenFingerprint", fingerprint);
    } else {
        cJSON_AddNullToObject(root, "tokenFingerprint");
    }
    cJSON_AddStringToObject(root, "transport", "http");
    return send_json(req, root);
}

// POST /api/pairing/verify  (admin-token header) → 200 ok, 401 otherwise.
// Lightweight endpoint for the web UI to confirm a pasted token before
// caching it in localStorage.
static esp_err_t handle_post_pairing_verify(httpd_req_t *req)
{
    if (check_admin_auth(req) != ESP_OK) {
        return send_error(req, 401, "Unauthorized");
    }
    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", true);
    return send_json(req, root);
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
    config.max_uri_handlers = 64;
    config.uri_match_fn     = httpd_uri_match_wildcard;
    config.stack_size       = 12288;  // Increased for OTA buffer

    esp_err_t ret = httpd_start(&s_server, &config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start HTTP server: %s", esp_err_to_name(ret));
        return;
    }

    ESP_LOGI(TAG, "HTTP server started on port %d", config.server_port);
    httpd_register_err_handler(s_server, HTTPD_404_NOT_FOUND, handle_http_error);
    httpd_register_err_handler(s_server, HTTPD_405_METHOD_NOT_ALLOWED, handle_http_error);

    // ----- GET routes -----

    httpd_uri_t uri_root = {
        .uri = "/", .method = HTTP_GET, .handler = handle_root, .user_ctx = NULL
    };
    httpd_register_uri_handler(s_server, &uri_root);

    httpd_uri_t uri_assets = {
        .uri = "/assets/*", .method = HTTP_GET, .handler = handle_static_asset, .user_ctx = NULL
    };
    httpd_register_uri_handler(s_server, &uri_assets);

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

    // ----- Digital IO (ESP32 GPIO) routes -----

    httpd_uri_t uri_dio_get_all = {
        .uri = "/api/dio", .method = HTTP_GET, .handler = handle_get_dio, .user_ctx = NULL
    };
    httpd_register_uri_handler(s_server, &uri_dio_get_all);

    httpd_uri_t uri_dio_get_single = {
        .uri = "/api/dio/*", .method = HTTP_GET, .handler = handle_get_dio_single, .user_ctx = NULL
    };
    httpd_register_uri_handler(s_server, &uri_dio_get_single);

    httpd_uri_t uri_dio_post = {
        .uri = "/api/dio/*", .method = HTTP_POST, .handler = handle_dio_post_dispatch, .user_ctx = NULL
    };
    httpd_register_uri_handler(s_server, &uri_dio_post);

    // ----- Self-Test / Calibration routes -----

    httpd_uri_t uri_selftest_get = {
        .uri = "/api/selftest", .method = HTTP_GET, .handler = handle_get_selftest, .user_ctx = NULL
    };
    httpd_register_uri_handler(s_server, &uri_selftest_get);

    httpd_uri_t uri_selftest_supply = {
        .uri = "/api/selftest/supply/*", .method = HTTP_GET, .handler = handle_get_selftest_supply, .user_ctx = NULL
    };
    httpd_register_uri_handler(s_server, &uri_selftest_supply);

    httpd_uri_t uri_selftest_efuse = {
        .uri = "/api/selftest/efuse", .method = HTTP_GET, .handler = handle_get_selftest_efuse, .user_ctx = NULL
    };
    httpd_register_uri_handler(s_server, &uri_selftest_efuse);

    httpd_uri_t uri_selftest_cal = {
        .uri = "/api/selftest/calibrate", .method = HTTP_POST, .handler = handle_post_selftest_calibrate, .user_ctx = NULL
    };
    httpd_register_uri_handler(s_server, &uri_selftest_cal);

    httpd_uri_t uri_selftest_supplies = {
        .uri = "/api/selftest/supplies", .method = HTTP_GET, .handler = handle_get_selftest_supplies, .user_ctx = NULL
    };
    httpd_register_uri_handler(s_server, &uri_selftest_supplies);

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

    // ----- I2C Device routes -----

    httpd_uri_t uri_idac_get = {
        .uri = "/api/idac", .method = HTTP_GET, .handler = handle_get_idac, .user_ctx = NULL
    };
    httpd_register_uri_handler(s_server, &uri_idac_get);

    httpd_uri_t uri_idac_post = {
        .uri = "/api/idac/*", .method = HTTP_POST, .handler = handle_idac_post_dispatch, .user_ctx = NULL
    };
    httpd_register_uri_handler(s_server, &uri_idac_post);

    httpd_uri_t uri_usbpd_get = {
        .uri = "/api/usbpd", .method = HTTP_GET, .handler = handle_get_usbpd, .user_ctx = NULL
    };
    httpd_register_uri_handler(s_server, &uri_usbpd_get);

    httpd_uri_t uri_usbpd_post = {
        .uri = "/api/usbpd/*", .method = HTTP_POST, .handler = handle_usbpd_post_dispatch, .user_ctx = NULL
    };
    httpd_register_uri_handler(s_server, &uri_usbpd_post);

    httpd_uri_t uri_ioexp_get = {
        .uri = "/api/ioexp", .method = HTTP_GET, .handler = handle_get_ioexp, .user_ctx = NULL
    };
    httpd_register_uri_handler(s_server, &uri_ioexp_get);

    httpd_uri_t uri_ioexp_faults_get = {
        .uri = "/api/ioexp/faults", .method = HTTP_GET, .handler = handle_get_ioexp_faults, .user_ctx = NULL
    };
    httpd_register_uri_handler(s_server, &uri_ioexp_faults_get);

    httpd_uri_t uri_ioexp_post = {
        .uri = "/api/ioexp/*", .method = HTTP_POST, .handler = handle_ioexp_post_dispatch, .user_ctx = NULL
    };
    httpd_register_uri_handler(s_server, &uri_ioexp_post);

    // ----- HAT routes -----

    httpd_uri_t uri_hat_get = {
        .uri = "/api/hat", .method = HTTP_GET, .handler = handle_get_hat, .user_ctx = NULL
    };
    httpd_register_uri_handler(s_server, &uri_hat_get);

    httpd_uri_t uri_hat_la_status_get = {
        .uri = "/api/hat/la/status", .method = HTTP_GET, .handler = handle_get_hat_la_status, .user_ctx = NULL
    };
    httpd_register_uri_handler(s_server, &uri_hat_la_status_get);

    httpd_uri_t uri_hat_post = {
        .uri = "/api/hat/*", .method = HTTP_POST, .handler = handle_hat_post_dispatch, .user_ctx = NULL
    };
    httpd_register_uri_handler(s_server, &uri_hat_post);

    httpd_uri_t uri_debug_get = {
        .uri = "/api/debug", .method = HTTP_GET, .handler = handle_get_debug, .user_ctx = NULL
    };
    httpd_register_uri_handler(s_server, &uri_debug_get);

    // ----- MUX routes -----

    httpd_uri_t uri_mux_get = {
        .uri = "/api/mux", .method = HTTP_GET, .handler = handle_get_mux, .user_ctx = NULL
    };
    httpd_register_uri_handler(s_server, &uri_mux_get);

    httpd_uri_t uri_mux_post = {
        .uri = "/api/mux/*", .method = HTTP_POST, .handler = handle_mux_post_dispatch, .user_ctx = NULL
    };
    httpd_register_uri_handler(s_server, &uri_mux_post);

    // ----- IDAC Calibration routes -----

    httpd_uri_t uri_cal_post = {
        .uri = "/api/idac/cal/*", .method = HTTP_POST, .handler = handle_cal_post_dispatch, .user_ctx = NULL
    };
    httpd_register_uri_handler(s_server, &uri_cal_post);

    // ----- Level Shifter OE -----

    httpd_uri_t uri_lshift = {
        .uri = "/api/lshift/oe", .method = HTTP_POST, .handler = handle_post_lshift_oe, .user_ctx = NULL
    };
    httpd_register_uri_handler(s_server, &uri_lshift);

    // ----- Wavegen routes -----

    httpd_uri_t uri_wavegen_post = {
        .uri = "/api/wavegen/*", .method = HTTP_POST, .handler = handle_wavegen_post_dispatch, .user_ctx = NULL
    };
    httpd_register_uri_handler(s_server, &uri_wavegen_post);

    // ----- WiFi routes -----

    httpd_uri_t uri_wifi_get = {
        .uri = "/api/wifi", .method = HTTP_GET, .handler = handle_get_wifi, .user_ctx = NULL
    };
    httpd_register_uri_handler(s_server, &uri_wifi_get);

    httpd_uri_t uri_wifi_connect = {
        .uri = "/api/wifi/connect", .method = HTTP_POST, .handler = handle_post_wifi_connect, .user_ctx = NULL
    };
    httpd_register_uri_handler(s_server, &uri_wifi_connect);

    httpd_uri_t uri_wifi_scan = {
        .uri = "/api/wifi/scan", .method = HTTP_GET, .handler = handle_get_wifi_scan, .user_ctx = NULL
    };
    httpd_register_uri_handler(s_server, &uri_wifi_scan);

    // ----- OTA / Version routes -----

    httpd_uri_t uri_version = {
        .uri = "/api/device/version", .method = HTTP_GET, .handler = handle_get_version, .user_ctx = NULL
    };
    httpd_register_uri_handler(s_server, &uri_version);

    httpd_uri_t uri_ota = {
        .uri = "/api/ota/upload", .method = HTTP_POST, .handler = handle_ota_upload, .user_ctx = NULL
    };
    httpd_register_uri_handler(s_server, &uri_ota);

    // ----- Board profile + pairing -----

    httpd_uri_t uri_board_get = {
        .uri = "/api/board", .method = HTTP_GET, .handler = handle_get_board, .user_ctx = NULL
    };
    httpd_register_uri_handler(s_server, &uri_board_get);

    httpd_uri_t uri_board_select = {
        .uri = "/api/board/select", .method = HTTP_POST, .handler = handle_post_board_select, .user_ctx = NULL
    };
    httpd_register_uri_handler(s_server, &uri_board_select);

    httpd_uri_t uri_pairing_info = {
        .uri = "/api/pairing/info", .method = HTTP_GET, .handler = handle_get_pairing_info, .user_ctx = NULL
    };
    httpd_register_uri_handler(s_server, &uri_pairing_info);

    httpd_uri_t uri_pairing_verify = {
        .uri = "/api/pairing/verify", .method = HTTP_POST, .handler = handle_post_pairing_verify, .user_ctx = NULL
    };
    httpd_register_uri_handler(s_server, &uri_pairing_verify);

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
