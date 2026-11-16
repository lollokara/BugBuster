// =============================================================================
// webserver_hat.cpp - HTTP handlers: HAT expansion board.
// Routes are registered in webserver.cpp (initWebServer).
// =============================================================================

#include "webserver_internal.h"

// =============================================================================
// HAT Expansion Board API
// =============================================================================

// GET /api/hat - Get HAT status
esp_err_t handle_get_hat(httpd_req_t *req)
{
    char *resp = api_core_handle("GET", "/api/hat", NULL);
    if (!resp) return send_error(req, 500, "hat failed");
    esp_err_t rc = send_raw_json(req, resp);
    cJSON_free(resp);
    return rc;
}

// POST /api/daq/wifi_stream/start
esp_err_t handle_post_daq_wifi_stream_start(httpd_req_t *req)
{
    if (check_admin_auth(req) != ESP_OK) return send_error(req, 401, "Admin token required");
    char *resp = api_core_handle("POST", "/api/daq/wifi_stream/start", NULL);
    if (!resp) return send_error(req, 500, "wifi stream start failed");
    esp_err_t rc = send_raw_json(req, resp);
    cJSON_free(resp);
    return rc;
}

// POST /api/daq/wifi_stream/stop
esp_err_t handle_post_daq_wifi_stream_stop(httpd_req_t *req)
{
    if (check_admin_auth(req) != ESP_OK) return send_error(req, 401, "Admin token required");
    char *resp = api_core_handle("POST", "/api/daq/wifi_stream/stop", NULL);
    if (!resp) return send_error(req, 500, "wifi stream stop failed");
    esp_err_t rc = send_raw_json(req, resp);
    cJSON_free(resp);
    return rc;
}

// POST /api/daq/wifi_stream/recycle
esp_err_t handle_post_daq_wifi_stream_recycle(httpd_req_t *req)
{
    if (check_admin_auth(req) != ESP_OK) return send_error(req, 401, "Admin token required");
    char *resp = api_core_handle("POST", "/api/daq/wifi_stream/recycle", NULL);
    if (!resp) return send_error(req, 500, "wifi stream recycle failed");
    esp_err_t rc = send_raw_json(req, resp);
    cJSON_free(resp);
    return rc;
}

// GET /api/daq/wifi_stream/status
esp_err_t handle_get_daq_wifi_stream_status(httpd_req_t *req)
{
    char *resp = api_core_handle("GET", "/api/daq/wifi_stream/status", NULL);
    if (!resp) return send_error(req, 500, "wifi stream status failed");
    if (check_admin_auth(req) != ESP_OK) {
        // api_core is shared with BLE (paired channel); only HTTP strips the hotspot credentials.
        cJSON *root = cJSON_Parse(resp);
        cJSON_free(resp);
        if (!root) return send_error(req, 500, "wifi stream status failed");
        cJSON_DeleteItemFromObject(root, "ssid");
        cJSON_DeleteItemFromObject(root, "password");
        return send_json(req, root);
    }
    esp_err_t rc = send_raw_json(req, resp);
    cJSON_free(resp);
    return rc;
}

// GET /api/daq - top-level DAQ status. api_core_handle() already dispatches
// this path (used by BLE), but no httpd_uri_t was ever registered for it, so
// HTTP callers got a 404. Mirror /api/daq/vdut/status: read-only, no admin
// auth required.
esp_err_t handle_get_daq(httpd_req_t *req)
{
    char *resp = api_core_handle("GET", "/api/daq", NULL);
    if (!resp) return send_error(req, 500, "daq status failed");
    esp_err_t rc = send_raw_json(req, resp);
    cJSON_free(resp);
    return rc;
}

// GET /api/daq/vdut/status
esp_err_t handle_get_daq_vdut_status(httpd_req_t *req)
{
    char *resp = api_core_handle("GET", "/api/daq/vdut/status", NULL);
    if (!resp) return send_error(req, 500, "vdut status failed");
    esp_err_t rc = send_raw_json(req, resp);
    cJSON_free(resp);
    return rc;
}

