// =============================================================================
// http_adapter.cpp — HTTP transport adapter for the command registry
//
// Each registry-backed route shares one generic POST/GET handler that:
//   1. Reads the JSON body (POST) or query string (GET)
//   2. Calls the registry handler with a pre-built binary payload
//   3. Decodes the binary response back into JSON
//
// For Slice 2 (DAC only) the payload encoding matches the BBP wire format
// exactly so the same handlers serve both transports.
// =============================================================================
#include "http_adapter.h"
#include "cmd_registry.h"
#include "cmd_errors.h"
#include "bbp_codec.h"
#include "bbp.h"
#include "esp_log.h"
#include "cJSON.h"
#include <string.h>
#include <stdlib.h>

static const char *TAG = "http_adapter";

// ---------------------------------------------------------------------------
// Helpers shared with webserver.cpp (duplicated locally to keep adapter
// self-contained — these are tiny one-liners)
// ---------------------------------------------------------------------------

// Apply the same localhost-only CORS policy as webserver.cpp set_cors_headers().
// origin_buf must be a caller-frame buffer that outlives the subsequent
// httpd_resp_send*() call (the IDF stores the pointer, not a copy).
static void set_cors_headers_local(httpd_req_t *req, char *origin_buf, size_t origin_buf_size)
{
    origin_buf[0] = '\0';
    if (httpd_req_get_hdr_value_str(req, "Origin", origin_buf, origin_buf_size) == ESP_OK) {
        if (strncmp(origin_buf, "http://localhost", 16) == 0 ||
            strncmp(origin_buf, "http://127.0.0.1", 16) == 0) {
            httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", origin_buf);
            httpd_resp_set_hdr(req, "Vary", "Origin");
        }
    }
    httpd_resp_set_hdr(req, "Access-Control-Allow-Methods", "GET, POST, OPTIONS");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Headers", "Content-Type");
}

static esp_err_t send_json_resp(httpd_req_t *req, cJSON *root)
{
    char *str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!str) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, "application/json");
    char origin_buf[96];
    set_cors_headers_local(req, origin_buf, sizeof(origin_buf));
    esp_err_t ret = httpd_resp_sendstr(req, str);
    free(str);
    return ret;
}

static esp_err_t send_err_resp(httpd_req_t *req, int status, const char *msg)
{
    char body[128];
    snprintf(body, sizeof(body), "{\"error\":\"%s\"}", msg);
    httpd_resp_set_type(req, "application/json");
    char origin_buf[96];
    set_cors_headers_local(req, origin_buf, sizeof(origin_buf));
    httpd_resp_set_status(req, status == 400 ? "400 Bad Request"
                                             : "500 Internal Server Error");
    return httpd_resp_sendstr(req, body);
}

static cJSON *recv_json(httpd_req_t *req)
{
    int total = req->content_len;
    if (total <= 0 || total > 4096) return NULL;
    char *buf = (char *)malloc(total + 1);
    if (!buf) return NULL;
    int received = 0;
    while (received < total) {
        int r = httpd_req_recv(req, buf + received, total - received);
        if (r <= 0) { free(buf); return NULL; }
        received += r;
    }
    buf[total] = '\0';
    cJSON *doc = cJSON_Parse(buf);
    free(buf);
    return doc;
}

