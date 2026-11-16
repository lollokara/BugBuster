// =============================================================================
// webserver_power.cpp - HTTP handlers: DS4424 IDAC, HUSB238 USB-PD, PCA9535 power and IDAC calibration.
// Routes are registered in webserver.cpp (initWebServer).
// =============================================================================

#include "webserver_internal.h"

// =============================================================================
// I2C Device Handlers - DS4424 / HUSB238 / PCA9535
// =============================================================================

// GET /api/idac - Get all IDAC status
esp_err_t handle_get_idac(httpd_req_t *req)
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
        float poly[4] = {0};
        bool have_poly = ds4424_cal_fit_cubic(ch, poly);
        cJSON_AddBoolToObject(obj, "polyValid", have_poly);
        cJSON *poly_arr = cJSON_AddArrayToObject(obj, "calPoly");
        for (int i = 0; i < 4; i++) cJSON_AddItemToArray(poly_arr, cJSON_CreateNumber((double)poly[i]));
        cJSON_AddItemToArray(channels, obj);
    }
    return send_json(req, root);
}

// GET /api/idac/cal/points?ch=0
esp_err_t handle_get_idac_cal_points(httpd_req_t *req)
{
    // Delegates so both transports return the SAME document. This handler used
    // to build its own response, which stopped at points[] and omitted the
    // entire "hat" calibration object that api_core attaches when a HAT is
    // present -- so a desktop client polling over HTTP during a HAT rail
    // calibration read resp.hat.progress as undefined and its progress bar
    // never moved, while the identical BLE poll worked.
    // See docs/superpowers/reviews/2026-08-03-design-sweep.md finding S2-3.
    char buf[128];
    int ch = 0;
    esp_err_t qrc = httpd_req_get_url_query_str(req, buf, sizeof(buf));
    if (qrc == ESP_OK) {
        char param[32];
        if (httpd_query_key_value(buf, "ch", param, sizeof(param)) == ESP_OK) {
            ch = atoi(param);
        }
    } else if (qrc == ESP_ERR_HTTPD_RESULT_TRUNC) {
        return send_error(req, 400, "query string too long");
    }
    if (ch < 0 || ch > 2) return send_error(req, 400, "ch must be 0-2");

    char path[64];
    snprintf(path, sizeof(path), "/api/idac/cal/points?ch=%d", ch);
    char *resp = api_core_handle("GET", path, NULL);
    return send_api_core_result(req, resp, "idac cal points unavailable");
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
    char *resp = api_core_handle("POST", "/api/idac/voltage", body);
    if (body) cJSON_Delete(body);
    if (!resp) return send_error(req, 500, "idac voltage failed");
    esp_err_t rc = send_raw_json(req, resp);
    cJSON_free(resp);
    return rc;
}

// Forward decl — handle_cal_post_dispatch is defined later but referenced here.
static esp_err_t handle_cal_post_dispatch(httpd_req_t *req);

// POST /api/idac dispatch
//
// NOTE: esp_http_server matches the first registered URI handler, so the
// `/api/idac/*` wildcard registered at startup catches *all* /api/idac/...
// paths. Keep cal requests routed through this dispatcher instead of
// registering a second overlapping `/api/idac/cal/*` handler.
esp_err_t handle_idac_post_dispatch(httpd_req_t *req)
{
    if (check_admin_auth(req) != ESP_OK) return send_error(req, 401, "Admin token required");

    if (strstr(req->uri, "/api/idac/code")) return handle_post_idac_code(req);
    if (strstr(req->uri, "/api/idac/voltage")) return handle_post_idac_voltage(req);
    if (strstr(req->uri, "/api/idac/cal/")) return handle_cal_post_dispatch(req);
    return send_error(req, 404, "Unknown IDAC endpoint");
}

