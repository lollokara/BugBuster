// =============================================================================
// webserver_update.cpp - HTTP handlers: firmware update and DAQ HAT image upload.
// Routes are registered in webserver.cpp (initWebServer).
// =============================================================================

#include "webserver_internal.h"

static const char* TAG = "webserver";

// ---------------------------------------------------------------------------
// DAQ HAT (P4 / C6) direct image upload.
//
// Progress rides this request's own response stream. esp_http_server is
// single-task, so during the ~60-90 s (P4) or ~3 min (C6) push NO other HTTP
// request is served -- a client polling /api/update/status would simply hang.
// The stream also reports the honest number: bytes the P4 acknowledged, not
// bytes buffered into our socket, which an upload-progress bar would race
// ahead of and finish early.
//
// The 200 status is committed before the outcome is known, so failures appear
// ONLY as a final {"stage":"done","ok":false,...} record. Clients must treat a
// stream that ends without a done record as a failure.
// ---------------------------------------------------------------------------
typedef struct {
    httpd_req_t *req;
    int          remaining;   // body bytes not yet handed to update_manager
    bool         send_failed;
} DaqUploadCtx;

static int daq_upload_read(void *vctx, uint8_t *buf, size_t max)
{
    DaqUploadCtx *c = (DaqUploadCtx *)vctx;
    if (c->remaining <= 0) return 0;
    size_t want = ((size_t)c->remaining < max) ? (size_t)c->remaining : max;
    while (true) {
        int n = upload_recv(c->req, (char *)buf, want);
        if (n <= 0) return -1;
        c->remaining -= n;
        return n;
    }
}

static void daq_upload_emit(void *vctx, const char *json)
{
    DaqUploadCtx *c = (DaqUploadCtx *)vctx;
    if (c->send_failed) return;
    char line[224];
    int n = snprintf(line, sizeof(line), "%s\n", json);
    if (n <= 0 || n >= (int)sizeof(line)) return;
    if (httpd_resp_send_chunk(c->req, line, n) != ESP_OK) c->send_failed = true;
}

static esp_err_t handle_daq_upload(httpd_req_t *req, uint32_t target)
{
    if (check_admin_auth(req) != ESP_OK) {
        return send_error(req, 401, "Admin token required");
    }

    uint8_t sha[32];
    bool have_sha = false;
    if (!parse_sha256_query(req, sha, &have_sha) || !have_sha) {
        return send_error(req, 400, "sha256 query must be 64 lowercase hex chars");
    }
    if (req->content_len <= 0 || req->content_len > 3584 * 1024) {
        return send_error(req, 400, "Invalid firmware size (max 3.5MB)");
    }
    if ((update_manager_available_targets() & target) == 0) {
        return send_error(req, 409, "No DAQ HAT connected");
    }

    // Past this point the status is committed; every outcome is a stream record.
    httpd_resp_set_type(req, "application/x-ndjson");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");

    DaqUploadCtx ctx = { .req = req, .remaining = (int)req->content_len, .send_failed = false };
    char err[160] = {0};
    esp_err_t rc = update_manager_push_local(target, (uint32_t)req->content_len, sha,
                                             daq_upload_read, daq_upload_emit,
                                             &ctx, err, sizeof(err));

    // Sized from sizeof(err), not an independent literal: cJSON_PrintUnformatted can
    // double every '"'/'\\' and expand control chars to \uXXXX (6x per byte) in the
    // worst case, so the record buffer must scale with the source buffer it escapes
    // or a long/adversarial err string truncates mid-escape into invalid JSON -- the
    // one thing this endpoint must never emit, since clients treat "done" as the
    // sole authoritative outcome record.
    char last[sizeof(err) * 2 + 64];
    if (rc == ESP_OK) {
        snprintf(last, sizeof(last), "{\"stage\":\"done\",\"ok\":true}\n");
    } else {
        cJSON *e = cJSON_CreateString(err[0] ? err : esp_err_to_name(rc));
        char *esc = e ? cJSON_PrintUnformatted(e) : NULL;   // properly escaped JSON string
        snprintf(last, sizeof(last), "{\"stage\":\"done\",\"ok\":false,\"error\":%s}\n",
                 esc ? esc : "\"unknown\"");
        if (esc) cJSON_free(esc);
        cJSON_Delete(e);
        ESP_LOGE(TAG, "DAQ upload failed: %s", err);
    }
    httpd_resp_send_chunk(req, last, HTTPD_RESP_USE_STRLEN);
    httpd_resp_send_chunk(req, NULL, 0);          // terminate the chunked response
    return ESP_OK;
}

esp_err_t handle_p4_upload(httpd_req_t *req)
{
    return handle_daq_upload(req, UPDATE_TARGET_P4);
}

