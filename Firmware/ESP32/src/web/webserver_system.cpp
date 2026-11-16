// =============================================================================
// webserver_system.cpp - HTTP handlers: standby, WiFi, board profile and pairing.
// Routes are registered in webserver.cpp (initWebServer).
// =============================================================================

#include "webserver_internal.h"

static const char* TAG = "webserver";

// ----- System standby: /api/standby/* (anonymous: status, presence, wake; admin: policy, sleep) -----
// Success is the status object; failure is {"ok":false,"error":...}: 409 when the request
// is refused as busy (a real inhibitor, a full client table, a policy write in progress),
// 500 when a setting could not be stored, 400 for every other rejection.
static esp_err_t send_standby_result(httpd_req_t *req, char *resp)
{
    if (!resp) return send_error(req, 500, "standby request failed");
    int code = 200;
    if (strstr(resp, "\"ok\":false") != NULL) {
        if (strstr(resp, "\"error\":\"busy\"") != NULL) code = 409;
        else if (strstr(resp, "\"error\":\"storage\"") != NULL) code = 500;
        else code = 400;
    }
    esp_err_t rc = send_raw_json(req, resp, code);
    cJSON_free(resp);
    return rc;
}

esp_err_t handle_get_standby_status(httpd_req_t *req)
{
    return send_standby_result(req, api_core_handle("GET", "/api/standby/status", NULL));
}

esp_err_t handle_post_standby_presence(httpd_req_t *req)
{
    cJSON *body = recv_json_body_cap(req, 128);
    char *resp = api_core_handle("POST", "/api/standby/presence", body);
    if (body) cJSON_Delete(body);
    return send_standby_result(req, resp);
}

esp_err_t handle_post_standby_wake(httpd_req_t *req)
{
    return send_standby_result(req, api_core_handle("POST", "/api/standby/wake", NULL));
}

esp_err_t handle_post_standby_policy(httpd_req_t *req)
{
    if (check_admin_auth(req) != ESP_OK) return send_error(req, 401, "Admin token required");
    cJSON *body = recv_json_body_cap(req, 128);
    char *resp = api_core_handle("POST", "/api/standby/policy", body);
    if (body) cJSON_Delete(body);
    return send_standby_result(req, resp);
}

esp_err_t handle_post_standby_sleep(httpd_req_t *req)
{
    if (check_admin_auth(req) != ESP_OK) return send_error(req, 401, "Admin token required");
    return send_standby_result(req, api_core_handle("POST", "/api/standby/sleep", NULL));
}

// POST /api/daq/config  body: {"op": 0..4, "args": hex}. SET (1) and ACTION (4)
// change device state and need the admin token; GET/GET_ALL/SCHEMA do not.
esp_err_t handle_post_daq_config(httpd_req_t *req)
{
    cJSON *body = recv_json_body(req);
    cJSON *jop = body ? cJSON_GetObjectItem(body, "op") : NULL;
    int op = cJSON_IsNumber(jop) ? jop->valueint : -1;
    if ((op == 1 || op == 4) && check_admin_auth(req) != ESP_OK) {
        cJSON_Delete(body);
        return send_error(req, 401, "Admin token required");
    }
    char *resp = api_core_handle("POST", "/api/daq/config", body);
    if (body) cJSON_Delete(body);
    return send_api_core_result(req, resp, "daq config failed");
}

// GET /api/hat/la/status
esp_err_t handle_get_hat_la_status(httpd_req_t *req)
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
    if (check_admin_auth(req) != ESP_OK) return send_error(req, 401, "Admin token required");
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
    if (check_admin_auth(req) != ESP_OK) return send_error(req, 401, "Admin token required");
    bool ok = hat_reset();
    cJSON *rsp = cJSON_CreateObject();
    cJSON_AddBoolToObject(rsp, "ok", ok);
    return send_json(req, rsp);
}

// POST /api/hat/detect
static esp_err_t handle_post_hat_detect(httpd_req_t *req)
{
    if (check_admin_auth(req) != ESP_OK) return send_error(req, 401, "Admin token required");

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
    return send_json(req, rsp);
}