// api_core_handle() error responses are just {"error": "..."} bodies (its
// callers, e.g. api_error(), have no notion of HTTP status). The VDUT
// setpoint/enable contract requires a non-2xx reject for invalid requests
// (see ConnectionManager.swift's doc comment above vdutPresent), so translate
// an "error" key in the JSON body into HTTP 400 here rather than always 200.
esp_err_t send_api_core_result(httpd_req_t *req, char *resp, const char *fail_msg)
{
    if (!resp) return send_error(req, 500, fail_msg);
    cJSON *parsed = cJSON_Parse(resp);
    bool is_error = parsed && cJSON_GetObjectItem(parsed, "error") != NULL;
    // A battery-sim run owning VDUT is a state conflict, not a bad request.
    bool owned = is_error && cJSON_GetObjectItem(parsed, "runId") != NULL;
    esp_err_t rc = send_raw_json(req, resp, owned ? 409 : is_error ? 400 : 200);
    if (parsed) cJSON_Delete(parsed);
    cJSON_free(resp);
    return rc;
}

// POST /api/daq/vdut/enable  body: {"enabled": bool}
esp_err_t handle_post_daq_vdut_enable(httpd_req_t *req)
{
    if (check_admin_auth(req) != ESP_OK) return send_error(req, 401, "Admin token required");
    cJSON *body = recv_json_body(req);
    char *resp = api_core_handle("POST", "/api/daq/vdut/enable", body);
    if (body) cJSON_Delete(body);
    return send_api_core_result(req, resp, "vdut enable failed");
}

// POST /api/daq/vdut/setpoint  body: {"voltageV": number, "currentLimitMa": number}
esp_err_t handle_post_daq_vdut_setpoint(httpd_req_t *req)
{
    if (check_admin_auth(req) != ESP_OK) return send_error(req, 401, "Admin token required");
    cJSON *body = recv_json_body(req);
    char *resp = api_core_handle("POST", "/api/daq/vdut/setpoint", body);
    if (body) cJSON_Delete(body);
    return send_api_core_result(req, resp, "vdut setpoint failed");
}

// POST /api/daq/acq_config  body: {"filter": u8, "adc_dec": u8}
esp_err_t handle_post_daq_acq_config(httpd_req_t *req)
{
    if (check_admin_auth(req) != ESP_OK) return send_error(req, 401, "Admin token required");
    cJSON *body = recv_json_body(req);
    char *resp = api_core_handle("POST", "/api/daq/acq_config", body);
    if (body) cJSON_Delete(body);
    return send_api_core_result(req, resp, "acq config failed");
}

// POST /api/daq/bs, /api/daq/bs/read - battery simulator status/list/read.
// Read-only (SET_EPOCH only stamps the host clock), so no admin token.
esp_err_t handle_post_daq_bs(httpd_req_t *req)
{
    cJSON *body = recv_json_body(req);
    char *resp = api_core_handle("POST", "/api/daq/bs", body);
    if (body) cJSON_Delete(body);
    return send_api_core_result(req, resp, "battsim request failed");
}

esp_err_t handle_post_daq_bs_read(httpd_req_t *req)
{
    cJSON *body = recv_json_body(req);
    char *resp = api_core_handle("POST", "/api/daq/bs/read", body);
    if (body) cJSON_Delete(body);
    return send_api_core_result(req, resp, "battsim read failed");
}

// GET /api/hub/status - hub streaming counters (no secrets, no admin token).
// GET/POST /api/hub/config and POST /api/hub/resync change or reveal the hub URL: admin only.
esp_err_t handle_get_hub_status(httpd_req_t *req)
{
    return send_api_core_result(req, api_core_handle("GET", "/api/hub/status", NULL), "hub status failed");
}

esp_err_t handle_get_hub_config(httpd_req_t *req)
{
    if (check_admin_auth(req) != ESP_OK) return send_error(req, 401, "Admin token required");
    return send_api_core_result(req, api_core_handle("GET", "/api/hub/config", NULL), "hub config failed");
}

esp_err_t handle_post_hub_config(httpd_req_t *req)
{
    if (check_admin_auth(req) != ESP_OK) return send_error(req, 401, "Admin token required");
    cJSON *body = recv_json_body(req);
    if (!body) return send_error(req, 400, "Invalid JSON");
    char *resp = api_core_handle("POST", "/api/hub/config", body);
    cJSON_Delete(body);
    return send_api_core_result(req, resp, "hub config failed");
}