// ---------------------------------------------------------------------------
// POST /api/registry/set_dac_code
// JSON body: {"channel": N, "code": N}
// ---------------------------------------------------------------------------
static esp_err_t handle_set_dac_code(httpd_req_t *req)
{
    cJSON *doc = recv_json(req);
    if (!doc) return send_err_resp(req, 400, "Invalid JSON body");

    cJSON *jch   = cJSON_GetObjectItem(doc, "channel");
    cJSON *jcode = cJSON_GetObjectItem(doc, "code");
    if (!cJSON_IsNumber(jch) || !cJSON_IsNumber(jcode)) {
        cJSON_Delete(doc);
        return send_err_resp(req, 400, "Missing 'channel' or 'code'");
    }
    uint8_t  ch   = (uint8_t)jch->valueint;
    uint16_t code = (uint16_t)jcode->valueint;
    cJSON_Delete(doc);

    // Build binary payload
    uint8_t payload[3]; size_t ppos = 0;
    bbp_put_u8(payload, &ppos, ch);
    bbp_put_u16(payload, &ppos, code);

    uint8_t rsp[64]; size_t rsp_len = 0;
    const CmdDescriptor *d = cmd_registry_lookup_opcode(BBP_CMD_SET_DAC_CODE);
    if (!d) return send_err_resp(req, 500, "Command not registered");
    int rc = d->handler(payload, ppos, rsp, &rsp_len);
    if (rc < 0) return send_err_resp(req, 400, cmd_error_str((CmdError)(-rc)));

    cJSON *resp = cJSON_CreateObject();
    cJSON_AddBoolToObject(resp, "ok", true);
    cJSON_AddNumberToObject(resp, "channel", ch);
    cJSON_AddNumberToObject(resp, "code", code);
    return send_json_resp(req, resp);
}

// ---------------------------------------------------------------------------
// POST /api/registry/set_dac_voltage
// JSON body: {"channel": N, "voltage": F, "bipolar": bool}
// ---------------------------------------------------------------------------
static esp_err_t handle_set_dac_voltage(httpd_req_t *req)
{
    cJSON *doc = recv_json(req);
    if (!doc) return send_err_resp(req, 400, "Invalid JSON body");

    cJSON *jch   = cJSON_GetObjectItem(doc, "channel");
    cJSON *jv    = cJSON_GetObjectItem(doc, "voltage");
    cJSON *jbip  = cJSON_GetObjectItem(doc, "bipolar");
    if (!cJSON_IsNumber(jch) || !cJSON_IsNumber(jv)) {
        cJSON_Delete(doc);
        return send_err_resp(req, 400, "Missing 'channel' or 'voltage'");
    }
    uint8_t ch      = (uint8_t)jch->valueint;
    float   voltage = (float)jv->valuedouble;
    bool    bipolar = jbip ? cJSON_IsTrue(jbip) : false;
    cJSON_Delete(doc);

    uint8_t payload[6]; size_t ppos = 0;
    bbp_put_u8(payload, &ppos, ch);
    bbp_put_f32(payload, &ppos, voltage);
    bbp_put_bool(payload, &ppos, bipolar);

    uint8_t rsp[64]; size_t rsp_len = 0;
    const CmdDescriptor *d = cmd_registry_lookup_opcode(BBP_CMD_SET_DAC_VOLTAGE);
    if (!d) return send_err_resp(req, 500, "Command not registered");
    int rc = d->handler(payload, ppos, rsp, &rsp_len);
    if (rc < 0) return send_err_resp(req, 400, cmd_error_str((CmdError)(-rc)));

    cJSON *resp = cJSON_CreateObject();
    cJSON_AddBoolToObject(resp, "ok", true);
    cJSON_AddNumberToObject(resp, "channel", ch);
    cJSON_AddNumberToObject(resp, "voltage", (double)voltage);
    cJSON_AddBoolToObject(resp, "bipolar", bipolar);
    return send_json_resp(req, resp);
}