// GET /api/usbpd - Get USB PD status
esp_err_t handle_get_usbpd(httpd_req_t *req)
{
    char *resp = api_core_handle("GET", "/api/usbpd", NULL);
    if (!resp) return send_error(req, 500, "usbpd failed");
    esp_err_t rc = send_raw_json(req, resp);
    cJSON_free(resp);
    return rc;
}

// POST /api/usbpd/select  body: {"voltage": 20}
static esp_err_t handle_post_usbpd_select(httpd_req_t *req)
{
    cJSON *body = recv_json_body(req);
    char *resp = api_core_handle("POST", "/api/usbpd/select", body);
    if (body) cJSON_Delete(body);
    if (!resp) return send_error(req, 500, "usbpd select failed");
    esp_err_t rc = send_raw_json(req, resp);
    cJSON_free(resp);
    return rc;
}

// POST /api/usbpd dispatch
esp_err_t handle_usbpd_post_dispatch(httpd_req_t *req)
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
esp_err_t handle_get_ioexp(httpd_req_t *req)
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
    char *resp = api_core_handle("POST", "/api/ioexp/control", body);
    if (body) cJSON_Delete(body);
    if (!resp) return send_error(req, 500, "ioexp control failed");
    esp_err_t rc = send_raw_json(req, resp);
    cJSON_free(resp);
    return rc;
}

// GET /api/ioexp/faults - Get PCA9535 fault log
esp_err_t handle_get_ioexp_faults(httpd_req_t *req)
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
    // Use the shared bounded reader (1024 byte cap) so this endpoint matches
    // the size contract of every other JSON POST and rejects oversized bodies
    // with the same semantics.
    cJSON *json = recv_json_body(req);
    if (!json) return send_error(req, 400, "Invalid JSON or body too large");

    PcaFaultConfig cfg = {};
    pca9535_get_fault_config(&cfg);
    cJSON *ad = cJSON_GetObjectItem(json, "auto_disable");
    cJSON *le = cJSON_GetObjectItem(json, "log_events");
    if (cJSON_IsBool(ad)) cfg.auto_disable_efuse = cJSON_IsTrue(ad);
    if (cJSON_IsBool(le)) cfg.log_events = cJSON_IsTrue(le);
    cJSON_Delete(json);

    pca9535_set_fault_config(&cfg);

    cJSON *rsp = cJSON_CreateObject();
    cJSON_AddBoolToObject(rsp, "auto_disable", cfg.auto_disable_efuse);
    cJSON_AddBoolToObject(rsp, "log_events", cfg.log_events);
    return send_json(req, rsp);
}

// POST /api/ioexp/rail_up  body: {"rail":1, "voltage":5.0, "settleMs":500,
//   "confirm":false, "powerCycle":false, "efuseMask":0}
// Same firmware sequence as BBP RAIL_POWER_UP (power/rail_power.h).
static esp_err_t handle_post_ioexp_rail_up(httpd_req_t *req)
{
    cJSON *body = recv_json_body(req);
    if (!body) return send_error(req, 400, "Invalid JSON");
    const cJSON *j_rail = cJSON_GetObjectItem(body, "rail");
    const cJSON *j_v    = cJSON_GetObjectItem(body, "voltage");
    const cJSON *j_set  = cJSON_GetObjectItem(body, "settleMs");
    const cJSON *j_mask = cJSON_GetObjectItem(body, "efuseMask");
    int   rail   = cJSON_IsNumber(j_rail) ? j_rail->valueint : 0;
    float volts  = cJSON_IsNumber(j_v) ? (float)j_v->valuedouble : 0.0f;
    int   settle = cJSON_IsNumber(j_set) ? j_set->valueint : 500;
    int   mask   = cJSON_IsNumber(j_mask) ? j_mask->valueint : 0;
    uint8_t flags = (uint8_t)((cJSON_IsTrue(cJSON_GetObjectItem(body, "confirm")) ? RAIL_PU_CONFIRM : 0) |
                              (cJSON_IsTrue(cJSON_GetObjectItem(body, "powerCycle")) ? RAIL_PU_POWER_CYCLE : 0));
    cJSON_Delete(body);
    if (rail < 1 || rail > 2 || settle < 0 || settle > (int)RAIL_PU_MAX_SETTLE_MS || mask < 0 || mask > 3)
        return send_error(req, 400, "rail must be 1-2, settleMs 0-5000, efuseMask 0-3");

    RailPowerResult r = {};
    int rc = rail_power_up(rail_power_ops_hw(), (uint8_t)rail, volts, (uint16_t)settle,
                           flags, (uint8_t)mask, &r);
    if (rc == CMD_ERR_BAD_ARG)
        return send_error(req, 400, "voltage must be 3-15 V; above 12 V needs confirm");
    if (rc != 0) return send_error(req, 500, "rail power-up failed (I2C)");

    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "rail", rail);
    cJSON_AddNumberToObject(root, "appliedV", r.applied_v);
    cJSON_AddBoolToObject(root, "clamped", r.clamped);
    cJSON_AddBoolToObject(root, "pg", r.pg);
    cJSON *f = cJSON_AddArrayToObject(root, "efuseFaults");
    cJSON_AddItemToArray(f, cJSON_CreateBool(r.fault[0]));
    cJSON_AddItemToArray(f, cJSON_CreateBool(r.fault[1]));
    return send_json(req, root);
}

