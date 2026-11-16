// =============================================================================
// webserver_scripts.cpp - HTTP handlers: on-device scripting.
// Routes are registered in webserver.cpp (initWebServer).
// =============================================================================

#include "webserver_internal.h"


// =============================================================================
// Scripting endpoints
// =============================================================================

#define SCRIPTS_EVAL_MAX_BYTES 32768

// Scripts routes are served by api_core_handle() (api_scripts.cpp) so BLE and
// HTTP cannot drift. {"running":"<name>"} = the single file-script slot is
// taken -> 409; any other {"error"} -> 400; else 200.
static esp_err_t send_scripts_result(httpd_req_t *req, char *resp)
{
    if (!resp) return send_error(req, 500, "scripts request failed");
    cJSON *parsed = cJSON_Parse(resp);
    int code = 200;
    if (parsed && cJSON_IsString(cJSON_GetObjectItem(parsed, "running"))) code = 409;
    else if (parsed && cJSON_GetObjectItem(parsed, "error")) code = 400;
    if (parsed) cJSON_Delete(parsed);
    esp_err_t rc = send_raw_json(req, resp, code);
    cJSON_free(resp);
    return rc;
}

// Re-frame an api_scripts {"data": base64} reply as raw bytes for HTTP clients
// that predate the JSON shape (web console, Python client). Logic stays shared.
static esp_err_t send_scripts_b64_as(httpd_req_t *req, char *resp, const char *ctype, bool log_cursor)
{
    if (!resp) return send_error(req, 500, "scripts request failed");
    cJSON *j = cJSON_Parse(resp);
    cJSON_free(resp);
    if (!j) return send_error(req, 500, "scripts request failed");
    cJSON *err = cJSON_GetObjectItem(j, "error");
    if (cJSON_IsString(err)) {
        esp_err_t rc = send_error(req, strcmp(err->valuestring, "script not found") == 0 ? 404 : 400,
                                  err->valuestring);
        cJSON_Delete(j);
        return rc;
    }
    cJSON *data = cJSON_GetObjectItem(j, "data");
    const char *b64 = cJSON_IsString(data) ? data->valuestring : "";
    size_t blen = strlen(b64), olen = 0, cap = blen / 4 * 3 + 4;
    unsigned char *raw = (unsigned char *)heap_caps_malloc(cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!raw || mbedtls_base64_decode(raw, cap, &olen, (const unsigned char *)b64, blen) != 0) {
        if (raw) heap_caps_free(raw);
        cJSON_Delete(j);
        return send_error(req, 500, "decode failed");
    }
    char origin_buf[96];
    set_cors_headers(req, origin_buf, sizeof(origin_buf));
    httpd_resp_set_type(req, ctype);
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    char next_hdr[24];
    cJSON *next = cJSON_GetObjectItem(j, "next");
    if (log_cursor && cJSON_IsNumber(next)) {
        snprintf(next_hdr, sizeof(next_hdr), "%.0f", next->valuedouble);
        httpd_resp_set_hdr(req, "X-BugBuster-Log-Next", next_hdr);
        httpd_resp_set_hdr(req, "Access-Control-Expose-Headers", "X-BugBuster-Log-Next");
    }
    esp_err_t rc = httpd_resp_send(req, (const char *)raw, (ssize_t)olen);
    heap_caps_free(raw);
    cJSON_Delete(j);
    return rc;
}

// POST /api/scripts/eval[?persist=true] — raw Python body (<= 32 KB). Wrapped
// as {"src": ...} for the shared api_scripts eval (409 while a script runs).
esp_err_t handle_post_scripts_eval(httpd_req_t *req)
{
    if (check_admin_auth(req) != ESP_OK) return send_error(req, 401, "Admin token required");
    int total = req->content_len;
    if (total <= 0 || total > SCRIPTS_EVAL_MAX_BYTES) {
        return send_error(req, 400, "Body must be 1-32768 bytes of Python source");
    }
    char *src = (char *)heap_caps_malloc(total + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!src) return send_error(req, 500, "Out of memory");
    int received = 0;
    while (received < total) {
        int ret = upload_recv(req, src + received, total - received);
        if (ret <= 0) {
            heap_caps_free(src);
            return send_error(req, 500, "Receive error");
        }
        received += ret;
    }
    src[total] = '\0';
    cJSON *body = cJSON_CreateObject();
    cJSON_AddStringToObject(body, "src", src);
    heap_caps_free(src);
    char *resp = api_core_handle("POST", req->uri, body);
    cJSON_Delete(body);
    return send_scripts_result(req, resp);
}

