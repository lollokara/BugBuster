// =============================================================================
// webserver.cpp - HTTP API server for AD74416H controller (ESP-IDF httpd)
// =============================================================================

#include "webserver_internal.h"

static const char* TAG = "webserver";

static httpd_handle_t s_server = NULL;
// M06: guard s_server read/write — concurrent webserver_start() calls (e.g.
// recovery path + manual trigger) can race on this handle.  portMUX_TYPE
// matches the pattern used elsewhere in this codebase (see board_profile.cpp).
static portMUX_TYPE s_server_mux = portMUX_INITIALIZER_UNLOCKED;

#ifndef BUGBUSTER_TRACE_URI_REGISTRATION
#define BUGBUSTER_TRACE_URI_REGISTRATION 0
#endif

// Original handler of a route plus the context it was registered with.
struct StandbyRouteWrap {
    esp_err_t (*handler)(httpd_req_t *);
    void *ctx;
};
static esp_err_t standby_trampoline(httpd_req_t *req);

static esp_err_t register_uri_handler_checked(httpd_handle_t server, const httpd_uri_t *uri, int line)
{
    const char *path = (uri && uri->uri) ? uri->uri : "<null>";
    int method = uri ? (int)uri->method : -1;
#if BUGBUSTER_TRACE_URI_REGISTRATION
    term_printf("[webserver] register line=%d method=%d uri=%s\r\n", line, method, path);
#endif
    // Every route runs behind the standby operation barrier: keep the original
    // handler/context and register the trampoline instead (see standby_trampoline).
    if (!uri || !uri->handler) return ESP_ERR_INVALID_ARG;
    StandbyRouteWrap *wrap = (StandbyRouteWrap *)heap_caps_malloc(sizeof(StandbyRouteWrap),
                                                                   MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!wrap) wrap = (StandbyRouteWrap *)malloc(sizeof(StandbyRouteWrap));
    if (!wrap) {
        ESP_LOGE(TAG, "URI register skipped (no memory for barrier wrapper) line=%d uri=%s", line, path);
        return ESP_ERR_NO_MEM;
    }
    wrap->handler = uri->handler;
    wrap->ctx = uri->user_ctx;
    httpd_uri_t wrapped = *uri;
    wrapped.handler = standby_trampoline;
    wrapped.user_ctx = wrap;
    esp_err_t err = httpd_register_uri_handler(server, &wrapped);
#if BUGBUSTER_TRACE_URI_REGISTRATION
    term_printf("[webserver] register result line=%d err=%d uri=%s\r\n", line, (int)err, path);
#endif
    if (err != ESP_OK) {
        free(wrap);   // not referenced by any route
        ESP_LOGE(TAG, "URI register failed line=%d method=%d uri=%s err=%s",
                 line, method, path, esp_err_to_name(err));
    }
    return err;
}

#define httpd_register_uri_handler(server, uri) register_uri_handler_checked((server), (uri), __LINE__)

static const char* http_status_string(int code)
{
    switch (code) {
        case 200: return "200 OK";
        case 400: return "400 Bad Request";
        case 401: return "401 Unauthorized";
        case 404: return "404 Not Found";
        case 405: return "405 Method Not Allowed";
        case 409: return "409 Conflict";
        case 500: return "500 Internal Server Error";
        case 503: return "503 Service Unavailable";
        default:  return NULL;
    }
}

// IOS-23: camelCase only (see api_core.cpp); the second argument keeps the
// retired snake_case name greppable.
void add_number_alias(cJSON *obj, const char *camel, const char * /*retired*/, double value)
{
    cJSON_AddNumberToObject(obj, camel, value);
}

void add_bool_alias(cJSON *obj, const char *camel, const char * /*retired*/, bool value)
{
    cJSON_AddBoolToObject(obj, camel, value);
}

// -----------------------------------------------------------------------------
// Helper: CORS headers
// -----------------------------------------------------------------------------

// IMPORTANT: ESP-IDF httpd_resp_set_hdr() stores the *pointer* to the value,
// not a copy. The value buffer must outlive the eventual httpd_resp_send*()
// call. The Origin string therefore cannot live in this function's stack
// frame — it must be owned by the caller's frame, which spans the send. The
// caller passes in a `char origin_buf[96]` (use SET_CORS_HEADERS macro for
// boilerplate) so the buffer outlives the headers it backs.
void set_cors_headers(httpd_req_t *req, char *origin_buf, size_t origin_buf_size)
{
    // The on-device UI is always served same-origin from this same httpd, so
    // it does not need any Access-Control-Allow-Origin header to make
    // authenticated XHRs. We deliberately do NOT emit a wildcard origin: that
    // combined with the X-BugBuster-Admin-Token header was effectively a
    // CSRF amplifier (any browser context could mint admin requests).
    //
    // For convenience during local UI development against a flashed device
    // (e.g. running `vite dev` on the same machine and proxying), echo the
    // request Origin only if it is a localhost loopback. Anything else is
    // intentionally rejected by the browser's same-origin policy because we
    // simply never advertise it as allowed.
    if (origin_buf && origin_buf_size > 0) {
        origin_buf[0] = '\0';
        if (httpd_req_get_hdr_value_str(req, "Origin", origin_buf, origin_buf_size) == ESP_OK) {
            if (cors_origin_allowed(origin_buf)) {
                httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", origin_buf);
                httpd_resp_set_hdr(req, "Vary", "Origin");
            }
        }
    }
    httpd_resp_set_hdr(req, "Access-Control-Allow-Methods", "GET, POST, OPTIONS");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Headers", "Content-Type, " ADMIN_TOKEN_HEADER);
}

// Callers must supply a stack buffer with lifetime spanning the
// httpd_resp_send*() call:
//     char origin_buf[96];
//     set_cors_headers(req, origin_buf, sizeof(origin_buf));

// -----------------------------------------------------------------------------
// Helper: Check Admin Authentication
// -----------------------------------------------------------------------------