// POST /api/ioexp dispatch
esp_err_t handle_ioexp_post_dispatch(httpd_req_t *req)
{
    if (check_admin_auth(req) != ESP_OK) return send_error(req, 401, "Admin token required");

    if (strstr(req->uri, "/api/ioexp/control")) return handle_post_ioexp_control(req);
    if (strstr(req->uri, "/api/ioexp/fault_config")) return handle_post_ioexp_fault_config(req);
    if (strstr(req->uri, "/api/ioexp/rail_up")) return handle_post_ioexp_rail_up(req);
    return send_error(req, 404, "Unknown IO Expander endpoint");
}


// =============================================================================
// IDAC Calibration Web Handlers
// =============================================================================

// POST /api/idac/cal/point  body: {"ch":0, "code":-10, "measuredV":5.23}
static esp_err_t handle_post_cal_point(httpd_req_t *req)
{
    cJSON *body = recv_json_body(req);
    char *resp = api_core_handle("POST", "/api/idac/cal/point", body);
    if (body) cJSON_Delete(body);
    if (!resp) return send_error(req, 500, "cal point failed");
    esp_err_t rc = send_raw_json(req, resp);
    cJSON_free(resp);
    return rc;
}

// POST /api/idac/cal/clear  body: {"ch":0}
static esp_err_t handle_post_cal_clear(httpd_req_t *req)
{
    cJSON *body = recv_json_body(req);
    char *resp = api_core_handle("POST", "/api/idac/cal/clear", body);
    if (body) cJSON_Delete(body);
    if (!resp) return send_error(req, 500, "cal clear failed");
    esp_err_t rc = send_raw_json(req, resp);
    cJSON_free(resp);
    return rc;
}

// POST /api/idac/cal/save
static esp_err_t handle_post_cal_save(httpd_req_t *req)
{
    char *resp = api_core_handle("POST", "/api/idac/cal/save", NULL);
    if (!resp) return send_error(req, 500, "cal save failed");
    esp_err_t rc = send_raw_json(req, resp);
    cJSON_free(resp);
    return rc;
}

static esp_err_t handle_cal_post_dispatch(httpd_req_t *req)
{
    if (check_admin_auth(req) != ESP_OK) return send_error(req, 401, "Admin token required");

    if (strstr(req->uri, "/api/idac/cal/point")) return handle_post_cal_point(req);
    if (strstr(req->uri, "/api/idac/cal/clear")) return handle_post_cal_clear(req);
    if (strstr(req->uri, "/api/idac/cal/save")) return handle_post_cal_save(req);
    return send_error(req, 404, "Unknown calibration endpoint");
}
