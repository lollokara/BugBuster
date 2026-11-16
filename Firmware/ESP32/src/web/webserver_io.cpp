// =============================================================================
// webserver_io.cpp - HTTP handlers: digital IO, IO ownership, ADGS routes, self-test, quick setup, UART bridge, waveform generator, MUX, external bus.
// Routes are registered in webserver.cpp (initWebServer).
// =============================================================================

#include "webserver_internal.h"

static const char* TAG = "webserver";

// =============================================================================
// Digital IO (ESP32 GPIO) endpoints
// =============================================================================

// GET /api/dio — read all 12 IO states
esp_err_t handle_get_dio(httpd_req_t *req)
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

    // Route the IO terminal through the MUX before configuring the GPIO.
    // Non-fatal: log a warning and proceed so the GPIO config still applies.
    // Without this, the MUX switch is never closed and the signal never
    // reaches the physical IO connector (sev-3 DIO-bypass-bus_planner audit finding).
    {
        char bp_err[64];
        if (!bus_planner_route_digital_input((uint8_t)io, bp_err, sizeof(bp_err))) {
            ESP_LOGW(TAG, "handle_post_dio_config: bus_planner io=%d: %s", io, bp_err);
        }
    }

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
esp_err_t handle_get_dio_single(httpd_req_t *req)
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
esp_err_t handle_dio_post_dispatch(httpd_req_t *req)
{
    if (check_admin_auth(req) != ESP_OK) return send_error(req, 401, "Admin token required");

    const char *p = strstr(req->uri, "/api/dio/");
    if (!p) return send_error(req, 400, "Invalid DIO URI");

    // IO ownership guard for mutating DIO endpoints
#if defined(BB_IO_OWNERSHIP) && BB_IO_OWNERSHIP
    {
        int io = atoi(p + 9);
        if (io >= 1 && io <= 12) {
            uint32_t fp = io_owner_compute_token_fp(auth_get_admin_token());
            io_owner_t caller = { IO_OWNER_HTTP, (uint8_t)(fp & 0xFF), fp };
            uint64_t now_ms = (uint64_t)(esp_timer_get_time() / 1000LL);
            int rc = io_owner_guard_or_auto(io_owner_slot_for_io((uint8_t)io),
                                            caller.kind, caller.session_id, caller.token_fp32, now_ms);
            if (rc != 0) return send_error(req, 409, "IO slot owned by another session");
        }
    }
#endif

    // Find the action after /api/dio/N/
    p += 9; // skip "/api/dio/"
    while (*p >= '0' && *p <= '9') p++; // skip IO number
    if (*p == '/') p++;

    if (strcmp(p, "config") == 0) return handle_post_dio_config(req);
    if (strcmp(p, "set") == 0)    return handle_post_dio_set(req);

    return send_error(req, 404, "Unknown DIO POST endpoint");
}

// =============================================================================
// IO Ownership endpoints
// =============================================================================

// GET /api/io/owner — return all 16 slot states
esp_err_t handle_get_io_owner(httpd_req_t *req)
{
    io_owner_slot_t slots[IO_OWNER_NUM_SLOTS];
    io_owner_get_all(slots);

    cJSON *root = cJSON_CreateObject();
    cJSON *arr  = cJSON_AddArrayToObject(root, "slots");
    for (int i = 0; i < IO_OWNER_NUM_SLOTS; i++) {
        cJSON *obj = cJSON_CreateObject();
        cJSON_AddNumberToObject(obj, "slot",     i);
        cJSON_AddNumberToObject(obj, "kind",     (int)slots[i].kind);
        cJSON_AddNumberToObject(obj, "session",  slots[i].session_id);
        cJSON_AddNumberToObject(obj, "lease_until_ms", (double)(uint32_t)slots[i].lease_until_ms);
        cJSON_AddBoolToObject(obj, "free", slots[i].kind == IO_OWNER_NONE);
        cJSON_AddItemToArray(arr, obj);
    }
    return send_json(req, root);
}