// POST /api/scripts/lint
esp_err_t handle_post_scripts_lint(httpd_req_t *req)
{
    if (check_admin_auth(req) != ESP_OK) return send_error(req, 401, "Admin token required");

    int total = req->content_len;
    if (total < 0 || total > SCRIPTS_EVAL_MAX_BYTES) {
        return send_error(req, 400, "Body must be 0-32768 bytes of Python source");
    }

    char *src = (char*)malloc(total + 1);
    if (!src) return send_error(req, 500, "Out of memory");

    int received = 0;
    while (received < total) {
        int ret = upload_recv(req, src + received, total - received);
        if (ret <= 0) {
            free(src);
            return send_error(req, 500, "Receive error");
        }
        received += ret;
    }
    src[total] = '\0';

    char err_buf[512] = {0};
    bool ok = scripting_lint_string(src, (size_t)total, err_buf, sizeof(err_buf));
    free(src);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", ok);
    if (!ok) {
        cJSON_AddStringToObject(root, "err", err_buf);
    }
    return send_json(req, root);
}

// GET /api/scripts/logs[?since=N] — text/plain (+ X-BugBuster-Log-Next with since)
esp_err_t handle_get_scripts_logs(httpd_req_t *req)
{
    if (check_admin_auth(req) != ESP_OK) return send_error(req, 401, "Admin token required");
    return send_scripts_b64_as(req, api_core_handle("GET", req->uri, NULL), "text/plain", true);
}

// GET /api/scripts/status
esp_err_t handle_get_scripts_status(httpd_req_t *req)
{
    if (check_admin_auth(req) != ESP_OK) return send_error(req, 401, "Admin token required");
    return send_scripts_result(req, api_core_handle("GET", req->uri, NULL));
}

// POST /api/scripts/reset — reset the persistent VM
esp_err_t handle_post_scripts_reset(httpd_req_t *req)
{
    if (check_admin_auth(req) != ESP_OK) return send_error(req, 401, "Admin token required");

    scripting_reset_vm();

    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", true);
    return send_json(req, root);
}

// POST /api/scripts/stop
esp_err_t handle_post_scripts_stop(httpd_req_t *req)
{
    if (check_admin_auth(req) != ESP_OK) return send_error(req, 401, "Admin token required");
    return send_scripts_result(req, api_core_handle("POST", req->uri, NULL));
}

// POST /api/scripts/files  — upload a script file
// Body: raw Python source.  Query param: name=<filename>
esp_err_t handle_post_scripts_files(httpd_req_t *req)
{
    if (check_admin_auth(req) != ESP_OK) return send_error(req, 401, "Admin token required");

    // Extract "name" query parameter
    char query[256] = {0};
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK) {
        return send_error(req, 400, "Missing query string");
    }
    char name[SCRIPT_NAME_MAX + 1] = {0};
    if (httpd_query_key_value(query, "name", name, sizeof(name)) != ESP_OK) {
        return send_error(req, 400, "Missing 'name' query parameter");
    }
    if (!script_storage_validate_name(name)) {
        return send_error(req, 400, "Invalid script name");
    }

    int total = req->content_len;
    if (total <= 0 || total > SCRIPT_BODY_MAX) {
        return send_error(req, 400, "Body must be 1-32768 bytes of Python source");
    }

    uint8_t *buf = (uint8_t *)malloc(total);
    if (!buf) return send_error(req, 500, "Out of memory");

    int received = 0;
    while (received < total) {
        int ret = upload_recv(req, (char *)buf + received, total - received);
        if (ret <= 0) {
            free(buf);
            return send_error(req, 500, "Receive error");
        }
        received += ret;
    }

    char err[80] = {0};
    bool ok = script_storage_save(name, buf, (size_t)total, err, sizeof(err));
    free(buf);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", ok);
    if (!ok) cJSON_AddStringToObject(root, "err", err);
    return send_json(req, root);
}

// GET /api/scripts/files  — list stored scripts
esp_err_t handle_get_scripts_files(httpd_req_t *req)
{
    if (check_admin_auth(req) != ESP_OK) return send_error(req, 401, "Admin token required");
    return send_scripts_result(req, api_core_handle("GET", req->uri, NULL));
}