esp_err_t handle_c6_upload(httpd_req_t *req)
{
    return handle_daq_upload(req, UPDATE_TARGET_C6);
}


// Helper: stringify esp_ota_img_states_t for JSON.
static const char* ota_state_str(esp_ota_img_states_t s)
{
    switch (s) {
        case ESP_OTA_IMG_NEW:            return "NEW";
        case ESP_OTA_IMG_PENDING_VERIFY: return "PENDING_VERIFY";
        case ESP_OTA_IMG_VALID:          return "VALID";
        case ESP_OTA_IMG_INVALID:        return "INVALID";
        case ESP_OTA_IMG_ABORTED:        return "ABORTED";
        case ESP_OTA_IMG_UNDEFINED:      return "UNDEFINED";
        default:                         return "UNKNOWN";
    }
}

// GET /api/ota/info — running/next partition + ota_state + rollback availability
esp_err_t handle_get_ota_info(httpd_req_t *req)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    const esp_partition_t *next    = esp_ota_get_next_update_partition(NULL);
    const esp_partition_t *invalid = esp_ota_get_last_invalid_partition();

    cJSON *root = cJSON_CreateObject();

    if (running) {
        cJSON *r = cJSON_CreateObject();
        cJSON_AddStringToObject(r, "label", running->label);
        cJSON_AddNumberToObject(r, "address", running->address);
        cJSON_AddNumberToObject(r, "size", running->size);
        esp_ota_img_states_t st = ESP_OTA_IMG_UNDEFINED;
        if (esp_ota_get_state_partition(running, &st) == ESP_OK) {
            cJSON_AddStringToObject(r, "state", ota_state_str(st));
        }
        cJSON_AddItemToObject(root, "running", r);
    }
    if (next) {
        cJSON *n = cJSON_CreateObject();
        cJSON_AddStringToObject(n, "label", next->label);
        cJSON_AddNumberToObject(n, "address", next->address);
        cJSON_AddNumberToObject(n, "size", next->size);
        cJSON_AddItemToObject(root, "next", n);
    }
    if (invalid) {
        cJSON *inv = cJSON_CreateObject();
        cJSON_AddStringToObject(inv, "label", invalid->label);
        cJSON_AddItemToObject(root, "lastInvalid", inv);
    }

    cJSON_AddBoolToObject(root, "canRollback", esp_ota_check_rollback_is_possible());
    cJSON_AddNumberToObject(root, "fwMajor", BBP_FW_VERSION_MAJOR);
    cJSON_AddNumberToObject(root, "fwMinor", BBP_FW_VERSION_MINOR);
    cJSON_AddNumberToObject(root, "fwPatch", BBP_FW_VERSION_PATCH);
    return send_json(req, root);
}

// POST /api/ota/rollback — admin-gated; marks running app invalid + reboots to
// the previously-valid partition. Returns 409 if no rollback target exists
// (e.g. only one OTA slot has ever been written) — caller should check
// /api/ota/info `canRollback` first to give a clean UX.
esp_err_t handle_post_ota_rollback(httpd_req_t *req)
{
    if (check_admin_auth(req) != ESP_OK) {
        return send_error(req, 401, "Admin token required");
    }
    if (!esp_ota_check_rollback_is_possible()) {
        return send_error(req, 409, "No rollback target available");
    }

    ESP_LOGW(TAG, "OTA rollback requested — rebooting to previous slot");

    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "success", true);
    cJSON_AddStringToObject(root, "message", "Rolling back; device rebooting");
    esp_err_t ret = send_json(req, root);

    // Let the response flush before invalidating + rebooting.
    vTaskDelay(pdMS_TO_TICKS(500));
    // Returns ESP_FAIL only if rollback isn't possible — already checked above.
    esp_ota_mark_app_invalid_rollback_and_reboot();
    // Should not reach here.
    return ret;
}

esp_err_t handle_get_update_check(httpd_req_t *req)
{
    if (check_admin_auth(req) != ESP_OK) {
        return send_error(req, 401, "Admin token required");
    }
    // MUST delegate: update_manager_check() performs an HTTPS fetch
    // (esp_http_client_perform + software-AES mbedTLS) that needs ~16 KB of
    // stack, and the httpd task has 4096 (see config.stack_size below).
    // Calling it inline here rebooted the board on every request -- this is
    // the exact reproducer the api_core.cpp comment documents, which was
    // fixed only on the BLE side because /api/ota/check has no HTTP route and
    // this route never went through api_core_handle().
    // api_core's handler runs the query on a dedicated 16 KB worker.
    // See docs/superpowers/reviews/2026-08-03-design-sweep.md finding S1-2.
    char *resp = api_core_handle("GET", "/api/ota/check", NULL);
    return send_api_core_result(req, resp, "Update check failed");
}