// POST /api/hat dispatch
esp_err_t handle_hat_post_dispatch(httpd_req_t *req)
{
    if (strstr(req->uri, "/api/hat/config")) return handle_post_hat_config(req);
    if (strstr(req->uri, "/api/hat/reset")) return handle_post_hat_reset(req);
    if (strstr(req->uri, "/api/hat/detect")) return handle_post_hat_detect(req);
    if (strstr(req->uri, "/api/hat/v2/swd/detect")) {
        if (check_admin_auth(req) != ESP_OK) return send_error(req, 401, "Admin token required");
        char *resp = api_core_handle("POST", "/api/hat/v2/swd/detect", NULL);
        if (!resp) return send_error(req, 500, "SWD detect failed");
        esp_err_t rc = send_raw_json(req, resp);
        cJSON_free(resp);
        return rc;
    }
    return send_error(req, 404, "Unknown HAT endpoint");
}


// =============================================================================
// WiFi Status & Connect
// =============================================================================

// GET /api/wifi
esp_err_t handle_get_wifi(httpd_req_t *req)
{
    char *resp = api_core_handle("GET", "/api/wifi", NULL);
    if (!resp) return send_error(req, 500, "wifi failed");
    esp_err_t rc = send_raw_json(req, resp);
    cJSON_free(resp);
    return rc;
}

// POST /api/wifi/connect  body: {"ssid":"...", "password":"..."}
esp_err_t handle_post_wifi_connect(httpd_req_t *req)
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
esp_err_t handle_get_wifi_scan(httpd_req_t *req)
{
    wifi_scan_result_t results[20];
    int count = wifi_scan(results, 20);
    return send_json(req, api_core_wifi_scan_json(results, count));
}

// POST /api/wifi/ap_password  body: {"password":"..."}  (admin auth required)
esp_err_t handle_post_wifi_ap_password(httpd_req_t *req)
{
    if (check_admin_auth(req) != ESP_OK) {
        return send_error(req, 401, "Admin token required");
    }

    cJSON *body = recv_json_body(req);
    if (!body) return send_error(req, 400, "Invalid JSON");

    cJSON *j_pass = cJSON_GetObjectItem(body, "password");
    if (!j_pass || !cJSON_IsString(j_pass)) {
        cJSON_Delete(body);
        return send_error(req, 400, "Missing password");
    }
    const char *pass = j_pass->valuestring;
    size_t plen = strlen(pass);
    if (plen < 8 || plen > 63) {
        cJSON_Delete(body);
        return send_error(req, 400, "Password must be 8-63 characters (WPA2 requirement)");
    }

    bool persisted = false;
    bool ok = wifi_set_ap_password(pass, &persisted);
    cJSON_Delete(body);

    if (!ok) return send_error(req, 500, "Failed to set AP password");

    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "success", true);
    cJSON_AddBoolToObject(root, "persisted", persisted);
    cJSON_AddStringToObject(root, "message", persisted
        ? "AP password updated and applied live"
        : "AP password applied live but NVS write failed; will revert after reboot");
    return send_json(req, root);
}

// GET /api/wifi/hostname  → {"hostname":"bugbuster-a1b2c3"}
esp_err_t handle_get_wifi_hostname(httpd_req_t *req)
{
    char hn[MDNS_HOSTNAME_MAX] = {};
    mdns_responder_get_hostname(hn, sizeof(hn));
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "hostname", hn);
    return send_json(req, root);
}

// POST /api/wifi/hostname  body: {"hostname":"benchA"}  (admin auth required)
// Pass empty string or omit field to revert to the auto-derived default.
esp_err_t handle_post_wifi_hostname(httpd_req_t *req)
{
    if (check_admin_auth(req) != ESP_OK) {
        return send_error(req, 401, "Admin token required");
    }

    cJSON *body = recv_json_body(req);
    if (!body) return send_error(req, 400, "Invalid JSON");

    cJSON *j_hn = cJSON_GetObjectItem(body, "hostname");
    const char *hn = (j_hn && cJSON_IsString(j_hn)) ? j_hn->valuestring : "";

    bool ok = mdns_responder_set_hostname(hn);
    cJSON_Delete(body);

    if (!ok) {
        return send_error(req, 400,
            "Hostname must be 1-31 chars, lowercase letters/digits/'-', "
            "and must not begin or end with '-'");
    }

    char applied[MDNS_HOSTNAME_MAX] = {};
    mdns_responder_get_hostname(applied, sizeof(applied));

    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "success", true);
    cJSON_AddStringToObject(root, "hostname", applied);
    return send_json(req, root);
}

