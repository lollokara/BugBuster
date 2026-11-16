// =============================================================================
// webserver_core.cpp - HTTP handlers: status, channel, ADC/DAC, GPIO and scope handlers, and the /api/channel dispatchers.
// Routes are registered in webserver.cpp (initWebServer).
// =============================================================================

#include "webserver_internal.h"

static const char* TAG = "webserver";

// =============================================================================
// Route handler implementations
// =============================================================================

// GET /
static esp_err_t send_index_html(httpd_req_t *req)
{
    FILE *f = fopen("/spiffs/index.html", "r");
    if (!f) {
        ESP_LOGW(TAG, "index.html missing from /spiffs; upload the web UI filesystem image");
        char origin_buf[96];
        set_cors_headers(req, origin_buf, sizeof(origin_buf));
        httpd_resp_send_404(req);
        return ESP_FAIL;
    }
    char origin_buf[96];
    set_cors_headers(req, origin_buf, sizeof(origin_buf));
    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store, no-cache, must-revalidate");
    httpd_resp_set_hdr(req, "X-BugBuster-Web-Build", BUGBUSTER_WEB_BUILD_MARKER);
    char buf[512];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
        httpd_resp_send_chunk(req, buf, n);
    }
    httpd_resp_send_chunk(req, NULL, 0);  // end chunked response
    fclose(f);
    return ESP_OK;
}

esp_err_t handle_root(httpd_req_t *req)
{
    return send_index_html(req);
}