esp_err_t handle_post_hub_resync(httpd_req_t *req)
{
    if (check_admin_auth(req) != ESP_OK) return send_error(req, 401, "Admin token required");
    return send_api_core_result(req, api_core_handle("POST", "/api/hub/resync", NULL), "hub resync failed");
}


// =============================================================================
// HAT v2 route handlers
// =============================================================================

// GET /api/hat/v2/caps
esp_err_t handle_get_hat_v2_caps(httpd_req_t *req)
{
    if (!hat_detected()) {
        return send_error(req, 404, "HAT not detected");
    }

    const HatState *hs = hat_get_state();
    if (!hs->caps_valid) {
        // Try to refresh caps from the HAT
        HatCaps caps = {};
        if (!hat_get_caps(&caps)) {
            return send_error(req, 503, "HAT not responding");
        }
    }

    const HatCaps *caps = &hs->caps;
    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "hwRevision", caps->hw_revision);
    cJSON_AddNumberToObject(root, "flags", caps->flags);
    cJSON_AddNumberToObject(root, "railCount", caps->rail_count);
    cJSON_AddNumberToObject(root, "ledCount", caps->led_count);
    cJSON_AddNumberToObject(root, "shiftedIoCount", caps->shifted_io_count);
    cJSON_AddNumberToObject(root, "laRouteCount", caps->la_routes);
    cJSON_AddNumberToObject(root, "fwMajor", caps->fw_major);
    cJSON_AddNumberToObject(root, "fwMinor", caps->fw_minor);
    return send_json(req, root);
}

// GET /api/hat/v2/rails
esp_err_t handle_get_hat_v2_rails(httpd_req_t *req)
{
    char *resp = api_core_handle("GET", "/api/hat/v2/rails", NULL);
    if (!resp) return send_error(req, 500, "hat rails failed");
    esp_err_t rc = send_raw_json(req, resp);
    cJSON_free(resp);
    return rc;
}

// GET /api/hat/calibration[?rail=N] — RP2040 HAT calibration snapshot + points.
esp_err_t handle_get_hat_calibration(httpd_req_t *req)
{
    char *resp = api_core_handle("GET", req->uri, NULL);
    if (!resp) return send_error(req, 500, "hat calibration failed");
    esp_err_t rc = send_raw_json(req, resp);
    cJSON_free(resp);
    return rc;
}

// POST /api/hat/v2/rail/enable
esp_err_t handle_post_hat_v2_rail_enable(httpd_req_t *req)
{
    if (check_admin_auth(req) != ESP_OK) return send_error(req, 401, "Admin token required");
    cJSON *body = recv_json_body(req);
    char *resp = api_core_handle("POST", "/api/hat/v2/rail/enable", body);
    if (body) cJSON_Delete(body);
    if (!resp) return send_error(req, 500, "rail enable failed");
    esp_err_t rc = send_raw_json(req, resp);
    cJSON_free(resp);
    return rc;
}

// POST /api/hat/v2/rail/voltage
esp_err_t handle_post_hat_v2_rail_voltage(httpd_req_t *req)
{
    if (check_admin_auth(req) != ESP_OK) return send_error(req, 401, "Admin token required");
    cJSON *body = recv_json_body(req);
    char *resp = api_core_handle("POST", "/api/hat/v2/rail/voltage", body);
    if (body) cJSON_Delete(body);
    if (!resp) return send_error(req, 500, "rail voltage failed");
    esp_err_t rc = send_raw_json(req, resp);
    cJSON_free(resp);
    return rc;
}

// GET /api/hat/power
esp_err_t handle_get_hat_power(httpd_req_t *req)
{
    if (!hat_detected()) {
        return send_error(req, 404, "HAT not detected");
    }
    const HatState *hs = hat_get_state();
    cJSON *root = cJSON_CreateObject();
    for (int i = 0; i < 2; i++) {
        const HatConnectorStatus *c = &hs->connector[i];
        cJSON *obj = cJSON_CreateObject();
        cJSON_AddBoolToObject(obj, "enabled", c->enabled);
        cJSON_AddNumberToObject(obj, "currentMa", c->current_ma);
        cJSON_AddBoolToObject(obj, "fault", c->fault);
        cJSON_AddItemToObject(root, i == 0 ? "connA" : "connB", obj);
    }
    cJSON_AddNumberToObject(root, "ioVoltageMv", hs->io_voltage_mv);
    return send_json(req, root);
}