esp_err_t handle_get_update_status(httpd_req_t *req)
{
    if (check_admin_auth(req) != ESP_OK) {
        return send_error(req, 401, "Admin token required");
    }
    char *resp = api_core_handle("GET", "/api/update/status", NULL);
    return send_api_core_result(req, resp, "Update status unavailable");
}

typedef struct { uint32_t targets; } http_update_apply_args_t;

static void http_update_apply_task(void *arg)
{
    http_update_apply_args_t *a = (http_update_apply_args_t *)arg;
    uint32_t targets = a->targets;
    free(a);
    cJSON *root = NULL;
    esp_err_t err = update_manager_apply(targets, &root);
    if (root) cJSON_Delete(root);
    bool reboot = update_manager_reboot_pending();
    if (err == ESP_OK && reboot) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        esp_restart();
    }
    // MUST be vTaskDeleteWithCaps: this task was created with
    // xTaskCreatePinnedToCoreWithCaps, which allocates the stack and TCB itself.
    // Plain vTaskDelete() cannot free that memory, leaking the whole stack --
    // 12 KB of INTERNAL RAM per update. Measured: internal free 31 -> 19 KB and
    // largest free block 14 -> 8 KB after a single update, never recovering, so
    // the second update in a boot failed to start at all (the worker needs a
    // 12 KB contiguous internal block).
    vTaskDeleteWithCaps(NULL);
}