// POST /api/io/owner  body: {"slot":N, "kind":K, "session":S, "lease_ms":L}
esp_err_t handle_post_io_owner(httpd_req_t *req)
{
    if (check_admin_auth(req) != ESP_OK) return send_error(req, 401, "Admin token required");

    cJSON *body = recv_json_body(req);
    if (!body) return send_error(req, 400, "Invalid JSON");

    cJSON *jslot  = cJSON_GetObjectItem(body, "slot");
    cJSON *jkind  = cJSON_GetObjectItem(body, "kind");
    cJSON *jsess  = cJSON_GetObjectItem(body, "session");
    cJSON *jlease = cJSON_GetObjectItem(body, "lease_ms");
    if (!cJSON_IsNumber(jslot) || !cJSON_IsNumber(jkind)) {
        cJSON_Delete(body);
        return send_error(req, 400, "Missing slot or kind");
    }
    int slot     = jslot->valueint;
    int kind     = jkind->valueint;
    int session  = jsess  ? jsess->valueint  : 0;
    int lease_ms = jlease ? jlease->valueint : 0;
    cJSON_Delete(body);

    if (slot < 0 || slot >= IO_OWNER_NUM_SLOTS) return send_error(req, 400, "slot out of range");

    uint64_t now_ms = (uint64_t)(esp_timer_get_time() / 1000LL);
    bool ok = io_owner_acquire((uint8_t)slot, (io_owner_kind_t)kind,
                               (uint8_t)session, 0, (uint32_t)lease_ms, now_ms);
    if (!ok) return send_error(req, 409, "IO slot owned by another session");

    cJSON *resp = cJSON_CreateObject();
    cJSON_AddBoolToObject(resp, "ok", true);
    cJSON_AddNumberToObject(resp, "slot", slot);
    return send_json(req, resp);
}

// DELETE /api/io/owner  body: {"slot":N, "session":S}
esp_err_t handle_delete_io_owner(httpd_req_t *req)
{
    if (check_admin_auth(req) != ESP_OK) return send_error(req, 401, "Admin token required");

    cJSON *body = recv_json_body(req);
    if (!body) return send_error(req, 400, "Invalid JSON");

    cJSON *jslot = cJSON_GetObjectItem(body, "slot");
    cJSON *jsess = cJSON_GetObjectItem(body, "session");
    if (!cJSON_IsNumber(jslot)) { cJSON_Delete(body); return send_error(req, 400, "Missing slot"); }
    int slot    = jslot->valueint;
    int session = jsess ? jsess->valueint : 0;
    cJSON_Delete(body);

    if (slot < 0 || slot >= IO_OWNER_NUM_SLOTS) return send_error(req, 400, "slot out of range");

    bool released = io_owner_release((uint8_t)slot, (uint8_t)session);
    cJSON *resp = cJSON_CreateObject();
    cJSON_AddBoolToObject(resp, "ok", released);
    cJSON_AddNumberToObject(resp, "slot", slot);
    return send_json(req, resp);
}

// POST /api/io/owner/force  body: {"slot":N}
esp_err_t handle_post_io_owner_force(httpd_req_t *req)
{
    if (check_admin_auth(req) != ESP_OK) return send_error(req, 401, "Admin token required");

    cJSON *body = recv_json_body(req);
    if (!body) return send_error(req, 400, "Invalid JSON");

    cJSON *jslot = cJSON_GetObjectItem(body, "slot");
    if (!cJSON_IsNumber(jslot)) { cJSON_Delete(body); return send_error(req, 400, "Missing slot"); }
    int slot = jslot->valueint;
    cJSON_Delete(body);

    if (slot == 255) {
        // 255 (0xFF) = release all slots
        for (uint8_t i = 0; i < IO_OWNER_NUM_SLOTS; i++) {
            io_owner_force_release(i);
        }
    } else {
        if (slot < 0 || slot >= IO_OWNER_NUM_SLOTS) return send_error(req, 400, "slot out of range");
        io_owner_force_release((uint8_t)slot);
    }
    cJSON *resp = cJSON_CreateObject();
    cJSON_AddBoolToObject(resp, "ok", true);
    cJSON_AddNumberToObject(resp, "slot", slot);
    return send_json(req, resp);
}

// =============================================================================
// ADGS routes endpoint
// =============================================================================

// GET /api/adgs/routes — return cached switch state for all main devices
esp_err_t handle_get_adgs_routes(httpd_req_t *req)
{
    uint8_t states[ADGS_API_MAIN_DEVICES];
    adgs_get_api_states(states);

    cJSON *root = cJSON_CreateObject();
    cJSON *arr  = cJSON_AddArrayToObject(root, "devices");
    for (int d = 0; d < ADGS_API_MAIN_DEVICES; d++) {
        cJSON *obj = cJSON_CreateObject();
        cJSON_AddNumberToObject(obj, "device",     d);
        cJSON_AddNumberToObject(obj, "switchMask", states[d]);
        // List closed switches
        cJSON *sw_arr = cJSON_AddArrayToObject(obj, "closedSwitches");
        for (int s = 0; s < 8; s++) {
            if (states[d] & (1u << s)) {
                cJSON_AddItemToArray(sw_arr, cJSON_CreateNumber(s));
            }
        }
        cJSON_AddItemToArray(arr, obj);
    }
#if ADGS_HAS_SELFTEST
    cJSON_AddNumberToObject(root, "selftestMask", adgs_get_selftest());
#endif
    add_bool_alias(root, "muxFaulted", "mux_faulted", adgs_is_faulted());
    return send_json(req, root);
}