// GET /api/device/version
esp_err_t handle_get_version(httpd_req_t *req)
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
    cJSON_AddStringToObject(root, "webBuild", BUGBUSTER_WEB_BUILD_MARKER);
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

// Parse a 64-char hex SHA-256 from a query string. Returns true and fills
// @p out (32 bytes) when present and well-formed; false when absent.
bool parse_sha256_query(httpd_req_t *req, uint8_t out[32], bool *out_present)
{
    *out_present = false;
    char qbuf[160] = {};
    if (httpd_req_get_url_query_str(req, qbuf, sizeof(qbuf)) != ESP_OK) return true;
    char hex[80] = {};
    if (httpd_query_key_value(qbuf, "sha256", hex, sizeof(hex)) != ESP_OK) return true;
    size_t n = strlen(hex);
    if (n != 64) return false;
    for (int i = 0; i < 32; i++) {
        unsigned int byte;
        if (sscanf(hex + i*2, "%2x", &byte) != 1) return false;
        out[i] = (uint8_t)byte;
    }
    *out_present = true;
    return true;
}

// POST /api/ota/upload — OTA firmware update (binary body = firmware.bin)
//   Optional ?sha256=<64-hex>  — when present, SHA-256 of the body is computed
//   on the fly and compared before set_boot_partition; mismatch aborts the
//   write and returns 400 without changing the boot target.
esp_err_t handle_ota_upload(httpd_req_t *req)
{
    if (check_admin_auth(req) != ESP_OK) {
        return send_error(req, 401, "Admin token required");
    }

    ESP_LOGI(TAG, "OTA upload started, content_len=%d", req->content_len);

    if (req->content_len <= 0 || req->content_len > 3584 * 1024) {
        return send_error(req, 400, "Invalid firmware size (max 3.5MB)");
    }

    uint8_t expected_sha[32] = {};
    bool expect_sha = false;
    if (!parse_sha256_query(req, expected_sha, &expect_sha)) {
        return send_error(req, 400, "sha256 query must be 64 lowercase hex chars");
    }

    const esp_partition_t *update_partition = esp_ota_get_next_update_partition(NULL);
    if (!update_partition) {
        return send_error(req, 500, "No OTA partition available");
    }

    ESP_LOGI(TAG, "Writing to partition '%s' at 0x%lx, size %lu (sha256=%s)",
             update_partition->label, (unsigned long)update_partition->address,
             (unsigned long)update_partition->size, expect_sha ? "yes" : "no");

    esp_ota_handle_t ota_handle;
    esp_err_t err = esp_ota_begin(update_partition, OTA_WITH_SEQUENTIAL_WRITES, &ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_begin failed: %s", esp_err_to_name(err));
        return send_error(req, 500, esp_err_to_name(err));
    }

    mbedtls_sha256_context sha_ctx;
    if (expect_sha) {
        mbedtls_sha256_init(&sha_ctx);
        mbedtls_sha256_starts(&sha_ctx, 0 /* SHA-256, not 224 */);
    }

    // Stream firmware data in chunks
    char *buf = (char*)malloc(4096);
    if (!buf) {
        if (expect_sha) mbedtls_sha256_free(&sha_ctx);
        esp_ota_abort(ota_handle);
        return send_error(req, 500, "Out of memory");
    }

    int remaining = req->content_len;
    int total_written = 0;
    bool failed = false;

    while (remaining > 0) {
        int to_read = (remaining > 4096) ? 4096 : remaining;
        int received = upload_recv(req, buf, to_read);
        if (received <= 0) {
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
        if (expect_sha) {
            mbedtls_sha256_update(&sha_ctx, (const unsigned char*)buf, received);
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
        if (expect_sha) mbedtls_sha256_free(&sha_ctx);
        esp_ota_abort(ota_handle);
        return send_error(req, 500, "OTA write failed");
    }

    if (expect_sha) {
        uint8_t actual[32] = {};
        mbedtls_sha256_finish(&sha_ctx, actual);
        mbedtls_sha256_free(&sha_ctx);
        if (memcmp(actual, expected_sha, 32) != 0) {
            ESP_LOGW(TAG, "OTA SHA-256 mismatch — aborting before set_boot_partition");
            esp_ota_abort(ota_handle);
            return send_error(req, 400, "SHA-256 mismatch — image discarded, boot target unchanged");
        }
        ESP_LOGI(TAG, "OTA SHA-256 verified (%d bytes)", total_written);
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
    cJSON_AddBoolToObject(root, "sha256Verified", expect_sha);
    esp_err_t ret = send_json(req, root);

    // Reboot after a short delay to let the HTTP response flush
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();

    return ret;
}

// POST /api/ota/upload_rp2040 — Upload RP2040 firmware update (binary body = bugbuster_hat.bin)
//   Saves file directly to /scripts/update-rp2040.bin, gated by admin token.
esp_err_t handle_rp2040_upload(httpd_req_t *req)
{
    if (check_admin_auth(req) != ESP_OK) {
        return send_error(req, 401, "Admin token required");
    }

    ESP_LOGI(TAG, "RP2040 upload started, content_len=%d", req->content_len);

    if (req->content_len <= 0 || req->content_len > 1024 * 1024) {
        return send_error(req, 400, "Invalid RP2040 firmware size (max 1MB)");
    }

    const char *stage_path = "/scripts/update-rp2040.bin";
    remove(stage_path);
    FILE *f = fopen(stage_path, "wb");
    if (!f) {
        ESP_LOGE(TAG, "Failed to open staging path %s for writing", stage_path);
        return send_error(req, 500, "Failed to open stage file");
    }

    char *buf = (char*)malloc(4096);
    if (!buf) {
        fclose(f);
        return send_error(req, 500, "Out of memory");
    }

    int remaining = req->content_len;
    int total_written = 0;
    bool failed = false;

    while (remaining > 0) {
        int to_read = (remaining > 4096) ? 4096 : remaining;
        int received = upload_recv(req, buf, to_read);
        if (received <= 0) {
            ESP_LOGE(TAG, "RP2040 upload receive error at %d/%d bytes", total_written, req->content_len);
            failed = true;
            break;
        }

        if (fwrite(buf, 1, received, f) != (size_t)received) {
            ESP_LOGE(TAG, "RP2040 upload write failed at %d bytes", total_written);
            failed = true;
            break;
        }

        remaining -= received;
        total_written += received;
    }

    free(buf);
    fclose(f);

    if (failed) {
        remove(stage_path);
        return send_error(req, 500, "RP2040 upload failed");
    }

    ESP_LOGI(TAG, "RP2040 upload success! %d bytes written to %s", total_written, stage_path);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "success", true);
    cJSON_AddNumberToObject(root, "bytesWritten", total_written);
    return send_json(req, root);
}


// =============================================================================
// Board profile + pairing handlers
// =============================================================================

// GET /api/board → { active: string|null, available: [ BoardProfile... ] }
esp_err_t handle_get_board(httpd_req_t *req)
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
esp_err_t handle_post_board_select(httpd_req_t *req)
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
esp_err_t handle_get_pairing_info(httpd_req_t *req)
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
esp_err_t handle_post_pairing_verify(httpd_req_t *req)
{
    if (check_admin_auth(req) != ESP_OK) {
        return send_error(req, 401, "Unauthorized");
    }
    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", true);
    return send_json(req, root);
}

// POST /api/pairing/rotate  (admin-token header)
// Generates a fresh 64-char hex admin token, overwrites the previous one in
// NVS, and returns the new token in the response body. The caller (web UI)
// is expected to immediately stash it in its own cache — the old token is
// already invalid by the time this returns 200, so any device the user has
// previously paired must re-pair manually.
esp_err_t handle_post_pairing_rotate(httpd_req_t *req)
{
    if (check_admin_auth(req) != ESP_OK) {
        return send_error(req, 401, "Unauthorized");
    }
    char fresh[65] = {0};
    if (!auth_rotate_token(fresh)) {
        return send_error(req, 500, "Token rotation failed");
    }
    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", true);
    cJSON_AddStringToObject(root, "token", fresh);
    return send_json(req, root);
}