esp_err_t handle_post_update_apply(httpd_req_t *req)
{
    if (check_admin_auth(req) != ESP_OK) {
        return send_error(req, 401, "Admin token required");
    }
    // Legacy default (empty body) stays rp2040+esp32, but ANY explicit body
    // selects targets from scratch. The old code defaulted both to true and only
    // ever looked at "rp2040"/"esp32", so a body naming just the DAQ targets
    // silently updated the mainboard and LA HAT instead of the chips asked for.
    uint32_t targets = UPDATE_TARGET_RP2040 | UPDATE_TARGET_ESP32;
    if (req->content_len > 0) {
        cJSON *body = recv_json_body(req);
        if (!body) return send_error(req, 400, "Invalid JSON");
        static const struct { const char *key; uint32_t bit; } kMap[] = {
            { "rp2040", UPDATE_TARGET_RP2040 },
            { "esp32",  UPDATE_TARGET_ESP32  },
            { "p4",     UPDATE_TARGET_P4     },
            { "c6",     UPDATE_TARGET_C6     },
        };
        bool named_any = false;
        for (size_t i = 0; i < sizeof(kMap) / sizeof(kMap[0]); i++) {
            if (cJSON_IsBool(cJSON_GetObjectItem(body, kMap[i].key))) { named_any = true; break; }
        }
        if (named_any) {
            targets = 0;
            for (size_t i = 0; i < sizeof(kMap) / sizeof(kMap[0]); i++) {
                cJSON *j = cJSON_GetObjectItem(body, kMap[i].key);
                if (cJSON_IsTrue(j)) targets |= kMap[i].bit;
            }
        }
        cJSON_Delete(body);
    }
    if (targets == 0) return send_error(req, 400, "no update target selected");

    http_update_apply_args_t *args = (http_update_apply_args_t *)malloc(sizeof(http_update_apply_args_t));
    if (!args) return send_error(req, 500, "OOM");
    args->targets = targets;

    // 12 KB internal-RAM stack — must NOT use PSRAM (D-cache disable during flash
    // corrupts PSRAM stacks on both cores via cross-core IPC).
    BaseType_t ok = xTaskCreatePinnedToCoreWithCaps(
        http_update_apply_task, "http_update", 12288,
        args, 5, NULL, 0, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (ok != pdPASS) {
        free(args);
        return send_error(req, 500, "Failed to start update task");
    }

    cJSON *resp = cJSON_CreateObject();
    cJSON_AddBoolToObject(resp, "started", true);
    return send_json(req, resp);
}

// POST /api/ota/uploadfs — Upload SPIFFS filesystem image (binary body = spiffs.bin)
// Unmounts SPIFFS, writes the raw image to the partition, and remounts.
// This lets the desktop app / curl push a new web UI without USB access.
esp_err_t handle_uploadfs(httpd_req_t *req)
{
    if (check_admin_auth(req) != ESP_OK) {
        return send_error(req, 401, "Admin token required");
    }

    // Look up by label (not NULL) — there are TWO subtype=spiffs partitions
    // (`spiffs` web UI and `scripts` MicroPython). NULL would return whichever
    // appears first in partitions.csv and silently target the wrong one.
    const esp_partition_t *part = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_SPIFFS, "spiffs");
    if (!part) {
        return send_error(req, 500, "SPIFFS partition not found");
    }

    if (req->content_len <= 0 || (size_t)req->content_len != part->size) {
        ESP_LOGW(TAG, "Rejecting SPIFFS image size %d for partition '%s' (%lu bytes)",
                 req->content_len, part->label, (unsigned long)part->size);
        return send_error(req, 400, "SPIFFS image size must match partition size");
    }

    ESP_LOGI(TAG, "SPIFFS upload started: %d bytes -> partition '%s' (%lu bytes)",
             req->content_len, part->label, (unsigned long)part->size);

    // Must unmount before writing to the raw partition
    esp_vfs_spiffs_unregister("spiffs");

    // Erase sector-by-sector with WDT resets to avoid a watchdog reset during
    // the long blocking erase (4MB = ~1024 sectors × ~30ms = ~30s without resets).
    esp_err_t err = ESP_OK;
    for (size_t off = 0; off < part->size; off += 4096u) {
        err = esp_partition_erase_range(part, off, 4096u);
        if (err != ESP_OK) break;
        esp_task_wdt_reset();
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SPIFFS erase failed: %s", esp_err_to_name(err));
        esp_vfs_spiffs_conf_t conf = { .base_path = "/spiffs", .partition_label = "spiffs",
                                       .max_files = 5, .format_if_mount_failed = false };
        esp_vfs_spiffs_register(&conf);
        return send_error(req, 500, esp_err_to_name(err));
    }

    char *buf = (char*)malloc(4096);
    if (!buf) {
        esp_vfs_spiffs_conf_t conf = { .base_path = "/spiffs", .partition_label = "spiffs",
                                       .max_files = 5, .format_if_mount_failed = false };
        esp_vfs_spiffs_register(&conf);
        return send_error(req, 500, "Out of memory");
    }

    int remaining = req->content_len;
    int offset = 0;
    int buf_fill = 0;
    bool failed = false;

    while (remaining > 0) {
        int to_read = 4096 - buf_fill;
        if (to_read > remaining) {
            to_read = remaining;
        }

        int received = upload_recv(req, buf + buf_fill, to_read);
        if (received <= 0) {
            ESP_LOGE(TAG, "SPIFFS recv error at offset %d", offset);
            failed = true;
            break;
        }

        buf_fill += received;
        remaining -= received;

        if (buf_fill == 4096 || remaining == 0) {
            int actual_write = buf_fill;
            if (remaining == 0) {
                actual_write = (buf_fill + 3) & ~3;
                if (actual_write > buf_fill) {
                    memset(buf + buf_fill, 0xFF, actual_write - buf_fill);
                }
            }

            if ((size_t)(offset + actual_write) > part->size) {
                ESP_LOGE(TAG, "SPIFFS upload exceeds partition size at offset %d", offset);
                failed = true;
                break;
            }

            err = esp_partition_write(part, offset, buf, actual_write);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "SPIFFS write failed at offset %d: %s", offset, esp_err_to_name(err));
                failed = true;
                break;
            }

            offset += actual_write;
            buf_fill = 0;

            if (offset % (64 * 1024) == 0 || remaining == 0) {
                ESP_LOGI(TAG, "SPIFFS upload: %d / %d bytes (%d%%)",
                         offset, req->content_len, offset * 100 / req->content_len);
            }
        }
    }

    free(buf);

    // Remount regardless of outcome so the device stays usable
    esp_vfs_spiffs_conf_t conf = { .base_path = "/spiffs", .partition_label = "spiffs",
                                   .max_files = 5, .format_if_mount_failed = false };
    err = esp_vfs_spiffs_register(&conf);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "SPIFFS remount failed: %s", esp_err_to_name(err));
        if (!failed) {
            failed = true;
        }
    }

    if (failed) {
        return send_error(req, 500, "SPIFFS write or remount failed");
    }

    ESP_LOGI(TAG, "SPIFFS upload complete: %d bytes", offset);
    char origin_buf[96];
    set_cors_headers(req, origin_buf, sizeof(origin_buf));
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    httpd_resp_set_hdr(req, "Connection", "close");
    char body[96];
    int body_len = snprintf(body, sizeof(body), "{\"success\":true,\"bytesWritten\":%d}", offset);
    if (body_len < 0 || body_len >= (int)sizeof(body)) {
        return send_error(req, 500, "SPIFFS response encode failed");
    }
    err = httpd_resp_send(req, body, body_len);
    ESP_LOGI(TAG, "SPIFFS upload response sent: %s", esp_err_to_name(err));
    return err;
}