esp_err_t check_admin_auth(httpd_req_t *req)
{
    autorun_note_inbound();
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

// TR-11b: a failed action used to answer 200 {"ok":false}, which clients
// that only look at the status code took for success. A non-GET reply whose
// top-level "ok" is false is now sent as 400 (body unchanged). GET replies
// keep 200: there "ok":false is status data (e.g. no HAT fitted).
static int logical_failure_status(httpd_req_t *req, const cJSON *root, int code)
{
    if (code != 200 || req->method == HTTP_GET || !root) return code;
    return cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(root, "ok")) ? 400 : code;
}

esp_err_t send_json(httpd_req_t *req, cJSON *root, int code)
{
    code = logical_failure_status(req, root, code);
    char *body = cJSON_PrintUnformatted(root);
    if (!body) {
        cJSON_Delete(root);
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "JSON encode failed");
    }
    char origin_buf[96];
    set_cors_headers(req, origin_buf, sizeof(origin_buf));
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

// Consecutive recv timeouts (httpd recv_wait_timeout, 5 s default) tolerated
// before an upload is abandoned, so a stalled client cannot hold the httpd task.
#define UPLOAD_RECV_MAX_TIMEOUTS 5

static bool recv_timeout_retry(int *timeouts)
{
    return ++(*timeouts) <= UPLOAD_RECV_MAX_TIMEOUTS;
}

int upload_recv(httpd_req_t *req, char *buf, size_t len)
{
    int timeouts = 0;
    for (;;) {
        int n = httpd_req_recv(req, buf, len);
        if (n == HTTPD_SOCK_ERR_TIMEOUT && recv_timeout_retry(&timeouts)) continue;
        if (n == HTTPD_SOCK_ERR_TIMEOUT) ESP_LOGW(TAG, "upload stalled, aborting %s", req->uri);
        return n;
    }
}

esp_err_t send_raw_json(httpd_req_t *req, const char *body, int code)
{
    if (code == 200 && req->method != HTTP_GET && body && strstr(body, "\"ok\":false")) {
        cJSON *parsed = cJSON_Parse(body);
        code = logical_failure_status(req, parsed, code);
        cJSON_Delete(parsed);
    }
    char origin_buf[96];
    set_cors_headers(req, origin_buf, sizeof(origin_buf));
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    if (code != 200) {
        const char *status = http_status_string(code);
        if (status) {
            httpd_resp_set_status(req, status);
        }
    }
    return httpd_resp_sendstr(req, body ? body : "{}");
}

// -----------------------------------------------------------------------------
// Helper: send JSON error
// -----------------------------------------------------------------------------

esp_err_t send_error(httpd_req_t *req, int code, const char *msg)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "error", msg);
    return send_json(req, root, code);
}

// Forward declaration: defined further down alongside the DAQ handlers, but
// used by earlier handlers that delegate to api_core_handle().

// 503 for a request refused by the standby barrier. The wake has already been
// requested by the refusal; the request is NOT replayed, the client retries.
static esp_err_t send_standby_busy(httpd_req_t *req)
{
    char *body = api_standby_busy_json();
    char origin_buf[96];
    set_cors_headers(req, origin_buf, sizeof(origin_buf));
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Retry-After", "2");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    httpd_resp_set_status(req, "503 Service Unavailable");
    esp_err_t rc = httpd_resp_sendstr(req, body ? body : "{\"ok\":false,\"error\":\"standby\"}");
    if (body) cJSON_free(body);
    return rc;
}

// Operation barrier for every route registered through the checked helper.
// Static assets and cached status reads pass untouched; the standby routes
// manage their own admission; everything else is counted work and wakes a
// sleeping system instead of touching unpowered hardware.
static esp_err_t standby_trampoline(httpd_req_t *req)
{
    StandbyRouteWrap *wrap = (StandbyRouteWrap *)req->user_ctx;
    req->user_ctx = wrap->ctx;
    if (standby_api_http_class(req->method == HTTP_GET, req->uri) != STANDBY_HTTP_WORK)
        return wrap->handler(req);
    StandbyWork work;
    if (!work.ok()) return send_standby_busy(req);
    return wrap->handler(req);
}

static esp_err_t handle_http_error(httpd_req_t *req, httpd_err_code_t error)
{
    if (error == HTTPD_404_NOT_FOUND) {
        ESP_LOGW(TAG, "404 for %s", req->uri);
        char origin_buf[96];
        set_cors_headers(req, origin_buf, sizeof(origin_buf));
        return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Not found");
    }
    if (error == HTTPD_405_METHOD_NOT_ALLOWED) {
        char origin_buf[96];
        set_cors_headers(req, origin_buf, sizeof(origin_buf));
        // CORS preflight: any OPTIONS to /api/* lands here because we deliberately
        // don't register a wildcard OPTIONS handler (it would shadow the WS upgrade
        // GET on /api/ws/stream and produce a 405 of its own for non-browser
        // clients). Synthesize the 204 preflight response from this err handler.
        if (req->method == HTTP_OPTIONS && strncmp(req->uri, "/api/", 5) == 0) {
            httpd_resp_set_status(req, "204 No Content");
            httpd_resp_send(req, NULL, 0);
            return ESP_OK;
        }
        return httpd_resp_send_err(req, HTTPD_405_METHOD_NOT_ALLOWED, "Method not allowed");
    }

    return httpd_resp_send_err(req, error, NULL);
}

// -----------------------------------------------------------------------------
// Helper: receive and parse JSON body from request
// -----------------------------------------------------------------------------

cJSON* recv_json_body_cap(httpd_req_t *req, int cap)
{
    int total = req->content_len;
    if (total <= 0 || total > cap) return NULL;
    char *buf = (char*)malloc(total + 1);
    if (!buf) return NULL;
    int received = 0;
    while (received < total) {
        int ret = upload_recv(req, buf + received, total - received);
        if (ret <= 0) { free(buf); return NULL; }
        received += ret;
    }
    buf[total] = '\0';
    cJSON *root = cJSON_Parse(buf);
    free(buf);
    return root;
}