// GET /api/scripts/storage — SPIFFS/script storage telemetry (shared with BLE via api_scripts.cpp)
esp_err_t handle_get_scripts_storage(httpd_req_t *req)
{
    if (check_admin_auth(req) != ESP_OK) return send_error(req, 401, "Admin token required");
    char *resp = api_core_handle("GET", "/api/scripts/storage", NULL);
    if (!resp) return send_error(req, 500, "SPIFFS info unavailable");
    if (strstr(resp, "\"error\"")) {
        cJSON_free(resp);
        return send_error(req, 500, "SPIFFS info unavailable");
    }
    esp_err_t rc = send_raw_json(req, resp);
    cJSON_free(resp);
    return rc;
}

// GET /api/scripts/files/get?name=X — whole file as text/x-python (one page)
esp_err_t handle_get_scripts_file(httpd_req_t *req)
{
    if (check_admin_auth(req) != ESP_OK) return send_error(req, 401, "Admin token required");
    char path[192];
    int n = snprintf(path, sizeof(path), "%s%slen=%u", req->uri,
                     strchr(req->uri, '?') ? "&" : "?", (unsigned)SCRIPT_BODY_MAX);
    if (n <= 0 || (size_t)n >= sizeof(path)) return send_error(req, 400, "query string too long");
    return send_scripts_b64_as(req, api_core_handle("GET", path, NULL),
                               "text/x-python; charset=utf-8", false);
}

// DELETE /api/scripts/files?name=X — same operation as POST /api/scripts/files/delete
esp_err_t handle_delete_scripts_file(httpd_req_t *req)
{
    if (check_admin_auth(req) != ESP_OK) return send_error(req, 401, "Admin token required");
    const char *q = strchr(req->uri, '?');
    char path[160];
    int n = snprintf(path, sizeof(path), "/api/scripts/files/delete%s", q ? q : "");
    if (n <= 0 || (size_t)n >= sizeof(path)) return send_error(req, 400, "query string too long");
    return send_scripts_result(req, api_core_handle("POST", path, NULL));
}

// GET /api/scripts/autorun/status
esp_err_t handle_get_autorun_status(httpd_req_t *req)
{
    if (check_admin_auth(req) != ESP_OK) return send_error(req, 401, "Admin token required");
    return send_scripts_result(req, api_core_handle("GET", req->uri, NULL));
}

// POST /api/scripts/autorun/enable  — Query param: name=<filename>
esp_err_t handle_post_autorun_enable(httpd_req_t *req)
{
    if (check_admin_auth(req) != ESP_OK) return send_error(req, 401, "Admin token required");
    cJSON *body = recv_json_body(req);
    char *resp = api_core_handle("POST", req->uri, body);
    if (body) cJSON_Delete(body);
    return send_scripts_result(req, resp);
}

// POST /api/scripts/autorun/disable
esp_err_t handle_post_autorun_disable(httpd_req_t *req)
{
    if (check_admin_auth(req) != ESP_OK) return send_error(req, 401, "Admin token required");
    return send_scripts_result(req, api_core_handle("POST", req->uri, NULL));
}

// POST /api/scripts/run-file?name=X[&background=1][&replace=1]  (409 when busy)
esp_err_t handle_post_scripts_run_file(httpd_req_t *req)
{
    if (check_admin_auth(req) != ESP_OK) return send_error(req, 401, "Admin token required");
    cJSON *body = recv_json_body(req);
    char *resp = api_core_handle("POST", req->uri, body);
    if (body) cJSON_Delete(body);
    return send_scripts_result(req, resp);
}

// POST /api/scripts/files/chunk  body {"name","off","b64","final"}
esp_err_t handle_post_scripts_files_chunk(httpd_req_t *req)
{
    if (check_admin_auth(req) != ESP_OK) return send_error(req, 401, "Admin token required");
    cJSON *body = recv_json_body_cap(req, 6144);
    char *resp = api_core_handle("POST", req->uri, body);
    if (body) cJSON_Delete(body);
    return send_scripts_result(req, resp);
}

// POST /api/scripts/files/delete  (BLE spelling of DELETE /api/scripts/files)
esp_err_t handle_post_scripts_files_delete(httpd_req_t *req)
{
    if (check_admin_auth(req) != ESP_OK) return send_error(req, 401, "Admin token required");
    cJSON *body = recv_json_body(req);
    char *resp = api_core_handle("POST", req->uri, body);
    if (body) cJSON_Delete(body);
    return send_scripts_result(req, resp);
}