// POST /api/hat/power
esp_err_t handle_post_hat_power(httpd_req_t *req)
{
    if (check_admin_auth(req) != ESP_OK) return send_error(req, 401, "Admin token required");
    if (!hat_detected()) return send_error(req, 404, "HAT not detected");

    cJSON *doc = recv_json_body(req);
    if (!doc) return send_error(req, 400, "Invalid JSON");

    VALIDATE_JSON_FIELD(doc, "connector", Number, "Field 'connector' must be a number");
    VALIDATE_JSON_FIELD(doc, "enable", Bool, "Field 'enable' must be a boolean");

    int connector = cJSON_GetObjectItem(doc, "connector")->valueint;
    bool enable = cJSON_IsTrue(cJSON_GetObjectItem(doc, "enable"));
    cJSON_Delete(doc);

    if (connector < 0 || connector > 1) {
        return send_error(req, 400, "connector must be 0 or 1");
    }

    if (!hat_set_power((HatConnector)connector, enable)) {
        return send_error(req, 503, "HAT power command failed");
    }

    const HatState *hs = hat_get_state();
    const HatConnectorStatus *c = &hs->connector[connector];
    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", true);
    cJSON_AddNumberToObject(root, "connector", connector);
    cJSON_AddBoolToObject(root, "enabled", c->enabled);
    cJSON_AddNumberToObject(root, "currentMa", c->current_ma);
    cJSON_AddBoolToObject(root, "fault", c->fault);
    return send_json(req, root);
}

// POST /api/hat/v2/led
esp_err_t handle_post_hat_v2_led(httpd_req_t *req)
{
    if (check_admin_auth(req) != ESP_OK) return send_error(req, 401, "Admin token required");
    if (!hat_detected()) return send_error(req, 404, "HAT not detected");

    cJSON *doc = recv_json_body(req);
    if (!doc) return send_error(req, 400, "Invalid JSON");

    VALIDATE_JSON_FIELD(doc, "ledId", Number, "Field 'ledId' must be a number");
    VALIDATE_JSON_FIELD(doc, "colorCode", Number, "Field 'colorCode' must be a number");

    uint8_t led_id = (uint8_t)cJSON_GetObjectItem(doc, "ledId")->valueint;
    uint8_t color_code = (uint8_t)cJSON_GetObjectItem(doc, "colorCode")->valueint;
    cJSON_Delete(doc);

    if (!hat_set_led_state(led_id, color_code)) {
        return send_error(req, 503, "HAT LED command failed");
    }

    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", true);
    return send_json(req, root);
}

// POST /api/hat/v2/la/route
esp_err_t handle_post_hat_v2_la_route(httpd_req_t *req)
{
    if (check_admin_auth(req) != ESP_OK) return send_error(req, 401, "Admin token required");
    if (!hat_detected()) return send_error(req, 404, "HAT not detected");

    cJSON *doc = recv_json_body(req);
    if (!doc) return send_error(req, 400, "Invalid JSON");

    VALIDATE_JSON_FIELD(doc, "route", Number, "Field 'route' must be a number");

    uint8_t route = (uint8_t)cJSON_GetObjectItem(doc, "route")->valueint;
    cJSON_Delete(doc);

    if (!hat_la_set_route(route)) {
        return send_error(req, 503, "HAT LA route command failed");
    }

    const HatState *hs = hat_get_state();
    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", true);
    cJSON_AddNumberToObject(root, "route", hs->la_route);
    return send_json(req, root);
}