// =============================================================================
// Self-Test / Calibration endpoints
// =============================================================================

// GET /api/selftest — boot result + cal status
esp_err_t handle_get_selftest(httpd_req_t *req)
{
    char *resp = api_core_handle("GET", "/api/selftest", NULL);
    if (!resp) return send_error(req, 500, "selftest failed");
    esp_err_t rc = send_raw_json(req, resp);
    cJSON_free(resp);
    return rc;
}

// GET /api/selftest/supply/{rail} — measure a supply rail (0=VADJ1, 1=VADJ2, 2=3V3_ADJ)
esp_err_t handle_get_selftest_supply(httpd_req_t *req)
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
    // Debug block — commented out, kept for future diagnostics
    // const SelftestSupplyDebug *dbg = selftest_get_last_supply_debug();
    // if (dbg) {
    //     cJSON *debug = cJSON_AddObjectToObject(resp, "debug");
    //     cJSON_AddNumberToObject(debug, "rail", dbg->rail);
    //     cJSON_AddNumberToObject(debug, "u23Mask", dbg->u23_mask);
    //     cJSON_AddNumberToObject(debug, "rawCode", dbg->raw_code);
    //     cJSON_AddNumberToObject(debug, "adcUpr", dbg->adc_upr);
    //     cJSON_AddNumberToObject(debug, "adcConfig", dbg->adc_config);
    //     cJSON_AddNumberToObject(debug, "requestedMux", dbg->requested_mux);
    //     cJSON_AddNumberToObject(debug, "requestedRange", dbg->requested_range);
    //     cJSON_AddNumberToObject(debug, "configuredMux", (dbg->adc_config & ADC_CONFIG_CONV_MUX_MASK) >> ADC_CONFIG_CONV_MUX_SHIFT);
    //     cJSON_AddNumberToObject(debug, "configuredRange", (dbg->adc_config & ADC_CONFIG_CONV_RANGE_MASK) >> ADC_CONFIG_CONV_RANGE_SHIFT);
    //     cJSON_AddNumberToObject(debug, "configuredRate", (dbg->adc_config & ADC_CONFIG_CONV_RATE_MASK) >> ADC_CONFIG_CONV_RATE_SHIFT);
    //     cJSON_AddNumberToObject(debug, "adcMux", (dbg->adc_upr & ADC_RESULT_UPR_CONV_RES_MUX_MASK) >> ADC_RESULT_UPR_CONV_RES_MUX_SHIFT);
    //     cJSON_AddNumberToObject(debug, "adcRange", (dbg->adc_upr & ADC_RESULT_UPR_CONV_RES_RANGE_MASK) >> ADC_RESULT_UPR_CONV_RES_RANGE_SHIFT);
    //     cJSON_AddNumberToObject(debug, "adcSeqCount", (dbg->adc_upr & ADC_RESULT_UPR_CONV_SEQ_COUNT_MASK) >> ADC_RESULT_UPR_CONV_SEQ_COUNT_SHIFT);
    //     cJSON_AddNumberToObject(debug, "liveStatus", dbg->live_status);
    //     cJSON_AddNumberToObject(debug, "channelAlert", dbg->channel_alert);
    //     cJSON_AddNumberToObject(debug, "alertStatus", dbg->alert_status);
    //     cJSON_AddNumberToObject(debug, "rawVoltage", dbg->raw_voltage_v);
    //     cJSON_AddBoolToObject(debug, "readOk", dbg->read_ok);
    //     cJSON_AddBoolToObject(debug, "adcReady", dbg->adc_ready);
    //     cJSON_AddBoolToObject(debug, "muxFaulted", dbg->mux_faulted);
    // }
    return send_json(req, resp);
}

// GET /api/selftest/supplies/cached — cached supply rail voltages
esp_err_t handle_get_selftest_supplies_cached(httpd_req_t *req)
{
    char *resp = api_core_handle("GET", "/api/selftest/supplies/cached", NULL);
    return send_api_core_result(req, resp, "supplies unavailable");
}