// ---------------------------------------------------------------------------
// POST /api/registry/set_dac_current
// JSON body: {"channel": N, "current_mA": F}
// ---------------------------------------------------------------------------
static esp_err_t handle_set_dac_current(httpd_req_t *req)
{
    cJSON *doc = recv_json(req);
    if (!doc) return send_err_resp(req, 400, "Invalid JSON body");

    cJSON *jch = cJSON_GetObjectItem(doc, "channel");
    cJSON *jmA = cJSON_GetObjectItem(doc, "current_mA");
    if (!cJSON_IsNumber(jch) || !cJSON_IsNumber(jmA)) {
        cJSON_Delete(doc);
        return send_err_resp(req, 400, "Missing 'channel' or 'current_mA'");
    }
    uint8_t ch         = (uint8_t)jch->valueint;
    float   current_mA = (float)jmA->valuedouble;
    cJSON_Delete(doc);

    uint8_t payload[5]; size_t ppos = 0;
    bbp_put_u8(payload, &ppos, ch);
    bbp_put_f32(payload, &ppos, current_mA);

    uint8_t rsp[64]; size_t rsp_len = 0;
    const CmdDescriptor *d = cmd_registry_lookup_opcode(BBP_CMD_SET_DAC_CURRENT);
    if (!d) return send_err_resp(req, 500, "Command not registered");
    int rc = d->handler(payload, ppos, rsp, &rsp_len);
    if (rc < 0) return send_err_resp(req, 400, cmd_error_str((CmdError)(-rc)));

    cJSON *resp = cJSON_CreateObject();
    cJSON_AddBoolToObject(resp, "ok", true);
    cJSON_AddNumberToObject(resp, "channel", ch);
    cJSON_AddNumberToObject(resp, "current_mA", (double)current_mA);
    return send_json_resp(req, resp);
}

// ---------------------------------------------------------------------------
// GET /api/registry/get_dac_readback?channel=N
// ---------------------------------------------------------------------------
static esp_err_t handle_get_dac_readback(httpd_req_t *req)
{
    char qs[32] = {};
    if (httpd_req_get_url_query_str(req, qs, sizeof(qs)) != ESP_OK) {
        return send_err_resp(req, 400, "Missing query string");
    }
    char ch_str[8] = {};
    if (httpd_query_key_value(qs, "channel", ch_str, sizeof(ch_str)) != ESP_OK) {
        return send_err_resp(req, 400, "Missing 'channel' query param");
    }
    uint8_t ch = (uint8_t)atoi(ch_str);

    uint8_t payload[1]; size_t ppos = 0;
    bbp_put_u8(payload, &ppos, ch);

    uint8_t rsp[16]; size_t rsp_len = 0;
    const CmdDescriptor *d = cmd_registry_lookup_opcode(BBP_CMD_GET_DAC_READBACK);
    if (!d) return send_err_resp(req, 500, "Command not registered");
    int rc = d->handler(payload, ppos, rsp, &rsp_len);
    if (rc < 0) return send_err_resp(req, 400, cmd_error_str((CmdError)(-rc)));

    // Decode response: u8 ch, u16 activeCode
    size_t rpos = 0;
    uint8_t  rch  = bbp_get_u8(rsp, &rpos);
    uint16_t code = bbp_get_u16(rsp, &rpos);

    cJSON *resp = cJSON_CreateObject();
    cJSON_AddNumberToObject(resp, "channel", rch);
    cJSON_AddNumberToObject(resp, "activeCode", (int)code);
    cJSON_AddNumberToObject(resp, "code", (int)code);
    return send_json_resp(req, resp);
}

// ---------------------------------------------------------------------------
// Public: register all adapter routes
// ---------------------------------------------------------------------------
void http_adapter_register(httpd_handle_t server)
{
    static const httpd_uri_t routes[] = {
        { "/api/registry/set_dac_code",     HTTP_POST, handle_set_dac_code,     NULL },
        { "/api/registry/set_dac_voltage",  HTTP_POST, handle_set_dac_voltage,  NULL },
        { "/api/registry/set_dac_current",  HTTP_POST, handle_set_dac_current,  NULL },
        { "/api/registry/get_dac_readback", HTTP_GET,  handle_get_dac_readback, NULL },
    };
    for (size_t i = 0; i < sizeof(routes)/sizeof(routes[0]); i++) {
        esp_err_t ret = httpd_register_uri_handler(server, &routes[i]);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to register %s: %s", routes[i].uri, esp_err_to_name(ret));
        }
    }
    ESP_LOGI(TAG, "Registered %u registry routes", (unsigned)(sizeof(routes)/sizeof(routes[0])));
}