// POST /api/hat/v2/calibrate/start
esp_err_t handle_post_hat_v2_calibrate_start(httpd_req_t *req)
{
    if (check_admin_auth(req) != ESP_OK) return send_error(req, 401, "Admin token required");
    if (!hat_detected()) return send_error(req, 404, "HAT not detected");

    cJSON *doc = recv_json_body(req);
    if (!doc) return send_error(req, 400, "Invalid JSON");

    VALIDATE_JSON_FIELD(doc, "railId", Number, "Field 'railId' must be a number");
    uint8_t rail_id = (uint8_t)cJSON_GetObjectItem(doc, "railId")->valueint;
    cJSON_Delete(doc);

    if (rail_id >= HAT_RAIL_COUNT) {
        return send_error(req, 400, "railId out of range (0-2)");
    }

    uint8_t status = 0;
    if (!hat_calibrate_start(rail_id, &status)) {
        return send_error(req, 503, "HAT calibration start failed");
    }

    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", true);
    cJSON_AddNumberToObject(root, "status", status);
    return send_json(req, root);
}

// GET /api/hat/v2/calibrate/status
esp_err_t handle_get_hat_v2_calibrate_status(httpd_req_t *req)
{
    if (!hat_detected()) return send_error(req, 404, "HAT not detected");

    uint8_t state = 0, progress = 0, rail_id = 0, last_error = 0;
    uint8_t persist_state = 0, stage = 0, point = 0;
    int8_t code = 0;
    int32_t measured_mv = -1;
    int32_t min_mv = -1, max_mv = -1, max_gap_mv = -1, max_error_mv = -1;
    uint16_t validation_flags = 0;
    if (!hat_calibrate_status(&state, &progress, &rail_id, &last_error,
                              &persist_state, &stage, &point, &code, &measured_mv,
                              &min_mv, &max_mv, &max_gap_mv, &max_error_mv,
                              &validation_flags)) {
        return send_error(req, 503, "HAT not responding");
    }

    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "state", state);
    cJSON_AddNumberToObject(root, "progress", progress);
    cJSON_AddNumberToObject(root, "railId", rail_id);
    cJSON_AddNumberToObject(root, "lastError", last_error);
    cJSON_AddNumberToObject(root, "persistState", persist_state);
    cJSON_AddNumberToObject(root, "stage", stage);
    cJSON_AddNumberToObject(root, "point", point);
    cJSON_AddNumberToObject(root, "code", code);
    cJSON_AddNumberToObject(root, "measuredMv", measured_mv);
    cJSON_AddNumberToObject(root, "minMv", min_mv);
    cJSON_AddNumberToObject(root, "maxMv", max_mv);
    cJSON_AddNumberToObject(root, "maxGapMv", max_gap_mv);
    cJSON_AddNumberToObject(root, "maxErrorMv", max_error_mv);
    cJSON_AddNumberToObject(root, "validationFlags", validation_flags);
    return send_json(req, root);
}

// POST /api/hat/v2/calibrate/import
esp_err_t handle_post_hat_v2_calibrate_import(httpd_req_t *req)
{
    if (check_admin_auth(req) != ESP_OK) return send_error(req, 401, "Admin token required");
    if (!hat_detected()) return send_error(req, 404, "HAT not detected");

    cJSON *doc = recv_json_body(req);
    if (!doc) return send_error(req, 400, "Invalid JSON");

    VALIDATE_JSON_FIELD(doc, "railId", Number, "Field 'railId' must be a number");
    VALIDATE_JSON_FIELD(doc, "points", Array, "Field 'points' must be an array");

    uint8_t rail_id = (uint8_t)cJSON_GetObjectItem(doc, "railId")->valueint;
    cJSON *points = cJSON_GetObjectItem(doc, "points");
    int count = cJSON_GetArraySize(points);

    if (rail_id >= HAT_RAIL_COUNT) {
        cJSON_Delete(doc);
        return send_error(req, 400, "railId out of range (0-2)");
    }
    if (count < 0 || count > 6) {
        cJSON_Delete(doc);
        return send_error(req, 400, "points count must be between 0 and 6");
    }

    uint8_t points_data[30] = {};
    size_t pos = 0;
    for (int i = 0; i < count; i++) {
        cJSON *pt = cJSON_GetArrayItem(points, i);
        if (!pt || !cJSON_IsObject(pt)) {
            cJSON_Delete(doc);
            return send_error(req, 400, "point elements must be objects");
        }
        cJSON *dac_item = cJSON_GetObjectItem(pt, "dacCode");
        cJSON *v_item = cJSON_GetObjectItem(pt, "measuredV");
        if (!dac_item || !cJSON_IsNumber(dac_item) || !v_item || !cJSON_IsNumber(v_item)) {
            cJSON_Delete(doc);
            return send_error(req, 400, "point must contain dacCode and measuredV");
        }
        points_data[pos++] = (uint8_t)(int8_t)dac_item->valueint;
        float measured_v = (float)v_item->valuedouble;
        memcpy(&points_data[pos], &measured_v, 4);
        pos += 4;
    }
    cJSON_Delete(doc);

    if (!hat_calibrate_import(rail_id, (uint8_t)count, points_data, pos)) {
        return send_error(req, 503, "HAT calibration import failed");
    }

    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", true);
    return send_json(req, root);
}