// GET /api/overview — coalesced snapshot for the on-device Overview tab.
// One request returns the same data the tab used to fetch via 5 separate
// calls (idac, ioexp, selftest/supply/{0,1,2}). Drops Overview's polling
// budget from 5 reqs / 2.5 s to 1 req / 2.5 s. Layout is intentionally
// flat — older clients can keep using the per-resource endpoints.
esp_err_t handle_get_overview(httpd_req_t *req)
{
    // Delegates to the shared, transport-agnostic api_core so HTTP and BLE
    // return the identical snapshot from a single implementation.
    char *resp = api_core_handle("GET", "/api/overview", NULL);
    if (!resp) return send_error(req, 500, "overview failed");
    esp_err_t rc = send_raw_json(req, resp);
    cJSON_free(resp);
    return rc;
}

// POST /api/selftest/worker body: {"enabled": true}
esp_err_t handle_post_selftest_worker(httpd_req_t *req)
{
    if (check_admin_auth(req) != ESP_OK) return send_error(req, 401, "Admin token required");
    cJSON *body = recv_json_body(req);
    char *resp = api_core_handle("POST", "/api/selftest/worker", body);
    if (body) cJSON_Delete(body);
    if (!resp) return send_error(req, 500, "selftest worker failed");
    esp_err_t rc = send_raw_json(req, resp);
    cJSON_free(resp);
    return rc;
}

// POST /api/selftest/calibrate body: {"channel": 1}
esp_err_t handle_post_selftest_calibrate(httpd_req_t *req)
{
    if (check_admin_auth(req) != ESP_OK) return send_error(req, 401, "Admin token required");
    cJSON *body = recv_json_body(req);
    char *resp = api_core_handle("POST", "/api/selftest/calibrate", body);
    if (body) cJSON_Delete(body);
    if (!resp) return send_error(req, 500, "calibrate failed");
    esp_err_t rc = send_raw_json(req, resp);
    cJSON_free(resp);
    return rc;
}

// GET /api/selftest/efuse_imon — monitored e-fuse current
esp_err_t handle_get_efuse_imon(httpd_req_t *req)
{
    char *resp = api_core_handle("GET", "/api/selftest/efuse_imon", NULL);
    if (!resp) return send_error(req, 500, "efuse_imon failed");
    esp_err_t rc = send_raw_json(req, resp);
    cJSON_free(resp);
    return rc;
}

// POST /api/selftest/efuse_imon body: {"efuse": 0..4, "confirm": bool}
esp_err_t handle_post_efuse_imon(httpd_req_t *req)
{
    if (check_admin_auth(req) != ESP_OK) return send_error(req, 401, "Admin token required");
    cJSON *body = recv_json_body(req);
    char *resp = api_core_handle("POST", "/api/selftest/efuse_imon", body);
    if (body) cJSON_Delete(body);
    if (!resp) return send_error(req, 500, "efuse_imon failed");
    esp_err_t rc = send_raw_json(req, resp);
    cJSON_free(resp);
    return rc;
}

// GET /api/selftest/supplies — measure internal ADC supplies
esp_err_t handle_get_selftest_supplies(httpd_req_t *req)
{
    char *resp = api_core_handle("GET", "/api/selftest/supplies", NULL);
    return send_api_core_result(req, resp, "supplies unavailable");
}

// =============================================================================
// Quick Setup endpoints
// =============================================================================

// GET /api/quicksetup
esp_err_t handle_get_quicksetup_list(httpd_req_t *req)
{
    char *resp = api_core_handle("GET", "/api/quicksetup", NULL);
    if (!resp) return send_error(req, 500, "quick setup list failed");
    esp_err_t rc = send_raw_json(req, resp);
    cJSON_free(resp);
    return rc;
}

// GET /api/quicksetup/{0-3}
esp_err_t handle_get_quicksetup_slot(httpd_req_t *req)
{
    char *resp = api_core_handle("GET", req->uri, NULL);
    if (!resp) return send_error(req, 500, "quick setup slot failed");
    esp_err_t rc = send_raw_json(req, resp);
    cJSON_free(resp);
    return rc;
}

// POST /api/quicksetup/{0-3}[/{apply|delete}]
esp_err_t handle_quicksetup_post_dispatch(httpd_req_t *req)
{
    if (check_admin_auth(req) != ESP_OK) return send_error(req, 401, "Admin token required");
    cJSON *body = recv_json_body(req);
    char *resp = api_core_handle("POST", req->uri, body);
    if (body) cJSON_Delete(body);
    if (!resp) return send_error(req, 500, "quick setup failed");
    esp_err_t rc = send_raw_json(req, resp);
    cJSON_free(resp);
    return rc;
}

// =============================================================================
// UART Bridge endpoints
// =============================================================================

// GET /api/uart/config
esp_err_t handle_get_uart_config(httpd_req_t *req)
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
esp_err_t handle_get_uart_pins(httpd_req_t *req)
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
esp_err_t handle_uart_post_dispatch(httpd_req_t *req)
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