// GET /* — browser shell fallback.
//
// Keep API and asset misses strict, but serve the Vite app shell for
// /index.html and extensionless browser routes so bookmarks/manual URLs do not
// fall through to ESP-IDF's opaque 404 handler.
esp_err_t handle_browser_fallback(httpd_req_t *req)
{
    const char *uri = req->uri;
    const char *q = strchr(uri, '?');
    size_t uri_len = q ? (size_t)(q - uri) : strlen(uri);

    if (strncmp(uri, "/api/", 5) == 0 || strncmp(uri, "/assets/", 8) == 0) {
        ESP_LOGW(TAG, "404 for %.*s", (int)uri_len, uri);
        char origin_buf[96];
        set_cors_headers(req, origin_buf, sizeof(origin_buf));
        return httpd_resp_send_404(req);
    }

    if (uri_len == strlen("/favicon.ico") &&
        strncmp(uri, "/favicon.ico", uri_len) == 0) {
        httpd_resp_set_status(req, "204 No Content");
        return httpd_resp_send(req, NULL, 0);
    }

    bool is_index = (uri_len == strlen("/index.html") &&
                     strncmp(uri, "/index.html", uri_len) == 0);
    bool has_extension = false;
    for (size_t i = 0; i < uri_len; i++) {
        if (uri[i] == '.') {
            has_extension = true;
            break;
        }
    }

    if (is_index || !has_extension) {
        return send_index_html(req);
    }

    char origin_buf[96];
    set_cors_headers(req, origin_buf, sizeof(origin_buf));
    ESP_LOGW(TAG, "404 for %.*s", (int)uri_len, uri);
    return httpd_resp_send_404(req);
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
esp_err_t handle_static_asset(httpd_req_t *req)
{
    const char *uri = req->uri;

    // Strip query string, if any.
    const char *q = strchr(uri, '?');
    size_t uri_len = q ? (size_t)(q - uri) : strlen(uri);

    // Must start with "/assets/".
    const char *prefix = "/assets/";
    size_t prefix_len = strlen(prefix);
    if (uri_len <= prefix_len || strncmp(uri, prefix, prefix_len) != 0) {
        char origin_buf[96];
        set_cors_headers(req, origin_buf, sizeof(origin_buf));
        return httpd_resp_send_404(req);
    }

    // Reject path traversal.
    if (strstr(uri, "..") != NULL) {
        char origin_buf[96];
        set_cors_headers(req, origin_buf, sizeof(origin_buf));
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Bad path");
    }

    // Build /spiffs<uri> path (bounded).
    char fs_path[160];
    if (uri_len + sizeof("/spiffs") > sizeof(fs_path)) {
        char origin_buf[96];
        set_cors_headers(req, origin_buf, sizeof(origin_buf));
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Path too long");
    }
    int written = snprintf(fs_path, sizeof(fs_path), "/spiffs%.*s",
                           (int)uri_len, uri);
    if (written <= 0 || (size_t)written >= sizeof(fs_path)) {
        char origin_buf[96];
        set_cors_headers(req, origin_buf, sizeof(origin_buf));
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Path too long");
    }

    // If the build emitted a precompressed sibling (vite-config.ts gzipEmitter)
    // and the browser advertises gzip support, prefer that. Saves ~70% on
    // text/* assets and is purely a transport optimization — clients that
    // omit Accept-Encoding still get the uncompressed file.
    bool serve_gzipped = false;
    char gz_path[160];
    char accept_enc[64] = {0};
    if (httpd_req_get_hdr_value_str(req, "Accept-Encoding", accept_enc,
                                    sizeof(accept_enc)) == ESP_OK &&
        strstr(accept_enc, "gzip") != NULL) {
        if (snprintf(gz_path, sizeof(gz_path), "%s.gz", fs_path) >= 0 &&
            (size_t)snprintf(NULL, 0, "%s.gz", fs_path) < sizeof(gz_path)) {
            FILE *gf = fopen(gz_path, "rb");
            if (gf) {
                fclose(gf);
                serve_gzipped = true;
            }
        }
    }

    const char *open_path = serve_gzipped ? gz_path : fs_path;
    FILE *f = fopen(open_path, "rb");
    if (!f) {
        ESP_LOGW(TAG, "Asset not found: %s", open_path);
        char origin_buf[96];
        set_cors_headers(req, origin_buf, sizeof(origin_buf));
        return httpd_resp_send_404(req);
    }

    char origin_buf[96];
    set_cors_headers(req, origin_buf, sizeof(origin_buf));
    httpd_resp_set_type(req, mime_from_path(fs_path));
    if (serve_gzipped) {
        httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
        // Vary so an HTTP cache does not feed a gzip body to a client that
        // only accepts identity (rare, but free correctness).
        httpd_resp_set_hdr(req, "Vary", "Accept-Encoding");
    }
    // Short max-age without immutable — SPIFFS assets can change on the same
    // URL when firmware is redeployed without a content-hash change (e.g. after
    // a define-only rebuild).  5 minutes is enough to avoid per-click fetches
    // while still letting browsers pick up fresh content after a SPIFFS update.
    httpd_resp_set_hdr(req, "Cache-Control", "public, max-age=300");

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
esp_err_t handle_get_status(httpd_req_t *req)
{
    char *resp = api_core_handle("GET", "/api/status", NULL);
    if (!resp) return send_error(req, 503, "State mutex timeout");
    esp_err_t rc = send_raw_json(req, resp);
    cJSON_free(resp);
    return rc;
}

// GET /api/system/memory
esp_err_t handle_get_system_memory(httpd_req_t *req)
{
    char *resp = api_core_handle("GET", "/api/system/memory", NULL);
    if (!resp) return send_error(req, 500, "memory status failed");
    esp_err_t rc = send_raw_json(req, resp);
    cJSON_free(resp);
    return rc;
}

// GET /api/system/crash[?report=1 | ?offset=N&len=M | ?raw=1]  (admin)
// JSON via api_core_handle (shared with BLE). ?raw=1 streams the stored ELF
// coredump as a download: 1 KB heap slice at a time, nothing else buffered.
esp_err_t handle_get_system_crash(httpd_req_t *req)
{
    if (check_admin_auth(req) != ESP_OK) return send_error(req, 401, "Admin token required");

    char query[64] = {0};
    char flag[8] = {0};
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK &&
        httpd_query_key_value(query, "raw", flag, sizeof(flag)) == ESP_OK && flag[0] == '1') {
        size_t total = crash_report_dump_size();
        if (total == 0) return send_error(req, 404, "No coredump stored");
        const size_t SLICE = 1024;
        uint8_t *buf = (uint8_t *)malloc(SLICE);
        if (!buf) return send_error(req, 500, "Out of memory");
        char origin_buf[96];
        set_cors_headers(req, origin_buf, sizeof(origin_buf));
        httpd_resp_set_type(req, "application/octet-stream");
        httpd_resp_set_hdr(req, "Content-Disposition", "attachment; filename=\"bugbuster-coredump.elf\"");
        httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
        esp_err_t rc = ESP_OK;
        for (size_t off = 0; off < total && rc == ESP_OK; off += SLICE) {
            size_t n = total - off < SLICE ? total - off : SLICE;
            if (crash_report_dump_read(off, buf, n) != ESP_OK) { rc = ESP_FAIL; break; }
            rc = httpd_resp_send_chunk(req, (const char *)buf, n);
        }
        free(buf);
        if (rc != ESP_OK) {
            httpd_resp_sendstr_chunk(req, NULL);   // abort: truncated body, client sees bad length
            return ESP_FAIL;
        }
        return httpd_resp_send_chunk(req, NULL, 0);
    }

    char *resp = api_core_handle("GET", req->uri, NULL);
    if (!resp) return send_error(req, 500, "crash info failed");
    esp_err_t rc = send_raw_json(req, resp);
    cJSON_free(resp);
    return rc;
}

// POST /api/system/crash/clear  (admin) — erase the stored coredump
esp_err_t handle_post_system_crash_clear(httpd_req_t *req)
{
    if (check_admin_auth(req) != ESP_OK) return send_error(req, 401, "Admin token required");
    char *resp = api_core_handle("POST", "/api/system/crash/clear", NULL);
    if (!resp) return send_error(req, 500, "crash clear failed");
    esp_err_t rc = send_raw_json(req, resp);
    cJSON_free(resp);
    return rc;
}

// GET /api/channel/?/adc
static esp_err_t handle_get_channel_adc(httpd_req_t *req)
{
    char *resp = api_core_handle("GET", req->uri, NULL);
    if (!resp) return send_error(req, 500, "channel adc failed");
    esp_err_t rc = send_raw_json(req, resp);
    cJSON_free(resp);
    return rc;
}

// GET /api/faults
esp_err_t handle_get_faults(httpd_req_t *req)
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
esp_err_t handle_get_scope(httpd_req_t *req)
{
    uint16_t since_seq = 0;
    // 256 bytes, and truncation is an explicit 400 rather than being folded
    // into "no query". A 32-byte buffer silently dropped ?since= as soon as a
    // client appended a cache-buster (?since=65535&_=1738500000000 is 33
    // bytes): the delta request became a full replay of the oldest buckets on
    // every poll, so the trace never advanced and it looked like a firmware
    // sampling bug. Same shape at /api/scripts/eval and /api/scripts/logs.
    // See docs/superpowers/reviews/2026-08-03-design-sweep.md finding S3-7.
    char qbuf[256] = {};
    esp_err_t qrc = httpd_req_get_url_query_str(req, qbuf, sizeof(qbuf));
    if (qrc == ESP_ERR_HTTPD_RESULT_TRUNC) {
        return send_error(req, 400, "query string too long");
    }
    if (qrc == ESP_OK) {
        char val[16] = {};
        if (httpd_query_key_value(qbuf, "since", val, sizeof(val)) == ESP_OK) {
            since_seq = (uint16_t)atoi(val);
        }
    }

    cJSON *root = cJSON_CreateObject();
    cJSON *samples = cJSON_AddArrayToObject(root, "samples");
    cJSON_AddItemReferenceToObject(root, "s", samples);

    if (xSemaphoreTake(g_stateMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        const ScopeBuffer& sb = *g_deviceState.scope;
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

// GET /api/scope/stream  — Server-Sent Events stream of scope buckets.
//
// Replaces the old 30 Hz polling loop on the web client. Holds the http
// connection open and writes one `data: { ... }\n\n` event whenever new
// buckets arrive (i.e. whenever `sb.seq` advances past the last seq we
// emitted). Uses non-blocking-ish chunked sends; `httpd_resp_send_chunk`
// returns ESP_FAIL the moment the client TCP socket goes away (tab closed,
// reconnect, navigation), which is our cue to exit and free the worker.
//
// Caveats:
// - Holds one ESP-IDF httpd worker for the duration of the stream. With the
//   current pool size and a single browser this is fine; if multiple tabs
//   ever open the scope concurrently we may need to bump
//   CONFIG_HTTPD_MAX_OPEN_SOCKETS.
// - Iterates at ~10 Hz with vTaskDelay so it doesn't spin; this caps event
//   latency at ~100 ms. For a homemade scope that's well below human
//   perception and it lets the HTTP socket close quickly when the tab is
//   hidden.
// WEB-23: this loop runs on its own task with a detached (async) request -
// httpd serves every socket from one task, so running it inline stalled
// every other request for as long as a scope tab was open.
static esp_err_t scope_sse_loop(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/event-stream");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    httpd_resp_set_hdr(req, "Connection", "keep-alive");
    httpd_resp_set_hdr(req, "X-Accel-Buffering", "no");
    char origin_buf[96];
    set_cors_headers(req, origin_buf, sizeof(origin_buf));

    // Send a leading retry hint so the browser's auto-reconnect spaces out
    // a little if the device reboots. EventSource default is 3 s — bump to
    // 5 s to be gentler on the ESP32.
    httpd_resp_send_chunk(req, "retry: 5000\n\n", 13);
    tasks_scope_mode_enter(0x0F);

    uint16_t last_seq = 0;
    bool primed = false;
    // Heartbeat keeps proxies / NAT translations from collapsing the
    // connection while no buckets are produced.
    uint32_t last_heartbeat_ms = millis_now();
    const uint32_t HEARTBEAT_MS = 15000;

    for (;;) {
        bool sent_data = false;

        if (xSemaphoreTake(g_stateMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
            const ScopeBuffer& sb = *g_deviceState.scope;
            uint16_t cur_seq = sb.seq;
            uint16_t head    = sb.head;

            if (!primed) {
                // First message after connect: send the most recent bucket
                // so the client has something to draw immediately rather
                // than waiting for a fresh bucket to be produced.
                if (cur_seq != 0) {
                    last_seq = (uint16_t)(cur_seq - 1);
                }
                primed = true;
            }

            uint16_t avail = (uint16_t)(cur_seq - last_seq);
            if (avail > 0) {
                if (avail > SCOPE_BUF_SIZE) avail = SCOPE_BUF_SIZE;
                if (avail > 32) avail = 32;

                ScopeBucket *snapshot = (ScopeBucket *)malloc(sizeof(ScopeBucket) * 32);
                if (!snapshot) {
                    xSemaphoreGive(g_stateMutex);
                    tasks_scope_mode_exit();
                    return ESP_ERR_NO_MEM;
                }
                uint16_t start = (head + SCOPE_BUF_SIZE - avail) % SCOPE_BUF_SIZE;
                for (uint16_t n = 0; n < avail; n++) {
                    uint16_t idx = (start + n) % SCOPE_BUF_SIZE;
                    snapshot[n] = sb.buckets[idx];
                }
                xSemaphoreGive(g_stateMutex);

                cJSON *root = cJSON_CreateObject();
                cJSON_AddNumberToObject(root, "seq", cur_seq);
                cJSON *samples = cJSON_AddArrayToObject(root, "samples");

                for (uint16_t n = 0; n < avail; n++) {
                    const ScopeBucket& bk = snapshot[n];
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

                free(snapshot);

                char *body = cJSON_PrintUnformatted(root);
                cJSON_Delete(root);
                if (!body) {
                    tasks_scope_mode_exit();
                    return ESP_FAIL;
                }
                // SSE wire format: "data: <json>\n\n"
                size_t blen = strlen(body);
                if (httpd_resp_send_chunk(req, "data: ", 6) != ESP_OK ||
                    httpd_resp_send_chunk(req, body, blen) != ESP_OK ||
                    httpd_resp_send_chunk(req, "\n\n", 2) != ESP_OK) {
                    free(body);
                    tasks_scope_mode_exit();
                    return ESP_OK; // client disconnected — clean exit
                }
                free(body);

                last_seq = cur_seq;
                last_heartbeat_ms = millis_now();
                sent_data = true;
            } else {
                xSemaphoreGive(g_stateMutex);
            }
        }

        // Heartbeat (an SSE comment line) keeps the connection alive across
        // intermediate proxies even when no scope buckets are flowing.
        if (!sent_data) {
            uint32_t now = millis_now();
            if (now - last_heartbeat_ms >= HEARTBEAT_MS) {
                if (httpd_resp_send_chunk(req, ": keep-alive\n\n", 14) != ESP_OK) {
                    tasks_scope_mode_exit();
                    return ESP_OK;
                }
                last_heartbeat_ms = now;
            }
        }

        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

#define SCOPE_SSE_MAX_STREAMS 2
static volatile int s_scope_sse_streams = 0;

static void scope_sse_task(void *arg)
{
    httpd_req_t *req = (httpd_req_t *)arg;
    scope_sse_loop(req);
    httpd_req_async_handler_complete(req);
    __atomic_sub_fetch(&s_scope_sse_streams, 1, __ATOMIC_SEQ_CST);
    vTaskDelete(NULL);
}

// GET /api/scope/stream - detach and stream from a dedicated task (WEB-23).
esp_err_t handle_get_scope_stream(httpd_req_t *req)
{
    if (__atomic_add_fetch(&s_scope_sse_streams, 1, __ATOMIC_SEQ_CST) > SCOPE_SSE_MAX_STREAMS) {
        __atomic_sub_fetch(&s_scope_sse_streams, 1, __ATOMIC_SEQ_CST);
        return send_error(req, 503, "too many scope streams");
    }
    httpd_req_t *async_req = NULL;
    if (httpd_req_async_handler_begin(req, &async_req) != ESP_OK) {
        __atomic_sub_fetch(&s_scope_sse_streams, 1, __ATOMIC_SEQ_CST);
        return send_error(req, 500, "scope stream: async begin failed");
    }
    if (xTaskCreate(scope_sse_task, "scope_sse", 4096, async_req, 4, NULL) != pdPASS) {
        httpd_req_async_handler_complete(async_req);
        __atomic_sub_fetch(&s_scope_sse_streams, 1, __ATOMIC_SEQ_CST);
        return ESP_FAIL;
    }
    return ESP_OK;
}

// GET /api/diagnostics
esp_err_t handle_get_diagnostics(httpd_req_t *req)
{
    cJSON *root = cJSON_CreateObject();

    if (xSemaphoreTake(g_stateMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        const bool analog_ok = !g_deviceState.analogUnavailable;
        cJSON_AddBoolToObject(root, "analogAvailable", analog_ok);
        cJSON *slots = cJSON_AddArrayToObject(root, "slots");
        for (uint8_t i = 0; i < 4; i++) {
            const DiagState& ds = g_deviceState.diag[i];
            cJSON *obj = cJSON_CreateObject();
            cJSON_AddNumberToObject(obj, "slot", i);
            cJSON_AddNumberToObject(obj, "source", ds.source);
            cJSON_AddStringToObject(obj, "sourceName", diagSourceName(ds.source));
            api_add_measurement(obj, "raw", ds.rawCode, analog_ok);
            api_add_measurement(obj, "value", ds.value, analog_ok);
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
esp_err_t handle_get_device_info(httpd_req_t *req)
{
    char *resp = api_core_handle("GET", "/api/device/info", NULL);
    if (!resp) return send_error(req, 500, "device info failed");
    esp_err_t rc = send_raw_json(req, resp);
    cJSON_free(resp);
    return rc;
}

// GET /api/gpio
esp_err_t handle_get_gpio(httpd_req_t *req)
{
    cJSON *root = cJSON_CreateArray();

    if (xSemaphoreTake(g_stateMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        for (uint8_t g = 0; g < 12; g++) {
            const GpioState& gs = g_deviceState.dio[g];
            cJSON *obj = cJSON_CreateObject();
            cJSON_AddNumberToObject(obj, "id", g);
            cJSON_AddNumberToObject(obj, "pin", g);
            char name[8];
            snprintf(name, sizeof(name), "IO %d", g + 1);
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
    spiDriver.readRegister(AD74416H_REG_DAC_ACTIVE(tasks_logical_to_physical((uint8_t)ch)), &activeCode);

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
    cJSON *body = recv_json_body(req);
    if (!body) return send_error(req, 400, "Invalid JSON");
    char *resp = api_core_handle("POST", req->uri, body);
    if (body) cJSON_Delete(body);
    if (!resp) return send_error(req, 500, "channel function failed");
    esp_err_t rc = send_raw_json(req, resp);
    cJSON_free(resp);
    return rc;
}

// POST /api/channel/?/dac
static esp_err_t handle_post_dac(httpd_req_t *req)
{
    cJSON *body = recv_json_body(req);
    if (!body) return send_error(req, 400, "Invalid JSON");
    char *resp = api_core_handle("POST", req->uri, body);
    if (body) cJSON_Delete(body);
    if (!resp) return send_error(req, 500, "dac failed");
    esp_err_t rc = send_raw_json(req, resp);
    cJSON_free(resp);
    return rc;
}

// POST /api/channel/?/adc/config
static esp_err_t handle_post_adc_config(httpd_req_t *req)
{
    cJSON *body = recv_json_body(req);
    if (!body) return send_error(req, 400, "Invalid JSON");
    char *resp = api_core_handle("POST", req->uri, body);
    if (body) cJSON_Delete(body);
    if (!resp) return send_error(req, 500, "adc config failed");
    esp_err_t rc = send_raw_json(req, resp);
    cJSON_free(resp);
    return rc;
}

// POST /api/adc/dsp/start  — body: {channel, rate, windowSamples, spikeThreshold, nFftPeaks}
esp_err_t handle_post_adc_dsp_start(httpd_req_t *req)
{
    if (check_admin_auth(req) != ESP_OK) return send_error(req, 401, "Admin token required");

    cJSON *doc = recv_json_body(req);
    if (!doc) return send_error(req, 400, "Invalid JSON");

    cJSON *ch_item    = cJSON_GetObjectItem(doc, "channel");
    cJSON *rate_item  = cJSON_GetObjectItem(doc, "rate");
    if (!cJSON_IsNumber(ch_item) || !cJSON_IsNumber(rate_item)) {
        cJSON_Delete(doc);
        return send_error(req, 400, "Missing required fields: channel, rate");
    }

    uint8_t  channel         = (uint8_t)ch_item->valueint;
    uint8_t  rate_code       = (uint8_t)rate_item->valueint;
    uint16_t window_samples  = 256;
    float    spike_threshold = 0.1f;
    uint8_t  n_fft_peaks     = 8;

    cJSON *ws_item  = cJSON_GetObjectItem(doc, "windowSamples");
    cJSON *st_item  = cJSON_GetObjectItem(doc, "spikeThreshold");
    cJSON *nfp_item = cJSON_GetObjectItem(doc, "nFftPeaks");
    if (cJSON_IsNumber(ws_item))  window_samples  = (uint16_t)ws_item->valueint;
    if (cJSON_IsNumber(st_item))  spike_threshold = (float)st_item->valuedouble;
    if (cJSON_IsNumber(nfp_item)) n_fft_peaks     = (uint8_t)nfp_item->valueint;

    cJSON_Delete(doc);

    if (channel > 3) return send_error(req, 400, "channel must be 0-3");
    if (!bbpIsValidAdcRate(rate_code)) return send_error(req, 400, "invalid rate code");

    if (bbpAdcDspActive()) bbpStopAdcDspStream();

    uint16_t effective_rate = 0;
    if (!bbpStartAdcDspStream(channel, rate_code, window_samples, spike_threshold,
                              n_fft_peaks, &effective_rate)) {
        return send_error(req, 409,
                          "cannot stream DSP on this channel - configure it as "
                          "an analog input first (HIGH_IMP channels are never sampled)");
    }

    cJSON *resp = cJSON_CreateObject();
    cJSON_AddBoolToObject(resp, "ok", true);
    cJSON_AddNumberToObject(resp, "channel", channel);
    cJSON_AddNumberToObject(resp, "effectiveRateHz", effective_rate);
    return send_json(req, resp);
}

// POST /api/adc/dsp/stop
esp_err_t handle_post_adc_dsp_stop(httpd_req_t *req)
{
    if (check_admin_auth(req) != ESP_OK) return send_error(req, 401, "Admin token required");
    bbpStopAdcDspStream();
    cJSON *resp = cJSON_CreateObject();
    cJSON_AddBoolToObject(resp, "ok", true);
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
    cJSON *body = recv_json_body(req);
    if (!body) return send_error(req, 400, "Invalid JSON");
    char *resp = api_core_handle("POST", req->uri, body);
    if (body) cJSON_Delete(body);
    if (!resp) return send_error(req, 500, "vout range failed");
    esp_err_t rc = send_raw_json(req, resp);
    cJSON_free(resp);
    return rc;
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
esp_err_t handle_post_device_reset(httpd_req_t *req)
{
    if (check_admin_auth(req) != ESP_OK) return send_error(req, 401, "Admin token required");
    cJSON *body = recv_json_body(req);
    char *resp = api_core_handle("POST", "/api/device/reset", body);
    if (body) cJSON_Delete(body);
    if (!resp) return send_error(req, 500, "reset failed");
    esp_err_t rc = send_raw_json(req, resp);
    cJSON_free(resp);
    return rc;
}

// POST /api/faults/clear
esp_err_t handle_post_clear_all_faults(httpd_req_t *req)
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
esp_err_t handle_post_clear_channel_fault(httpd_req_t *req)
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
esp_err_t handle_post_faults_mask(httpd_req_t *req)
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
esp_err_t handle_post_channel_fault_mask(httpd_req_t *req)
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
esp_err_t handle_post_diag_config(httpd_req_t *req)
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
    cJSON *body = recv_json_body(req);
    if (!body) return send_error(req, 400, "Invalid JSON");
    char *resp = api_core_handle("POST", req->uri, body);
    if (body) cJSON_Delete(body);
    if (!resp) return send_error(req, 500, "gpio config failed");
    esp_err_t rc = send_raw_json(req, resp);
    cJSON_free(resp);
    return rc;
}

// POST /api/gpio/?/set
static esp_err_t handle_post_gpio_set(httpd_req_t *req)
{
    cJSON *body = recv_json_body(req);
    if (!body) return send_error(req, 400, "Invalid JSON");
    char *resp = api_core_handle("POST", req->uri, body);
    if (body) cJSON_Delete(body);
    if (!resp) return send_error(req, 500, "gpio set failed");
    esp_err_t rc = send_raw_json(req, resp);
    cJSON_free(resp);
    return rc;
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
esp_err_t handle_channel_get_dispatch(httpd_req_t *req)
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
    cJSON *body = recv_json_body(req);
    if (!body) return send_error(req, 400, "Invalid JSON");
    char *resp = api_core_handle("POST", req->uri, body);
    if (body) cJSON_Delete(body);
    if (!resp) return send_error(req, 500, "rtd config failed");
    esp_err_t rc = send_raw_json(req, resp);
    cJSON_free(resp);
    return rc;
}

// POST /api/channel/* dispatcher
esp_err_t handle_channel_post_dispatch(httpd_req_t *req)
{
    if (check_admin_auth(req) != ESP_OK) return send_error(req, 401, "Admin token required");

    const char *suffix = channel_suffix(req->uri);
    if (!suffix) return send_error(req, 400, "Invalid channel URL");

    // IO ownership guard for mutating channel endpoints
#if defined(BB_IO_OWNERSHIP) && BB_IO_OWNERSHIP
    {
        // Extract channel number from URI: /api/channel/N/...
        const char *p = strstr(req->uri, "/api/channel/");
        if (p) {
            int ch = atoi(p + 13);
            if (ch >= 0 && ch < 4) {
                uint32_t fp = io_owner_compute_token_fp(auth_get_admin_token());
                io_owner_t caller = { IO_OWNER_HTTP, (uint8_t)(fp & 0xFF), fp };
                uint64_t now_ms = (uint64_t)(esp_timer_get_time() / 1000LL);
                int rc = io_owner_guard_or_auto(io_owner_slot_for_ch((uint8_t)ch),
                                                caller.kind, caller.session_id, caller.token_fp32, now_ms);
                if (rc != 0) return send_error(req, 409, "IO slot owned by another session");
            }
        }
    }
#endif

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
    p += 10; // skip past "/api/gpio/"
    while (*p >= '0' && *p <= '9') p++; // skip pin digits (1 or 2)
    if (*p == '/') return p + 1;
    if (*p == '\0') return "";
    return NULL;
}

// POST /api/gpio/* dispatcher
esp_err_t handle_gpio_post_dispatch(httpd_req_t *req)
{
    if (check_admin_auth(req) != ESP_OK) return send_error(req, 401, "Admin token required");

    const char *suffix = gpio_suffix(req->uri);
    if (!suffix) return send_error(req, 400, "Invalid GPIO URL");

    if (strcmp(suffix, "config") == 0)  return handle_post_gpio_config(req);
    if (strcmp(suffix, "set") == 0)     return handle_post_gpio_set(req);

    return send_error(req, 404, "Unknown GPIO POST endpoint");
}