// POST /api/hat/v2/io_bank
esp_err_t handle_post_hat_v2_io_bank(httpd_req_t *req)
{
    if (check_admin_auth(req) != ESP_OK) return send_error(req, 401, "Admin token required");
    if (!hat_detected()) return send_error(req, 404, "HAT not detected");

    cJSON *doc = recv_json_body(req);
    if (!doc) return send_error(req, 400, "Invalid JSON");

    VALIDATE_JSON_FIELD(doc, "dirs", Number, "Field 'dirs' must be a number");
    VALIDATE_JSON_FIELD(doc, "ups", Number, "Field 'ups' must be a number");
    VALIDATE_JSON_FIELD(doc, "dns", Number, "Field 'dns' must be a number");

    uint8_t dirs = (uint8_t)cJSON_GetObjectItem(doc, "dirs")->valueint;
    uint8_t ups  = (uint8_t)cJSON_GetObjectItem(doc, "ups")->valueint;
    uint8_t dns  = (uint8_t)cJSON_GetObjectItem(doc, "dns")->valueint;
    // Optional: output values for pins configured as output (bitmask, default 0).
    cJSON *vals_item = cJSON_GetObjectItem(doc, "vals");
    uint8_t vals = (vals_item && cJSON_IsNumber(vals_item))
                   ? (uint8_t)vals_item->valueint : 0;
    cJSON_Delete(doc);

    if (!hat_set_io_bank(dirs, ups, dns, vals)) {
        return send_error(req, 503, "HAT IO bank command failed");
    }

    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", true);
    return send_json(req, root);
}

// POST /api/hat/v2/level_shift
esp_err_t handle_post_hat_v2_level_shift(httpd_req_t *req)
{
    if (check_admin_auth(req) != ESP_OK) return send_error(req, 401, "Admin token required");
    if (!hat_detected()) return send_error(req, 404, "HAT not detected");

    cJSON *doc = recv_json_body(req);
    if (!doc) return send_error(req, 400, "Invalid JSON");

    VALIDATE_JSON_FIELD(doc, "oe", Bool, "Field 'oe' must be a boolean");
    VALIDATE_JSON_FIELD(doc, "dir", Bool, "Field 'dir' must be a boolean");

    bool oe = cJSON_IsTrue(cJSON_GetObjectItem(doc, "oe"));
    bool dir = cJSON_IsTrue(cJSON_GetObjectItem(doc, "dir"));
    cJSON_Delete(doc);

    bool oe_out = false, dir_out = false;
    if (!hat_set_level_shift(oe, dir, &oe_out, &dir_out)) {
        return send_error(req, 503, "HAT level shift command failed");
    }

    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", true);
    cJSON_AddBoolToObject(root, "oe", oe_out);
    cJSON_AddBoolToObject(root, "dir", dir_out);
    return send_json(req, root);
}

// POST /api/hat/v2/io_voltage
esp_err_t handle_post_hat_v2_io_voltage(httpd_req_t *req)
{
    if (check_admin_auth(req) != ESP_OK) return send_error(req, 401, "Admin token required");
    if (!hat_detected()) return send_error(req, 404, "HAT not detected");

    cJSON *doc = recv_json_body(req);
    if (!doc) return send_error(req, 400, "Invalid JSON");

    VALIDATE_JSON_FIELD(doc, "voltageMv", Number, "Field 'voltageMv' must be a number");
    uint16_t mv = (uint16_t)cJSON_GetObjectItem(doc, "voltageMv")->valueint;
    cJSON_Delete(doc);

    if (!hat_set_io_voltage(mv)) {
        return send_error(req, 503, "HAT I/O voltage command failed");
    }

    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", true);
    cJSON_AddNumberToObject(root, "voltageMv", hat_get_state()->io_voltage_mv);
    return send_json(req, root);
}