// =============================================================================
// Waveform Generator Web Handlers
// =============================================================================

// POST /api/wavegen/start
static esp_err_t handle_post_wavegen_start(httpd_req_t *req)
{
    cJSON *body = recv_json_body(req);
    if (!body) return send_error(req, 400, "Invalid JSON");

    cJSON *ch_item = cJSON_GetObjectItem(body, "channel");
    cJSON *wf_item = cJSON_GetObjectItem(body, "waveform");
    cJSON *freq_item = cJSON_GetObjectItem(body, "freq_hz");
    cJSON *amp_item = cJSON_GetObjectItem(body, "amplitude");
    cJSON *off_item = cJSON_GetObjectItem(body, "offset");
    cJSON *mode_item = cJSON_GetObjectItem(body, "mode");

    int ch = cJSON_IsNumber(ch_item) ? ch_item->valueint : 0;
    int wf = cJSON_IsNumber(wf_item) ? wf_item->valueint : 0;
    double freq = cJSON_IsNumber(freq_item) ? freq_item->valuedouble : 1.0;
    double amp = cJSON_IsNumber(amp_item) ? amp_item->valuedouble : 5.0;
    double off = cJSON_IsNumber(off_item) ? off_item->valuedouble : 0.0;
    int mode = cJSON_IsNumber(mode_item) ? mode_item->valueint : 0;
    cJSON_Delete(body);

    if (ch < 0 || ch > 3) return send_error(req, 400, "Invalid channel");
    if (wf < 0 || wf > 3) return send_error(req, 400, "Invalid waveform");
    if (mode < 0 || mode > 1) return send_error(req, 400, "Invalid mode");
    if (freq < 0.1 || freq > 100.0) return send_error(req, 400, "Frequency out of range");
    double max_out = (mode == WAVEGEN_CURRENT) ? IOUT_MAX_MA : VOUT_BIPOLAR_OFFSET_V;
    if (amp < 0.0 || amp > max_out) return send_error(req, 400, "Amplitude out of range");
    if (off < -max_out || off > max_out) return send_error(req, 400, "Offset out of range");

    bbpStartWavegen((uint8_t)ch, (uint8_t)wf, (float)freq, (float)amp, (float)off, (uint8_t)mode);

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
esp_err_t handle_wavegen_post_dispatch(httpd_req_t *req)
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
esp_err_t handle_get_mux(httpd_req_t *req)
{
    cJSON *root = cJSON_CreateObject();
    uint8_t states[ADGS_API_MAIN_DEVICES] = {};
    // Do NOT call adgs_get_all_states() here: it copies ADGS_NUM_DEVICES (5)
    // bytes and this buffer holds ADGS_API_MAIN_DEVICES (4).
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
        // IO-8: indexes are validated above, so this is an interlock refusal.
        return send_error(req, 409,
                          "MUX write refused: U17 S3 and the U23 self-test are "
                          "mutually exclusive. Retry once the self-test releases.");
    }
    if (xSemaphoreTake(g_stateMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        adgs_get_all_states(g_deviceState.muxState);
        xSemaphoreGive(g_stateMutex);
    }
    hat_update_leds();

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
    if (!adgs_set_api_all_safe(states)) {
        // The BBP path reports this as BUSY; HTTP ignored the return value and
        // answered "ok", so a refused write looked like a write to the wrong
        // device. 409 is the HTTP equivalent of a transient resource conflict.
        return send_error(req, 409,
                          "MUX write refused: U17 S3 and the U23 self-test are "
                          "mutually exclusive. Retry once the self-test releases.");
    }
    if (xSemaphoreTake(g_stateMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        adgs_get_all_states(g_deviceState.muxState);
        xSemaphoreGive(g_stateMutex);
    }
    hat_update_leds();
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "status", "ok");
    return send_json(req, root);
}

esp_err_t handle_mux_post_dispatch(httpd_req_t *req)
{
    if (check_admin_auth(req) != ESP_OK) return send_error(req, 401, "Admin token required");

    if (strstr(req->uri, "/api/mux/switch")) return handle_post_mux_switch(req);
    if (strstr(req->uri, "/api/mux/all")) return handle_post_mux_all(req);
    return send_error(req, 404, "Unknown MUX endpoint");
}


// =============================================================================
// External routed bus handlers
// =============================================================================

static void json_add_optional_gpio(cJSON *obj, const char *key, uint8_t gpio)
{
    if (gpio == 0xFF) cJSON_AddNullToObject(obj, key);
    else cJSON_AddNumberToObject(obj, key, gpio);
}

static bool json_get_u8(cJSON *body, const char *key, uint8_t *out, bool required, uint8_t fallback = 0)
{
    cJSON *item = cJSON_GetObjectItem(body, key);
    if (!item || cJSON_IsNull(item)) {
        if (required) return false;
        *out = fallback;
        return true;
    }
    if (!cJSON_IsNumber(item) || item->valueint < 0 || item->valueint > 255) return false;
    *out = (uint8_t)item->valueint;
    return true;
}

static bool json_get_u16(cJSON *body, const char *key, uint16_t *out, uint16_t fallback)
{
    cJSON *item = cJSON_GetObjectItem(body, key);
    if (!item) {
        *out = fallback;
        return true;
    }
    if (!cJSON_IsNumber(item) || item->valueint < 0 || item->valueint > 65535) return false;
    *out = (uint16_t)item->valueint;
    return true;
}

static bool json_get_u32(cJSON *body, const char *key, uint32_t *out, uint32_t fallback)
{
    cJSON *item = cJSON_GetObjectItem(body, key);
    if (!item) {
        *out = fallback;
        return true;
    }
    if (!cJSON_IsNumber(item) || item->valuedouble < 0) return false;
    *out = (uint32_t)item->valuedouble;
    return true;
}

static bool json_get_bytes(cJSON *body, const char *key, uint8_t *out, size_t max_len, size_t *out_len)
{
    cJSON *arr = cJSON_GetObjectItem(body, key);
    if (!arr || !cJSON_IsArray(arr)) return false;
    size_t count = (size_t)cJSON_GetArraySize(arr);
    if (count > max_len) return false;
    for (size_t i = 0; i < count; i++) {
        cJSON *item = cJSON_GetArrayItem(arr, (int)i);
        if (!item || !cJSON_IsNumber(item) || item->valueint < 0 || item->valueint > 255) return false;
        out[i] = (uint8_t)item->valueint;
    }
    *out_len = count;
    return true;
}

esp_err_t handle_get_bus_status(httpd_req_t *req)
{
    bool i2c_ready = false, i2c_pullups = false, spi_ready = false;
    uint8_t sda = 0xFF, scl = 0xFF, sck = 0xFF, mosi = 0xFF, miso = 0xFF, cs = 0xFF, mode = 0;
    uint32_t i2c_hz = 0, spi_hz = 0;
    ext_i2c_get_status(&i2c_ready, &sda, &scl, &i2c_hz, &i2c_pullups);
    ext_spi_get_status(&spi_ready, &sck, &mosi, &miso, &cs, &spi_hz, &mode);

    cJSON *root = cJSON_CreateObject();
    cJSON *i2c = cJSON_AddObjectToObject(root, "i2c");
    cJSON_AddBoolToObject(i2c, "ready", i2c_ready);
    json_add_optional_gpio(i2c, "sdaGpio", sda);
    json_add_optional_gpio(i2c, "sclGpio", scl);
    cJSON_AddNumberToObject(i2c, "frequencyHz", i2c_hz);
    cJSON_AddBoolToObject(i2c, "internalPullups", i2c_pullups);

    cJSON *spi = cJSON_AddObjectToObject(root, "spi");
    cJSON_AddBoolToObject(spi, "ready", spi_ready);
    json_add_optional_gpio(spi, "sckGpio", sck);
    json_add_optional_gpio(spi, "mosiGpio", mosi);
    json_add_optional_gpio(spi, "misoGpio", miso);
    json_add_optional_gpio(spi, "csGpio", cs);
    cJSON_AddNumberToObject(spi, "frequencyHz", spi_hz);
    cJSON_AddNumberToObject(spi, "mode", mode);
    return send_json(req, root);
}

static esp_err_t handle_post_bus_i2c_setup(httpd_req_t *req)
{
    if (check_admin_auth(req) != ESP_OK) return send_error(req, 401, "Admin token required");
    cJSON *body = recv_json_body(req);
    if (!body) return send_error(req, 400, "Invalid JSON");
    uint8_t sda = 0, scl = 0;
    uint32_t hz = 400000;
    bool ok = json_get_u8(body, "sdaGpio", &sda, true) &&
              json_get_u8(body, "sclGpio", &scl, true) &&
              json_get_u32(body, "frequencyHz", &hz, 400000);
    bool pullups = cJSON_IsTrue(cJSON_GetObjectItem(body, "internalPullups"));
    cJSON_Delete(body);
    if (!ok) return send_error(req, 400, "Invalid I2C setup fields");
    if (!ext_i2c_setup(sda, scl, hz, pullups)) return send_error(req, 400, "I2C setup failed");
    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "sdaGpio", sda);
    cJSON_AddNumberToObject(root, "sclGpio", scl);
    cJSON_AddNumberToObject(root, "frequencyHz", hz);
    cJSON_AddBoolToObject(root, "internalPullups", pullups);
    return send_json(req, root);
}