cJSON* recv_json_body(httpd_req_t *req)
{
    return recv_json_body_cap(req, 1024);
}

// -----------------------------------------------------------------------------
// Helper: strict JSON field validation
// -----------------------------------------------------------------------------

// VALIDATE_JSON_FIELD is in webserver_internal.h.

// -----------------------------------------------------------------------------
// Helper: extract channel number from URI   /api/channel/X/...
// -----------------------------------------------------------------------------

int extract_channel(const char* uri)
{
    const char *prefix = "/api/channel/";
    const char *p = strstr(uri, prefix);
    if (!p) return -1;
    p += strlen(prefix);
    if (*p >= '0' && *p <= '3') return *p - '0';
    return -1;
}

// -----------------------------------------------------------------------------
// Helper: extract number from /api/faults/clear/X or /api/faults/mask/X
// -----------------------------------------------------------------------------

int extract_fault_channel(const char* uri, const char* prefix)
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

const char* diagSourceName(uint8_t source)
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

const char* diagSourceUnit(uint8_t source)
{
    switch (source) {
        case 1:  return "C";       // Temperature in degrees Celsius
        default: return "V";       // All other sources are voltages
    }
}

// -----------------------------------------------------------------------------
// GPIO mode name helper
// -----------------------------------------------------------------------------

const char* gpioModeName(uint8_t mode) {
    static const char* names[] = {"HIGH_IMP","OUTPUT","INPUT","DIN_OUT","DO_EXT"};
    return mode < 5 ? names[mode] : "?";
}


// =============================================================================
// Server init / stop
// =============================================================================