// POST /api/hat/v2/swd/setup
esp_err_t handle_post_hat_v2_swd_setup(httpd_req_t *req)
{
    if (check_admin_auth(req) != ESP_OK) return send_error(req, 401, "Admin token required");
    if (!hat_detected()) return send_error(req, 404, "HAT not detected");

    cJSON *doc = recv_json_body(req);
    if (!doc) return send_error(req, 400, "Invalid JSON");

    VALIDATE_JSON_FIELD(doc, "targetVoltageMv", Number, "Field 'targetVoltageMv' must be a number");
    VALIDATE_JSON_FIELD(doc, "connector", Number, "Field 'connector' must be a number");

    uint16_t target_mv = (uint16_t)cJSON_GetObjectItem(doc, "targetVoltageMv")->valueint;
    uint8_t connector = (uint8_t)cJSON_GetObjectItem(doc, "connector")->valueint;
    cJSON_Delete(doc);

    if (connector > 1) {
        return send_error(req, 400, "connector must be 0 (A) or 1 (B)");
    }

    if (!hat_setup_swd(target_mv, (HatConnector)connector)) {
        return send_error(req, 503, "HAT SWD setup command failed");
    }

    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", true);
    return send_json(req, root);
}

// POST /api/hat/v2/la/log/enable   body: {"enable": true/false}
esp_err_t handle_post_hat_v2_la_log_enable(httpd_req_t *req)
{
    if (check_admin_auth(req) != ESP_OK) return send_error(req, 401, "Admin token required");
    if (!hat_detected()) return send_error(req, 404, "HAT not detected");

    cJSON *doc = recv_json_body(req);
    if (!doc) return send_error(req, 400, "Invalid JSON");
    VALIDATE_JSON_FIELD(doc, "enable", Bool, "Field 'enable' must be boolean");

    bool enable = cJSON_IsTrue(cJSON_GetObjectItem(doc, "enable"));
    cJSON_Delete(doc);

    if (!hat_la_log_enable(enable)) {
        return send_error(req, 503, "HAT log relay command failed");
    }

    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", true);
    return send_json(req, root);
}

// GET /api/hat/v2/la/log  — drain buffered RP2040 log lines
esp_err_t handle_get_hat_v2_la_log(httpd_req_t *req)
{
    // Allocate a 16 KB drain buffer (enough for 64 lines x 128 chars + JSON overhead)
    const size_t BUF_SZ = 16 * 1024;
    char *buf = (char *)heap_caps_malloc(BUF_SZ, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) return send_error(req, 500, "Out of memory");

    int len = hat_log_ring_drain(buf, BUF_SZ);
    if (len < 0) {
        free(buf);
        return send_error(req, 500, "Buffer overflow");
    }

    // Build response: {"lines": [...]}
    char *resp = (char *)heap_caps_malloc(BUF_SZ + 32, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!resp) { free(buf); return send_error(req, 500, "Out of memory"); }
    int resp_len = snprintf(resp, BUF_SZ + 32, "{\"lines\":%s}", buf);
    free(buf);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    esp_err_t err = httpd_resp_send(req, resp, resp_len);
    free(resp);
    return err;
}

// GET /api/debug - Combined debug status of all I2C devices
esp_err_t handle_get_debug(httpd_req_t *req)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "i2cBusOk", i2c_bus_ready());

    // AN-11: ADC poll loop counters
    {
        AdcPollStats ps = {};
        tasks_adc_poll_stats(&ps);
        cJSON *ap = cJSON_AddObjectToObject(root, "adcPoll");
        cJSON_AddNumberToObject(ap, "loops", ps.loops);
        cJSON_AddNumberToObject(ap, "ready", ps.ready);
        cJSON_AddNumberToObject(ap, "rdyIrq", ps.rdy_irq);
    }

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