static esp_err_t handle_post_bus_i2c_scan(httpd_req_t *req)
{
    cJSON *body = recv_json_body(req);
    if (!body) return send_error(req, 400, "Invalid JSON");
    uint8_t start = 0x08, stop = 0x77;
    uint16_t timeout_ms = 50;
    bool ok = json_get_u8(body, "startAddr", &start, false, 0x08) &&
              json_get_u8(body, "stopAddr", &stop, false, 0x77) &&
              json_get_u16(body, "timeoutMs", &timeout_ms, 50);
    bool skip_reserved = !cJSON_IsFalse(cJSON_GetObjectItem(body, "skipReserved"));
    cJSON_Delete(body);
    if (!ok) return send_error(req, 400, "Invalid I2C scan fields");
    uint8_t addrs[128] = {};
    size_t count = 0;
    if (!ext_i2c_scan(start, stop, skip_reserved, addrs, sizeof(addrs), &count, timeout_ms)) {
        return send_error(req, 409, ext_i2c_ready() ? "I2C scan failed" : "I2C not configured");
    }
    cJSON *root = cJSON_CreateObject();
    cJSON *arr = cJSON_AddArrayToObject(root, "addresses");
    for (size_t i = 0; i < count; i++) cJSON_AddItemToArray(arr, cJSON_CreateNumber(addrs[i]));
    cJSON_AddNumberToObject(root, "count", count);
    return send_json(req, root);
}