bool initWebServer(void)
{
    // M06: check-then-act on s_server under the mux to prevent a second
    // concurrent caller from entering httpd_start and corrupting the handle.
    taskENTER_CRITICAL(&s_server_mux);
    bool already_running = (s_server != NULL);
    taskEXIT_CRITICAL(&s_server_mux);
    if (already_running) {
        ESP_LOGW(TAG, "Web server already running");
        return true;
    }

    quicksetup_init();
    update_manager_init();

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    // Keep route capacity close to the real table size. httpd_register_uri_handler
    // call count as of 2026-08-06: 128 in this file + 4 registry routes
    // (http_adapter_register) + 1 WS stream route + 1 REPL WS route = 134.
    // 2026-10-03 recount: 134 httpd_register_uri_handler(s_server) calls in this file
    // (incl. the 2 /api/system/crash routes) + 4 registry + 2 WS = 140 of 150.
    // 128 alone silently starved the last ~6 registrations (registry routes,
    // this file's own wildcard "/*" catch-all) with ESP_ERR_HTTPD_HANDLERS_FULL.
    // 150 gives headroom without reserving a large unused slot table from heap.
    config.max_uri_handlers = 160;
    config.uri_match_fn     = httpd_uri_match_wildcard;
    // HTTPD task stack must stay in internal RAM, not PSRAM. Any handler
    // that touches flash (OTA partition reads, SPIFFS, NVS) goes through
    // spi_flash_disable_interrupts_caches_and_other_cpu(), which asserts
    // the calling task's stack is reachable with the flash cache disabled.
    // PSRAM stacks fail that assert and the httpd worker panics on the
    // first OTA request (observed 2026-05-07: coredump assert failed:
    // esp_task_stack_is_sane_cache_disabled, task=httpd).
    //
    // 4 KB fits the largest contiguous internal block we have at httpd
    // init time (~7 KB observed) with headroom; handlers heap-allocate
    // their working buffers so stack-frame depth stays modest.
    config.stack_size       = 4096;
    // SSE handlers (e.g. /api/scope/stream) hold a worker socket open for
    // the lifetime of the browser tab. lru_purge_enable lets the httpd
    // evict the least-recently-used connection when a new one arrives, so
    // a forgotten tab can't permanently starve other clients.
    config.lru_purge_enable = true;
    // Socket budget. The scope SSE stream holds one socket open for its whole
    // lifetime; the desktop app simultaneously polls status/overview/hat/rails.
    // At the ESP-IDF default of 7 those concurrent polls overflow the budget and
    // lru_purge_enable evicts the (briefly idle) SSE connection — the firmware
    // logs `httpd_sock_err send : 104`, scope mode exits, and the client must
    // reconnect, which stalls the scope. CONFIG_LWIP_MAX_SOCKETS is raised to 16
    // in sdkconfig.defaults precisely to allow this; 10 gives the SSE headroom
    // alongside the REST poll connections while leaving margin for mDNS/etc.
    config.max_open_sockets = 10;

    // M06: start into a local, then publish under the mux so concurrent readers
    // always see either NULL or a fully-started handle, never a partial write.
    httpd_handle_t new_server = NULL;
    esp_err_t ret = httpd_start(&new_server, &config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start HTTP server: %s", esp_err_to_name(ret));
        return false;
    }
    taskENTER_CRITICAL(&s_server_mux);
    s_server = new_server;
    taskEXIT_CRITICAL(&s_server_mux);

    ESP_LOGI(TAG, "HTTP server started on port %d", config.server_port);
    httpd_register_err_handler(s_server, HTTPD_404_NOT_FOUND, handle_http_error);
    httpd_register_err_handler(s_server, HTTPD_405_METHOD_NOT_ALLOWED, handle_http_error);

    // ----- WebSocket routes (registered FIRST, before the ~76 REST routes) -----
    // Each WS register() creates a FreeRTOS mutex + counting semaphore from the
    // internal heap. Registering the bulk REST routes first consumes ~7 KB of
    // internal heap (~100 B per httpd_register_uri_handler() call), leaving
    // free=43/largest=20 by the time WS register runs — at which point the
    // sync-primitive creation fails and the URI is never registered, producing
    // 404 on /api/ws/stream and /api/scripts/repl/ws (observed 2026-05-07).
    // Front-loading WS registration uses the fresh ~7 KB internal pool that
    // exists immediately after httpd_start.
    repl_ws_register(s_server);
    ws_stream_register(s_server);

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

    httpd_uri_t uri_system_memory = {
        .uri = "/api/system/memory", .method = HTTP_GET, .handler = handle_get_system_memory, .user_ctx = NULL
    };
    httpd_register_uri_handler(s_server, &uri_system_memory);

    httpd_uri_t uri_system_crash = {
        .uri = "/api/system/crash", .method = HTTP_GET, .handler = handle_get_system_crash, .user_ctx = NULL
    };
    httpd_register_uri_handler(s_server, &uri_system_crash);

    httpd_uri_t uri_system_crash_clear = {
        .uri = "/api/system/crash/clear", .method = HTTP_POST, .handler = handle_post_system_crash_clear, .user_ctx = NULL
    };
    httpd_register_uri_handler(s_server, &uri_system_crash_clear);

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

    httpd_uri_t uri_scope_stream = {
        .uri = "/api/scope/stream", .method = HTTP_GET, .handler = handle_get_scope_stream, .user_ctx = NULL
    };
    httpd_register_uri_handler(s_server, &uri_scope_stream);

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

    httpd_uri_t uri_overview = {
        .uri = "/api/overview", .method = HTTP_GET, .handler = handle_get_overview, .user_ctx = NULL
    };
    httpd_register_uri_handler(s_server, &uri_overview);

    httpd_uri_t uri_selftest_supply = {
        .uri = "/api/selftest/supply/*", .method = HTTP_GET, .handler = handle_get_selftest_supply, .user_ctx = NULL
    };
    httpd_register_uri_handler(s_server, &uri_selftest_supply);

    httpd_uri_t uri_selftest_supplies_cached = {
        .uri = "/api/selftest/supplies/cached", .method = HTTP_GET, .handler = handle_get_selftest_supplies_cached, .user_ctx = NULL
    };
    httpd_register_uri_handler(s_server, &uri_selftest_supplies_cached);

    httpd_uri_t uri_selftest_worker = {
        .uri = "/api/selftest/worker", .method = HTTP_POST, .handler = handle_post_selftest_worker, .user_ctx = NULL
    };
    httpd_register_uri_handler(s_server, &uri_selftest_worker);

    httpd_uri_t uri_selftest_cal = {
        .uri = "/api/selftest/calibrate", .method = HTTP_POST, .handler = handle_post_selftest_calibrate, .user_ctx = NULL
    };
    httpd_register_uri_handler(s_server, &uri_selftest_cal);

    httpd_uri_t uri_selftest_supplies = {
        .uri = "/api/selftest/supplies", .method = HTTP_GET, .handler = handle_get_selftest_supplies, .user_ctx = NULL
    };
    httpd_register_uri_handler(s_server, &uri_selftest_supplies);

    httpd_uri_t uri_efuse_imon_get = {
        .uri = "/api/selftest/efuse_imon", .method = HTTP_GET, .handler = handle_get_efuse_imon, .user_ctx = NULL
    };
    httpd_register_uri_handler(s_server, &uri_efuse_imon_get);

    httpd_uri_t uri_efuse_imon_post = {
        .uri = "/api/selftest/efuse_imon", .method = HTTP_POST, .handler = handle_post_efuse_imon, .user_ctx = NULL
    };
    httpd_register_uri_handler(s_server, &uri_efuse_imon_post);

    // ----- Quick Setup routes -----

    httpd_uri_t uri_quicksetup_list = {
        .uri = "/api/quicksetup", .method = HTTP_GET, .handler = handle_get_quicksetup_list, .user_ctx = NULL
    };
    httpd_register_uri_handler(s_server, &uri_quicksetup_list);

    httpd_uri_t uri_quicksetup_get = {
        .uri = "/api/quicksetup/*", .method = HTTP_GET, .handler = handle_get_quicksetup_slot, .user_ctx = NULL
    };
    httpd_register_uri_handler(s_server, &uri_quicksetup_get);

    httpd_uri_t uri_quicksetup_post = {
        .uri = "/api/quicksetup/*", .method = HTTP_POST, .handler = handle_quicksetup_post_dispatch, .user_ctx = NULL
    };
    httpd_register_uri_handler(s_server, &uri_quicksetup_post);

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

    httpd_uri_t uri_idac_cal_points_get = {
        .uri = "/api/idac/cal/points", .method = HTTP_GET, .handler = handle_get_idac_cal_points, .user_ctx = NULL
    };
    httpd_register_uri_handler(s_server, &uri_idac_cal_points_get);

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

    // ----- DAQ HAT WiFi streaming routes -----

    httpd_uri_t uri_daq_wifi_stream_start = {
        .uri = "/api/daq/wifi_stream/start", .method = HTTP_POST, .handler = handle_post_daq_wifi_stream_start, .user_ctx = NULL
    };
    httpd_register_uri_handler(s_server, &uri_daq_wifi_stream_start);

    httpd_uri_t uri_daq_wifi_stream_stop = {
        .uri = "/api/daq/wifi_stream/stop", .method = HTTP_POST, .handler = handle_post_daq_wifi_stream_stop, .user_ctx = NULL
    };
    httpd_register_uri_handler(s_server, &uri_daq_wifi_stream_stop);

    httpd_uri_t uri_daq_wifi_stream_recycle = {
        .uri = "/api/daq/wifi_stream/recycle", .method = HTTP_POST,
        .handler = handle_post_daq_wifi_stream_recycle, .user_ctx = NULL
    };
    httpd_register_uri_handler(s_server, &uri_daq_wifi_stream_recycle);

    httpd_uri_t uri_daq_wifi_stream_status = {
        .uri = "/api/daq/wifi_stream/status", .method = HTTP_GET, .handler = handle_get_daq_wifi_stream_status, .user_ctx = NULL
    };
    httpd_register_uri_handler(s_server, &uri_daq_wifi_stream_status);

    httpd_uri_t uri_daq = {
        .uri = "/api/daq", .method = HTTP_GET, .handler = handle_get_daq, .user_ctx = NULL
    };
    httpd_register_uri_handler(s_server, &uri_daq);

    httpd_uri_t uri_daq_vdut_status = {
        .uri = "/api/daq/vdut/status", .method = HTTP_GET, .handler = handle_get_daq_vdut_status, .user_ctx = NULL
    };
    httpd_register_uri_handler(s_server, &uri_daq_vdut_status);

    httpd_uri_t uri_daq_vdut_enable = {
        .uri = "/api/daq/vdut/enable", .method = HTTP_POST, .handler = handle_post_daq_vdut_enable, .user_ctx = NULL
    };
    httpd_register_uri_handler(s_server, &uri_daq_vdut_enable);

    httpd_uri_t uri_daq_vdut_setpoint = {
        .uri = "/api/daq/vdut/setpoint", .method = HTTP_POST, .handler = handle_post_daq_vdut_setpoint, .user_ctx = NULL
    };
    httpd_register_uri_handler(s_server, &uri_daq_vdut_setpoint);

    httpd_uri_t uri_daq_acq_config = {
        .uri = "/api/daq/acq_config", .method = HTTP_POST, .handler = handle_post_daq_acq_config, .user_ctx = NULL
    };
    httpd_register_uri_handler(s_server, &uri_daq_acq_config);
    httpd_uri_t uri_daq_bs = {
        .uri = "/api/daq/bs", .method = HTTP_POST, .handler = handle_post_daq_bs, .user_ctx = NULL
    };
    httpd_register_uri_handler(s_server, &uri_daq_bs);
    httpd_uri_t uri_daq_bs_read = {
        .uri = "/api/daq/bs/read", .method = HTTP_POST, .handler = handle_post_daq_bs_read, .user_ctx = NULL
    };
    httpd_register_uri_handler(s_server, &uri_daq_bs_read);
    httpd_uri_t uri_hub_status = {
        .uri = "/api/hub/status", .method = HTTP_GET, .handler = handle_get_hub_status, .user_ctx = NULL
    };
    httpd_register_uri_handler(s_server, &uri_hub_status);
    // POST alias: iOS only has a POST helper (ConnectionManager.postJSON), which also works over BLE.
    httpd_uri_t uri_hub_status_post = {
        .uri = "/api/hub/status", .method = HTTP_POST, .handler = handle_get_hub_status, .user_ctx = NULL
    };
    httpd_register_uri_handler(s_server, &uri_hub_status_post);
    httpd_uri_t uri_hub_config_get = {
        .uri = "/api/hub/config", .method = HTTP_GET, .handler = handle_get_hub_config, .user_ctx = NULL
    };
    httpd_register_uri_handler(s_server, &uri_hub_config_get);
    httpd_uri_t uri_hub_config_post = {
        .uri = "/api/hub/config", .method = HTTP_POST, .handler = handle_post_hub_config, .user_ctx = NULL
    };
    httpd_register_uri_handler(s_server, &uri_hub_config_post);
    httpd_uri_t uri_hub_resync = {
        .uri = "/api/hub/resync", .method = HTTP_POST, .handler = handle_post_hub_resync, .user_ctx = NULL
    };
    httpd_register_uri_handler(s_server, &uri_hub_resync);
    httpd_uri_t uri_standby_status = {
        .uri = "/api/standby/status", .method = HTTP_GET, .handler = handle_get_standby_status, .user_ctx = NULL
    };
    httpd_register_uri_handler(s_server, &uri_standby_status);
    httpd_uri_t uri_standby_presence = {
        .uri = "/api/standby/presence", .method = HTTP_POST, .handler = handle_post_standby_presence, .user_ctx = NULL
    };
    httpd_register_uri_handler(s_server, &uri_standby_presence);
    httpd_uri_t uri_standby_wake = {
        .uri = "/api/standby/wake", .method = HTTP_POST, .handler = handle_post_standby_wake, .user_ctx = NULL
    };
    httpd_register_uri_handler(s_server, &uri_standby_wake);
    httpd_uri_t uri_standby_policy = {
        .uri = "/api/standby/policy", .method = HTTP_POST, .handler = handle_post_standby_policy, .user_ctx = NULL
    };
    httpd_register_uri_handler(s_server, &uri_standby_policy);
    httpd_uri_t uri_standby_sleep = {
        .uri = "/api/standby/sleep", .method = HTTP_POST, .handler = handle_post_standby_sleep, .user_ctx = NULL
    };
    httpd_register_uri_handler(s_server, &uri_standby_sleep);
    httpd_uri_t uri_daq_config = {
        .uri = "/api/daq/config", .method = HTTP_POST, .handler = handle_post_daq_config, .user_ctx = NULL
    };
    httpd_register_uri_handler(s_server, &uri_daq_config);

    // ----- HAT v2 routes (registered before the /api/hat/* wildcard) -----

    httpd_uri_t uri_hat_v2_caps = {
        .uri = "/api/hat/v2/caps", .method = HTTP_GET, .handler = handle_get_hat_v2_caps, .user_ctx = NULL
    };
    httpd_register_uri_handler(s_server, &uri_hat_v2_caps);

    httpd_uri_t uri_hat_v2_rails = {
        .uri = "/api/hat/v2/rails", .method = HTTP_GET, .handler = handle_get_hat_v2_rails, .user_ctx = NULL
    };
    httpd_register_uri_handler(s_server, &uri_hat_v2_rails);

    httpd_uri_t uri_hat_calibration = {
        .uri = "/api/hat/calibration", .method = HTTP_GET, .handler = handle_get_hat_calibration, .user_ctx = NULL
    };
    httpd_register_uri_handler(s_server, &uri_hat_calibration);

    httpd_uri_t uri_hat_v2_rail_enable = {
        .uri = "/api/hat/v2/rail/enable", .method = HTTP_POST, .handler = handle_post_hat_v2_rail_enable, .user_ctx = NULL
    };
    httpd_register_uri_handler(s_server, &uri_hat_v2_rail_enable);

    httpd_uri_t uri_hat_v2_rail_voltage = {
        .uri = "/api/hat/v2/rail/voltage", .method = HTTP_POST, .handler = handle_post_hat_v2_rail_voltage, .user_ctx = NULL
    };
    httpd_register_uri_handler(s_server, &uri_hat_v2_rail_voltage);

    httpd_uri_t uri_hat_v2_led = {
        .uri = "/api/hat/v2/led", .method = HTTP_POST, .handler = handle_post_hat_v2_led, .user_ctx = NULL
    };
    httpd_register_uri_handler(s_server, &uri_hat_v2_led);

    httpd_uri_t uri_hat_v2_la_route = {
        .uri = "/api/hat/v2/la/route", .method = HTTP_POST, .handler = handle_post_hat_v2_la_route, .user_ctx = NULL
    };
    httpd_register_uri_handler(s_server, &uri_hat_v2_la_route);

    httpd_uri_t uri_hat_v2_calibrate_start = {
        .uri = "/api/hat/v2/calibrate/start", .method = HTTP_POST, .handler = handle_post_hat_v2_calibrate_start, .user_ctx = NULL
    };
    httpd_register_uri_handler(s_server, &uri_hat_v2_calibrate_start);

    httpd_uri_t uri_hat_v2_calibrate_status = {
        .uri = "/api/hat/v2/calibrate/status", .method = HTTP_GET, .handler = handle_get_hat_v2_calibrate_status, .user_ctx = NULL
    };
    httpd_register_uri_handler(s_server, &uri_hat_v2_calibrate_status);

    httpd_uri_t uri_hat_v2_calibrate_import = {
        .uri = "/api/hat/v2/calibrate/import", .method = HTTP_POST, .handler = handle_post_hat_v2_calibrate_import, .user_ctx = NULL
    };
    httpd_register_uri_handler(s_server, &uri_hat_v2_calibrate_import);

    httpd_uri_t uri_hat_v2_io_bank = {
        .uri = "/api/hat/v2/io_bank", .method = HTTP_POST, .handler = handle_post_hat_v2_io_bank, .user_ctx = NULL
    };
    httpd_register_uri_handler(s_server, &uri_hat_v2_io_bank);

    httpd_uri_t uri_hat_v2_level_shift = {
        .uri = "/api/hat/v2/level_shift", .method = HTTP_POST, .handler = handle_post_hat_v2_level_shift, .user_ctx = NULL
    };
    httpd_register_uri_handler(s_server, &uri_hat_v2_level_shift);

    httpd_uri_t uri_hat_v2_io_voltage = {
        .uri = "/api/hat/v2/io_voltage", .method = HTTP_POST, .handler = handle_post_hat_v2_io_voltage, .user_ctx = NULL
    };
    httpd_register_uri_handler(s_server, &uri_hat_v2_io_voltage);

    httpd_uri_t uri_hat_power_get = {
        .uri = "/api/hat/power", .method = HTTP_GET, .handler = handle_get_hat_power, .user_ctx = NULL
    };
    httpd_register_uri_handler(s_server, &uri_hat_power_get);

    httpd_uri_t uri_hat_power_post = {
        .uri = "/api/hat/power", .method = HTTP_POST, .handler = handle_post_hat_power, .user_ctx = NULL
    };
    httpd_register_uri_handler(s_server, &uri_hat_power_post);

    httpd_uri_t uri_hat_v2_swd_setup = {
        .uri = "/api/hat/v2/swd/setup", .method = HTTP_POST, .handler = handle_post_hat_v2_swd_setup, .user_ctx = NULL
    };
    httpd_register_uri_handler(s_server, &uri_hat_v2_swd_setup);

    httpd_uri_t uri_hat_v2_la_log_enable = {
        .uri = "/api/hat/v2/la/log/enable", .method = HTTP_POST, .handler = handle_post_hat_v2_la_log_enable, .user_ctx = NULL
    };
    httpd_register_uri_handler(s_server, &uri_hat_v2_la_log_enable);

    httpd_uri_t uri_hat_v2_la_log = {
        .uri = "/api/hat/v2/la/log", .method = HTTP_GET, .handler = handle_get_hat_v2_la_log, .user_ctx = NULL
    };
    httpd_register_uri_handler(s_server, &uri_hat_v2_la_log);

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

    // ----- External routed bus routes -----

    httpd_uri_t uri_bus_status = {
        .uri = "/api/bus/status", .method = HTTP_GET, .handler = handle_get_bus_status, .user_ctx = NULL
    };
    httpd_register_uri_handler(s_server, &uri_bus_status);

    httpd_uri_t uri_bus_post = {
        .uri = "/api/bus/*", .method = HTTP_POST, .handler = handle_bus_post_dispatch, .user_ctx = NULL
    };
    httpd_register_uri_handler(s_server, &uri_bus_post);

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

    httpd_uri_t uri_wifi_ap_password = {
        .uri = "/api/wifi/ap_password", .method = HTTP_POST, .handler = handle_post_wifi_ap_password, .user_ctx = NULL
    };
    httpd_register_uri_handler(s_server, &uri_wifi_ap_password);

    httpd_uri_t uri_wifi_hostname_get = {
        .uri = "/api/wifi/hostname", .method = HTTP_GET, .handler = handle_get_wifi_hostname, .user_ctx = NULL
    };
    httpd_register_uri_handler(s_server, &uri_wifi_hostname_get);

    httpd_uri_t uri_wifi_hostname_post = {
        .uri = "/api/wifi/hostname", .method = HTTP_POST, .handler = handle_post_wifi_hostname, .user_ctx = NULL
    };
    httpd_register_uri_handler(s_server, &uri_wifi_hostname_post);

    // ----- OTA / Version routes -----

    httpd_uri_t uri_version = {
        .uri = "/api/device/version", .method = HTTP_GET, .handler = handle_get_version, .user_ctx = NULL
    };
    httpd_register_uri_handler(s_server, &uri_version);

    httpd_uri_t uri_ota = {
        .uri = "/api/ota/upload", .method = HTTP_POST, .handler = handle_ota_upload, .user_ctx = NULL
    };
    httpd_register_uri_handler(s_server, &uri_ota);

    httpd_uri_t uri_ota_rp2040 = {
        .uri = "/api/ota/upload_rp2040", .method = HTTP_POST, .handler = handle_rp2040_upload, .user_ctx = NULL
    };
    httpd_register_uri_handler(s_server, &uri_ota_rp2040);

    httpd_uri_t uri_ota_upload_p4 = {
        .uri = "/api/ota/upload_p4", .method = HTTP_POST, .handler = handle_p4_upload, .user_ctx = NULL
    };
    httpd_register_uri_handler(s_server, &uri_ota_upload_p4);

    httpd_uri_t uri_ota_upload_c6 = {
        .uri = "/api/ota/upload_c6", .method = HTTP_POST, .handler = handle_c6_upload, .user_ctx = NULL
    };
    httpd_register_uri_handler(s_server, &uri_ota_upload_c6);


    httpd_uri_t uri_uploadfs = {
        .uri = "/api/ota/uploadfs", .method = HTTP_POST, .handler = handle_uploadfs, .user_ctx = NULL
    };
    httpd_register_uri_handler(s_server, &uri_uploadfs);

    httpd_uri_t uri_ota_info = {
        .uri = "/api/ota/info", .method = HTTP_GET, .handler = handle_get_ota_info, .user_ctx = NULL
    };
    httpd_register_uri_handler(s_server, &uri_ota_info);

    httpd_uri_t uri_ota_rollback = {
        .uri = "/api/ota/rollback", .method = HTTP_POST, .handler = handle_post_ota_rollback, .user_ctx = NULL
    };
    httpd_register_uri_handler(s_server, &uri_ota_rollback);

    httpd_uri_t uri_update_check = {
        .uri = "/api/update/check", .method = HTTP_GET, .handler = handle_get_update_check, .user_ctx = NULL
    };
    httpd_register_uri_handler(s_server, &uri_update_check);

    httpd_uri_t uri_update_apply = {
        .uri = "/api/update/apply", .method = HTTP_POST, .handler = handle_post_update_apply, .user_ctx = NULL
    };
    httpd_register_uri_handler(s_server, &uri_update_apply);

    httpd_uri_t uri_update_status = {
        .uri = "/api/update/status", .method = HTTP_GET, .handler = handle_get_update_status, .user_ctx = NULL
    };
    httpd_register_uri_handler(s_server, &uri_update_status);

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

    httpd_uri_t uri_pairing_rotate = {
        .uri = "/api/pairing/rotate", .method = HTTP_POST, .handler = handle_post_pairing_rotate, .user_ctx = NULL
    };
    httpd_register_uri_handler(s_server, &uri_pairing_rotate);

    // ----- Scripting routes -----

    httpd_uri_t uri_scripts_eval = {
        .uri = "/api/scripts/eval", .method = HTTP_POST, .handler = handle_post_scripts_eval, .user_ctx = NULL
    };
    httpd_register_uri_handler(s_server, &uri_scripts_eval);

    httpd_uri_t uri_scripts_lint = {
        .uri = "/api/scripts/lint", .method = HTTP_POST, .handler = handle_post_scripts_lint, .user_ctx = NULL
    };
    httpd_register_uri_handler(s_server, &uri_scripts_lint);

    httpd_uri_t uri_scripts_logs = {
        .uri = "/api/scripts/logs", .method = HTTP_GET, .handler = handle_get_scripts_logs, .user_ctx = NULL
    };
    httpd_register_uri_handler(s_server, &uri_scripts_logs);

    httpd_uri_t uri_scripts_status = {
        .uri = "/api/scripts/status", .method = HTTP_GET, .handler = handle_get_scripts_status, .user_ctx = NULL
    };
    httpd_register_uri_handler(s_server, &uri_scripts_status);

    httpd_uri_t uri_scripts_stop = {
        .uri = "/api/scripts/stop", .method = HTTP_POST, .handler = handle_post_scripts_stop, .user_ctx = NULL
    };
    httpd_register_uri_handler(s_server, &uri_scripts_stop);

    httpd_uri_t uri_scripts_files_post = {
        .uri = "/api/scripts/files", .method = HTTP_POST, .handler = handle_post_scripts_files, .user_ctx = NULL
    };
    httpd_register_uri_handler(s_server, &uri_scripts_files_post);

    httpd_uri_t uri_scripts_files_get = {
        .uri = "/api/scripts/files", .method = HTTP_GET, .handler = handle_get_scripts_files, .user_ctx = NULL
    };
    httpd_register_uri_handler(s_server, &uri_scripts_files_get);

    httpd_uri_t uri_scripts_storage = {
        .uri = "/api/scripts/storage", .method = HTTP_GET, .handler = handle_get_scripts_storage, .user_ctx = NULL
    };
    httpd_register_uri_handler(s_server, &uri_scripts_storage);

    httpd_uri_t uri_scripts_file_get = {
        .uri = "/api/scripts/files/get", .method = HTTP_GET, .handler = handle_get_scripts_file, .user_ctx = NULL
    };
    httpd_register_uri_handler(s_server, &uri_scripts_file_get);

    httpd_uri_t uri_scripts_file_delete = {
        .uri = "/api/scripts/files", .method = HTTP_DELETE, .handler = handle_delete_scripts_file, .user_ctx = NULL
    };
    httpd_register_uri_handler(s_server, &uri_scripts_file_delete);

    httpd_uri_t uri_scripts_run_file = {
        .uri = "/api/scripts/run-file", .method = HTTP_POST, .handler = handle_post_scripts_run_file, .user_ctx = NULL
    };
    httpd_register_uri_handler(s_server, &uri_scripts_run_file);

    httpd_uri_t uri_scripts_reset = {
        .uri = "/api/scripts/reset", .method = HTTP_POST, .handler = handle_post_scripts_reset, .user_ctx = NULL
    };
    httpd_register_uri_handler(s_server, &uri_scripts_reset);

    httpd_uri_t uri_scripts_files_chunk = {
        .uri = "/api/scripts/files/chunk", .method = HTTP_POST, .handler = handle_post_scripts_files_chunk, .user_ctx = NULL
    };
    httpd_register_uri_handler(s_server, &uri_scripts_files_chunk);

    httpd_uri_t uri_scripts_files_delete_post = {
        .uri = "/api/scripts/files/delete", .method = HTTP_POST, .handler = handle_post_scripts_files_delete, .user_ctx = NULL
    };
    httpd_register_uri_handler(s_server, &uri_scripts_files_delete_post);

    // ----- IO Ownership routes -----

    httpd_uri_t uri_io_owner_get = {
        .uri = "/api/io/owner", .method = HTTP_GET, .handler = handle_get_io_owner, .user_ctx = NULL
    };
    httpd_register_uri_handler(s_server, &uri_io_owner_get);

    httpd_uri_t uri_io_owner_post = {
        .uri = "/api/io/owner", .method = HTTP_POST, .handler = handle_post_io_owner, .user_ctx = NULL
    };
    httpd_register_uri_handler(s_server, &uri_io_owner_post);

    httpd_uri_t uri_io_owner_delete = {
        .uri = "/api/io/owner", .method = HTTP_DELETE, .handler = handle_delete_io_owner, .user_ctx = NULL
    };
    httpd_register_uri_handler(s_server, &uri_io_owner_delete);

    httpd_uri_t uri_io_owner_force = {
        .uri = "/api/io/owner/force", .method = HTTP_POST, .handler = handle_post_io_owner_force, .user_ctx = NULL
    };
    httpd_register_uri_handler(s_server, &uri_io_owner_force);

    // ----- ADGS routes endpoint -----

    httpd_uri_t uri_adgs_routes = {
        .uri = "/api/adgs/routes", .method = HTTP_GET, .handler = handle_get_adgs_routes, .user_ctx = NULL
    };
    httpd_register_uri_handler(s_server, &uri_adgs_routes);

    // ----- Autorun routes -----

    httpd_uri_t uri_autorun_status = {
        .uri = "/api/scripts/autorun/status", .method = HTTP_GET, .handler = handle_get_autorun_status, .user_ctx = NULL
    };
    httpd_register_uri_handler(s_server, &uri_autorun_status);

    httpd_uri_t uri_autorun_enable = {
        .uri = "/api/scripts/autorun/enable", .method = HTTP_POST, .handler = handle_post_autorun_enable, .user_ctx = NULL
    };
    httpd_register_uri_handler(s_server, &uri_autorun_enable);

    httpd_uri_t uri_autorun_disable = {
        .uri = "/api/scripts/autorun/disable", .method = HTTP_POST, .handler = handle_post_autorun_disable, .user_ctx = NULL
    };
    httpd_register_uri_handler(s_server, &uri_autorun_disable);

    // ----- ADC DSP streaming routes -----

    httpd_uri_t uri_adc_dsp_start = {
        .uri = "/api/adc/dsp/start", .method = HTTP_POST, .handler = handle_post_adc_dsp_start, .user_ctx = NULL
    };
    httpd_register_uri_handler(s_server, &uri_adc_dsp_start);

    httpd_uri_t uri_adc_dsp_stop = {
        .uri = "/api/adc/dsp/stop", .method = HTTP_POST, .handler = handle_post_adc_dsp_stop, .user_ctx = NULL
    };
    httpd_register_uri_handler(s_server, &uri_adc_dsp_stop);

    // ----- OPTIONS (CORS preflight) -----
    // No wildcard `/api/*` OPTIONS handler is registered. A wildcard handler
    // would shadow the WS upgrade GET on `/api/ws/stream` for clients whose
    // request doesn't match the websocket-handshake fast path (curl, python
    // `websockets`, ...), producing a spurious 405. Instead, OPTIONS preflight
    // for /api/* is synthesized from the HTTPD_405_METHOD_NOT_ALLOWED error
    // handler in `handle_http_error` (returns 204 + CORS headers).
    // WS routes are registered FIRST (right after httpd_start) — see the
    // comment up there for the internal-heap exhaustion reason.

    // Registry-backed adapter routes (Slice 2: DAC commands)
    http_adapter_register(s_server);

    // Register last so concrete API/static routes win, while browser-only
    // paths such as /index.html or a copied SPA URL still load the app shell.
    httpd_uri_t uri_browser_fallback = {
        .uri = "/*", .method = HTTP_GET, .handler = handle_browser_fallback, .user_ctx = NULL
    };
    httpd_register_uri_handler(s_server, &uri_browser_fallback);

    ESP_LOGI(TAG, "All URI handlers registered");
    return true;
}

void stopWebServer(void)
{
    // M06: snapshot handle under mux, stop outside, then clear under mux.
    taskENTER_CRITICAL(&s_server_mux);
    httpd_handle_t h = s_server;
    taskEXIT_CRITICAL(&s_server_mux);

    if (h) {
        httpd_stop(h);
        taskENTER_CRITICAL(&s_server_mux);
        s_server = NULL;
        taskEXIT_CRITICAL(&s_server_mux);
        ESP_LOGI(TAG, "HTTP server stopped");
    }
}