static esp_err_t handle_post_bus_i2c_write(httpd_req_t *req)
{
    if (check_admin_auth(req) != ESP_OK) return send_error(req, 401, "Admin token required");
    cJSON *body = recv_json_body(req);
    if (!body) return send_error(req, 400, "Invalid JSON");
    uint8_t addr = 0, bytes[255] = {};
    uint16_t timeout_ms = 100;
    size_t len = 0;
    bool ok = json_get_u8(body, "address", &addr, true) &&
              json_get_u16(body, "timeoutMs", &timeout_ms, 100) &&
              json_get_bytes(body, "data", bytes, sizeof(bytes), &len);
    cJSON_Delete(body);
    if (!ok) return send_error(req, 400, "Invalid I2C write fields");
    if (!ext_i2c_write(addr, bytes, len, timeout_ms)) return send_error(req, 409, "I2C write failed");
    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "written", len);
    return send_json(req, root);
}

static esp_err_t handle_post_bus_i2c_read(httpd_req_t *req)
{
    cJSON *body = recv_json_body(req);
    if (!body) return send_error(req, 400, "Invalid JSON");
    uint8_t addr = 0, length = 0, bytes[255] = {};
    uint16_t timeout_ms = 100;
    bool ok = json_get_u8(body, "address", &addr, true) &&
              json_get_u8(body, "length", &length, true) &&
              json_get_u16(body, "timeoutMs", &timeout_ms, 100);
    cJSON_Delete(body);
    if (!ok || length == 0) return send_error(req, 400, "Invalid I2C read fields");
    if (!ext_i2c_read(addr, bytes, length, timeout_ms)) return send_error(req, 409, "I2C read failed");
    cJSON *root = cJSON_CreateObject();
    cJSON *arr = cJSON_AddArrayToObject(root, "data");
    for (uint8_t i = 0; i < length; i++) cJSON_AddItemToArray(arr, cJSON_CreateNumber(bytes[i]));
    return send_json(req, root);
}

static esp_err_t handle_post_bus_i2c_write_read(httpd_req_t *req)
{
    cJSON *body = recv_json_body(req);
    if (!body) return send_error(req, 400, "Invalid JSON");
    uint8_t addr = 0, read_len = 0, write_bytes[255] = {}, read_bytes[255] = {};
    uint16_t timeout_ms = 100;
    size_t write_len = 0;
    bool ok = json_get_u8(body, "address", &addr, true) &&
              json_get_u8(body, "readLength", &read_len, true) &&
              json_get_u16(body, "timeoutMs", &timeout_ms, 100) &&
              json_get_bytes(body, "writeData", write_bytes, sizeof(write_bytes), &write_len);
    cJSON_Delete(body);
    if (!ok || read_len == 0 || write_len == 0) return send_error(req, 400, "Invalid I2C write-read fields");
    if (!ext_i2c_write_read(addr, write_bytes, write_len, read_bytes, read_len, timeout_ms)) {
        return send_error(req, 409, "I2C write-read failed");
    }
    cJSON *root = cJSON_CreateObject();
    cJSON *arr = cJSON_AddArrayToObject(root, "data");
    for (uint8_t i = 0; i < read_len; i++) cJSON_AddItemToArray(arr, cJSON_CreateNumber(read_bytes[i]));
    return send_json(req, root);
}

static esp_err_t handle_post_bus_spi_setup(httpd_req_t *req)
{
    if (check_admin_auth(req) != ESP_OK) return send_error(req, 401, "Admin token required");
    cJSON *body = recv_json_body(req);
    if (!body) return send_error(req, 400, "Invalid JSON");
    uint8_t sck = 0, mosi = 0xFF, miso = 0xFF, cs = 0xFF, mode = 0;
    uint32_t hz = 1000000;
    bool ok = json_get_u8(body, "sckGpio", &sck, true) &&
              json_get_u8(body, "mosiGpio", &mosi, false, 0xFF) &&
              json_get_u8(body, "misoGpio", &miso, false, 0xFF) &&
              json_get_u8(body, "csGpio", &cs, false, 0xFF) &&
              json_get_u8(body, "mode", &mode, false, 0) &&
              json_get_u32(body, "frequencyHz", &hz, 1000000);
    cJSON_Delete(body);
    if (!ok) return send_error(req, 400, "Invalid SPI setup fields");
    if (!ext_spi_setup(sck, mosi, miso, cs, hz, mode)) return send_error(req, 400, "SPI setup failed");
    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "sckGpio", sck);
    json_add_optional_gpio(root, "mosiGpio", mosi);
    json_add_optional_gpio(root, "misoGpio", miso);
    json_add_optional_gpio(root, "csGpio", cs);
    cJSON_AddNumberToObject(root, "frequencyHz", hz);
    cJSON_AddNumberToObject(root, "mode", mode);
    return send_json(req, root);
}

static esp_err_t handle_post_bus_spi_transfer(httpd_req_t *req)
{
    cJSON *body = recv_json_body(req);
    if (!body) return send_error(req, 400, "Invalid JSON");
    uint8_t tx[512] = {}, rx[512] = {};
    uint16_t timeout_ms = 100;
    size_t tx_len = 0;
    bool ok = json_get_u16(body, "timeoutMs", &timeout_ms, 100) &&
              json_get_bytes(body, "data", tx, sizeof(tx), &tx_len);
    cJSON_Delete(body);
    if (!ok || tx_len == 0) return send_error(req, 400, "Invalid SPI transfer fields");
    size_t rx_len = tx_len;
    if (!ext_spi_transfer(tx, tx_len, rx, &rx_len, timeout_ms)) {
        return send_error(req, 409, ext_spi_ready() ? "SPI transfer failed" : "SPI not configured");
    }
    cJSON *root = cJSON_CreateObject();
    cJSON *arr = cJSON_AddArrayToObject(root, "data");
    for (size_t i = 0; i < rx_len; i++) cJSON_AddItemToArray(arr, cJSON_CreateNumber(rx[i]));
    return send_json(req, root);
}

esp_err_t handle_bus_post_dispatch(httpd_req_t *req)
{
    if (check_admin_auth(req) != ESP_OK) return send_error(req, 401, "Admin token required");
    if (strstr(req->uri, "/api/bus/i2c/setup")) return handle_post_bus_i2c_setup(req);
    if (strstr(req->uri, "/api/bus/i2c/scan")) return handle_post_bus_i2c_scan(req);
    if (strstr(req->uri, "/api/bus/i2c/write_read")) return handle_post_bus_i2c_write_read(req);
    if (strstr(req->uri, "/api/bus/i2c/write")) return handle_post_bus_i2c_write(req);
    if (strstr(req->uri, "/api/bus/i2c/read")) return handle_post_bus_i2c_read(req);
    if (strstr(req->uri, "/api/bus/spi/setup")) return handle_post_bus_spi_setup(req);
    if (strstr(req->uri, "/api/bus/spi/transfer")) return handle_post_bus_spi_transfer(req);
    return send_error(req, 404, "Unknown bus endpoint");
}

// POST /api/lshift/oe  body: {"on":true}
esp_err_t handle_post_lshift_oe(httpd_req_t *req)
{
    if (check_admin_auth(req) != ESP_OK) return send_error(req, 401, "Admin token required");
    cJSON *body = recv_json_body(req);
    char *resp = api_core_handle("POST", "/api/lshift/oe", body);
    if (body) cJSON_Delete(body);
    if (!resp) return send_error(req, 500, "lshift failed");
    esp_err_t rc = send_raw_json(req, resp);
    cJSON_free(resp);
    return rc;
}
