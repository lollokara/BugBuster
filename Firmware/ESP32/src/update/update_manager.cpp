#include "update_manager.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "bbp.h"
#include "cJSON.h"
#include "esp_app_desc.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "hat.h"
#include "mbedtls/sha256.h"

static const char *TAG = "update_manager";
static const char *MANIFEST_URL =
    "https://github.com/lollokara/BugBuster/releases/download/nightly/bugbuster-update-manifest.json";
static const char *RELEASES_API_URL =
    "https://api.github.com/repos/lollokara/BugBuster/releases?per_page=20";
static const char *RP2040_STAGE_PATH = "/scripts/update-rp2040.bin";

typedef struct {
    update_state_t state;
    char last_error[128];
    char current_step[32];
    uint32_t progress_done;
    uint32_t progress_total;
} UpdateRuntime;

typedef struct {
    char build_id[96];
    char version[32];
    char url[256];
    char sha256_hex[65];
    uint32_t size;
    uint32_t crc32;
} UpdateComponent;

typedef struct {
    char build_id[96];
    char commit[48];
    UpdateComponent rp2040;
    UpdateComponent esp32;
} UpdateManifest;

typedef struct {
    char label[96];
    char tag[48];
    char published_at[32];
    UpdateManifest manifest;
} UpdateOption;

typedef struct {
    char *body;
    size_t cap;
    size_t len;
    bool oom;
    esp_err_t last_err;
    int status;
} HttpTextState;

typedef struct {
    FILE *file;
    mbedtls_sha256_context sha;
    bool hash;
    uint32_t written;
    uint32_t total;
    bool failed;
} FileDownloadState;

typedef struct {
    esp_ota_handle_t ota;
    mbedtls_sha256_context sha;
    uint32_t written;
    uint32_t total;
    bool failed;
} OtaDownloadState;

static UpdateRuntime s_update = {
    .state = UPDATE_STATE_IDLE,
};
static bool s_reboot_pending = false;

static void set_state(update_state_t state, const char *step)
{
    s_update.state = state;
    if (step) {
        snprintf(s_update.current_step, sizeof(s_update.current_step), "%s", step);
    }
}

static void set_error(const char *msg)
{
    s_update.state = UPDATE_STATE_FAILED;
    snprintf(s_update.last_error, sizeof(s_update.last_error), "%s", msg ? msg : "unknown error");
    ESP_LOGW(TAG, "update failed: %s", s_update.last_error);
}

static uint8_t hex_nibble(char c)
{
    if (c >= '0' && c <= '9') return (uint8_t)(c - '0');
    if (c >= 'a' && c <= 'f') return (uint8_t)(10 + c - 'a');
    if (c >= 'A' && c <= 'F') return (uint8_t)(10 + c - 'A');
    return 0xFF;
}

static bool parse_sha256(const char *hex, uint8_t out[32])
{
    if (!hex || strlen(hex) != 64) return false;
    for (int i = 0; i < 32; i++) {
        uint8_t hi = hex_nibble(hex[i * 2]);
        uint8_t lo = hex_nibble(hex[i * 2 + 1]);
        if (hi > 0x0F || lo > 0x0F) return false;
        out[i] = (uint8_t)((hi << 4) | lo);
    }
    return true;
}

static bool sha256_matches(const char *expected_hex, const uint8_t actual[32])
{
    uint8_t expected[32];
    return parse_sha256(expected_hex, expected) && memcmp(expected, actual, 32) == 0;
}

static uint32_t crc32_update(uint32_t crc, const uint8_t *data, size_t len)
{
    crc = ~crc;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; bit++) {
            crc = (crc >> 1) ^ (0xEDB88320u & (uint32_t)-(int32_t)(crc & 1u));
        }
    }
    return ~crc;
}

static esp_err_t text_event_handler(esp_http_client_event_t *evt)
{
    HttpTextState *s = (HttpTextState *)evt->user_data;
    if (!s || evt->event_id != HTTP_EVENT_ON_DATA || s->oom) return ESP_OK;
    if (s->len + evt->data_len + 1 > s->cap) {
        size_t new_cap = s->cap ? s->cap * 2 : 4096;
        while (new_cap < s->len + (size_t)evt->data_len + 1) new_cap *= 2;
        if (new_cap > 512 * 1024) {
            s->oom = true;
            return ESP_OK;
        }
        char *next = (char *)realloc(s->body, new_cap);
        if (!next) {
            s->oom = true;
            return ESP_OK;
        }
        s->body = next;
        s->cap = new_cap;
    }
    memcpy(s->body + s->len, evt->data, evt->data_len);
    s->len += evt->data_len;
    s->body[s->len] = '\0';
    return ESP_OK;
}

static int http_get_text(const char *url, char **body_out)
{
    HttpTextState s = {};
    esp_http_client_config_t cfg = {};
    cfg.url = url;
    cfg.timeout_ms = 20000;
    cfg.crt_bundle_attach = esp_crt_bundle_attach;
    cfg.event_handler = text_event_handler;
    cfg.user_data = &s;
    cfg.method = HTTP_METHOD_GET;
    cfg.buffer_size = 2048;
    cfg.buffer_size_tx = 1024;
    cfg.max_redirection_count = 5;

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) return -1;
    esp_http_client_set_header(client, "User-Agent", "BugBuster-Update/1");
    esp_err_t err = esp_http_client_perform(client);
    int status = (err == ESP_OK) ? esp_http_client_get_status_code(client) : -1;
    esp_http_client_cleanup(client);
    s.last_err = err;
    s.status = status;
    ESP_LOGI(TAG, "HTTP GET %s -> err=%s status=%d len=%u oom=%u",
             url, esp_err_to_name(err), status, (unsigned)s.len, (unsigned)s.oom);
    if (s.oom || status < 200 || status >= 300) {
        free(s.body);
        return status;
    }
    *body_out = s.body;
    return status;
}

static bool read_component(cJSON *root, const char *name, UpdateComponent *out)
{
    cJSON *obj = cJSON_GetObjectItem(root, name);
    if (!cJSON_IsObject(obj)) return false;
    cJSON *build = cJSON_GetObjectItem(obj, "buildId");
    cJSON *version = cJSON_GetObjectItem(obj, "version");
    cJSON *url = cJSON_GetObjectItem(obj, "url");
    cJSON *sha = cJSON_GetObjectItem(obj, "sha256");
    cJSON *size = cJSON_GetObjectItem(obj, "size");
    if (!cJSON_IsString(build) || !cJSON_IsString(url) || !cJSON_IsString(sha) || !cJSON_IsNumber(size)) {
        return false;
    }
    snprintf(out->build_id, sizeof(out->build_id), "%s", build->valuestring);
    snprintf(out->version, sizeof(out->version), "%s", cJSON_IsString(version) ? version->valuestring : "");
    snprintf(out->url, sizeof(out->url), "%s", url->valuestring);
    snprintf(out->sha256_hex, sizeof(out->sha256_hex), "%s", sha->valuestring);
    out->size = (uint32_t)size->valuedouble;
    cJSON *crc = cJSON_GetObjectItem(obj, "crc32");
    out->crc32 = cJSON_IsNumber(crc) ? (uint32_t)crc->valuedouble : 0;
    uint8_t parsed_sha[32];
    return out->size > 0 && parse_sha256(out->sha256_hex, parsed_sha);
}

static bool starts_with(const char *s, const char *prefix)
{
    return s && prefix && strncmp(s, prefix, strlen(prefix)) == 0;
}

static bool ends_with(const char *s, const char *suffix)
{
    if (!s || !suffix) return false;
    size_t sl = strlen(s);
    size_t tl = strlen(suffix);
    return sl >= tl && strcmp(s + sl - tl, suffix) == 0;
}

static bool extract_between(const char *s, const char *prefix, const char *suffix,
                            char *out, size_t out_len)
{
    if (!starts_with(s, prefix) || !ends_with(s, suffix) || !out || out_len == 0) {
        return false;
    }
    size_t prefix_len = strlen(prefix);
    size_t len = strlen(s) - prefix_len - strlen(suffix);
    if (len == 0 || len >= out_len) return false;
    memcpy(out, s + prefix_len, len);
    out[len] = '\0';
    return true;
}

static bool asset_digest_sha256(cJSON *asset, char out[65])
{
    cJSON *digest = cJSON_GetObjectItem(asset, "digest");
    if (!cJSON_IsString(digest) || !starts_with(digest->valuestring, "sha256:")) {
        return false;
    }
    const char *hex = digest->valuestring + strlen("sha256:");
    uint8_t parsed[32];
    if (!parse_sha256(hex, parsed)) return false;
    snprintf(out, 65, "%s", hex);
    return true;
}

static bool fill_component_from_asset(cJSON *asset, bool rp2040, UpdateComponent *out)
{
    cJSON *name = cJSON_GetObjectItem(asset, "name");
    cJSON *url = cJSON_GetObjectItem(asset, "browser_download_url");
    cJSON *size = cJSON_GetObjectItem(asset, "size");
    if (!cJSON_IsString(name) || !cJSON_IsString(url) || !cJSON_IsNumber(size)) {
        return false;
    }

    char version[sizeof(out->version)] = {};
    bool name_ok = rp2040
        ? extract_between(name->valuestring, "bugbuster-hat-rp2040-v", ".bin",
                          version, sizeof(version))
        : extract_between(name->valuestring, "bugbuster-esp32s3-v", "-ota.bin",
                          version, sizeof(version));
    if (!name_ok || !asset_digest_sha256(asset, out->sha256_hex)) {
        return false;
    }

    snprintf(out->version, sizeof(out->version), "%s", version);
    snprintf(out->build_id, sizeof(out->build_id), "%s", version);
    snprintf(out->url, sizeof(out->url), "%s", url->valuestring);
    out->size = (uint32_t)size->valuedouble;
    out->crc32 = 0;
    return out->size > 0;
}

static bool component_available(const UpdateComponent *component)
{
    return component && component->size > 0 && component->url[0] &&
           component->sha256_hex[0];
}

static bool collect_release_options(UpdateOption *options, uint8_t max_options, uint8_t *out_count)
{
    if (!options || !out_count || max_options == 0) return false;
    *out_count = 0;

    char *body = NULL;
    int status = http_get_text(RELEASES_API_URL, &body);
    if (status < 200 || status >= 300 || !body) {
        char msg[128];
        snprintf(msg, sizeof(msg), "GitHub releases API failed (HTTP %d)", status);
        set_error(msg);
        return false;
    }

    UpdateComponent current_rp = {};
    UpdateComponent current_esp = {};
    bool have_rp = false;
    bool have_esp = false;
    char last_rp_url[sizeof(current_rp.url)] = {};
    char last_esp_url[sizeof(current_esp.url)] = {};

    cJSON *root = cJSON_Parse(body);
    free(body);
    if (!cJSON_IsArray(root)) {
        cJSON_Delete(root);
        set_error("GitHub releases API JSON parse failed");
        return false;
    }

    cJSON *release = NULL;
    cJSON_ArrayForEach(release, root) {
        cJSON *tag = cJSON_GetObjectItem(release, "tag_name");
        cJSON *commitish = cJSON_GetObjectItem(release, "target_commitish");
        cJSON *published = cJSON_GetObjectItem(release, "published_at");
        cJSON *assets = cJSON_GetObjectItem(release, "assets");
        if (!cJSON_IsArray(assets)) continue;

        bool changed = false;
        cJSON *asset = NULL;
        cJSON_ArrayForEach(asset, assets) {
            UpdateComponent candidate = {};
            if (fill_component_from_asset(asset, true, &candidate)) {
                current_rp = candidate;
                have_rp = true;
                changed = true;
            }
            if (fill_component_from_asset(asset, false, &candidate)) {
                current_esp = candidate;
                have_esp = true;
                changed = true;
            }
        }

        if (!changed || (!have_rp && !have_esp)) continue;
        if (strcmp(last_rp_url, current_rp.url) == 0 && strcmp(last_esp_url, current_esp.url) == 0) {
            continue;
        }

        UpdateOption *opt = &options[*out_count];
        memset(opt, 0, sizeof(*opt));
        snprintf(opt->tag, sizeof(opt->tag), "%s", cJSON_IsString(tag) ? tag->valuestring : "release");
        snprintf(opt->published_at, sizeof(opt->published_at), "%s",
                 cJSON_IsString(published) ? published->valuestring : "");
        snprintf(opt->label, sizeof(opt->label), "%.40s  RP:%.20s  ESP:%.20s",
                 opt->tag, have_rp ? current_rp.version : "-",
                 have_esp ? current_esp.version : "-");
        snprintf(opt->manifest.build_id, sizeof(opt->manifest.build_id),
                 "%s.release-api", opt->tag);
        if (cJSON_IsString(commitish)) {
            snprintf(opt->manifest.commit, sizeof(opt->manifest.commit), "%s", commitish->valuestring);
        }
        if (have_rp) opt->manifest.rp2040 = current_rp;
        if (have_esp) opt->manifest.esp32 = current_esp;
        snprintf(last_rp_url, sizeof(last_rp_url), "%s", current_rp.url);
        snprintf(last_esp_url, sizeof(last_esp_url), "%s", current_esp.url);

        (*out_count)++;
        if (*out_count >= max_options) break;
    }
    cJSON_Delete(root);

    if (*out_count == 0) {
        set_error("no compatible firmware assets found in GitHub releases");
        return false;
    }
    return true;
}

static bool fetch_manifest_from_releases(UpdateManifest *manifest)
{
    UpdateOption options[1];
    uint8_t count = 0;
    if (!collect_release_options(options, 1, &count) || count == 0) {
        return false;
    }
    *manifest = options[0].manifest;
    ESP_LOGI(TAG, "Release API fallback selected RP2040=%s ESP32=%s",
             manifest->rp2040.version, manifest->esp32.version);
    return true;
}

static bool fetch_manifest(UpdateManifest *manifest)
{
    char *body = NULL;
    int status = http_get_text(MANIFEST_URL, &body);
    if (status < 200 || status >= 300 || !body) {
        if (status == 404) {
            ESP_LOGW(TAG, "nightly manifest missing; falling back to GitHub releases API");
            return fetch_manifest_from_releases(manifest);
        }
        char msg[128];
        snprintf(msg, sizeof(msg), "manifest download failed (HTTP %d)", status);
        set_error(msg);
        return false;
    }
    cJSON *root = cJSON_Parse(body);
    free(body);
    if (!root) {
        set_error("manifest JSON parse failed");
        return false;
    }
    cJSON *build = cJSON_GetObjectItem(root, "buildId");
    cJSON *commit = cJSON_GetObjectItem(root, "commit");
    snprintf(manifest->build_id, sizeof(manifest->build_id), "%s", cJSON_IsString(build) ? build->valuestring : "");
    snprintf(manifest->commit, sizeof(manifest->commit), "%s", cJSON_IsString(commit) ? commit->valuestring : "");
    bool ok = read_component(root, "rp2040", &manifest->rp2040) &&
              read_component(root, "esp32", &manifest->esp32);
    cJSON_Delete(root);
    if (!ok) {
        ESP_LOGW(TAG, "manifest missing component metadata; falling back to GitHub releases API");
        memset(manifest, 0, sizeof(*manifest));
        return fetch_manifest_from_releases(manifest);
    }
    return ok;
}

static const char *current_esp32_build_id(void)
{
    static char buf[32];
    snprintf(buf, sizeof(buf), "%d.%d.%d", BBP_FW_VERSION_MAJOR, BBP_FW_VERSION_MINOR, BBP_FW_VERSION_PATCH);
    return buf;
}

static const char *current_rp2040_build_id(void)
{
    const HatState *st = hat_get_state();
    static char buf[32];
    if (!st || !st->connected) {
        snprintf(buf, sizeof(buf), "not-connected");
    } else {
        snprintf(buf, sizeof(buf), "bb-hat-%u.%u", st->fw_version_major, st->fw_version_minor);
    }
    return buf;
}

static cJSON *component_json(const char *current, const UpdateComponent *available)
{
    cJSON *obj = cJSON_CreateObject();
    bool has_asset = component_available(available);
    cJSON_AddStringToObject(obj, "currentBuildId", current);
    cJSON_AddBoolToObject(obj, "available", has_asset);
    cJSON_AddStringToObject(obj, "availableBuildId", has_asset ? available->build_id : "");
    cJSON_AddStringToObject(obj, "version", has_asset ? available->version : "");
    cJSON_AddBoolToObject(obj, "updateAvailable",
                          has_asset &&
                          strcmp(current, available->build_id) != 0 &&
                          strcmp(current, available->version) != 0);
    cJSON_AddNumberToObject(obj, "size", has_asset ? available->size : 0);
    cJSON_AddStringToObject(obj, "sha256", has_asset ? available->sha256_hex : "");
    if (has_asset && available->crc32) cJSON_AddNumberToObject(obj, "crc32", available->crc32);
    return obj;
}

static cJSON *option_json(uint8_t index, const UpdateOption *option)
{
    cJSON *obj = cJSON_CreateObject();
    cJSON_AddNumberToObject(obj, "index", index);
    cJSON_AddStringToObject(obj, "label", option->label);
    cJSON_AddStringToObject(obj, "tag", option->tag);
    cJSON_AddStringToObject(obj, "publishedAt", option->published_at);
    cJSON_AddItemToObject(obj, "rp2040",
                          component_json(current_rp2040_build_id(), &option->manifest.rp2040));
    cJSON_AddItemToObject(obj, "esp32",
                          component_json(current_esp32_build_id(), &option->manifest.esp32));
    return obj;
}

static esp_err_t file_event_handler(esp_http_client_event_t *evt)
{
    FileDownloadState *s = (FileDownloadState *)evt->user_data;
    if (!s || evt->event_id != HTTP_EVENT_ON_DATA || s->failed) return ESP_OK;
    if (fwrite(evt->data, 1, evt->data_len, s->file) != (size_t)evt->data_len) {
        s->failed = true;
        return ESP_OK;
    }
    if (s->hash) mbedtls_sha256_update(&s->sha, (const unsigned char *)evt->data, evt->data_len);
    s->written += evt->data_len;
    s_update.progress_done = s->written;
    return ESP_OK;
}

static bool download_file(const UpdateComponent *component, const char *path)
{
    remove(path);
    FILE *f = fopen(path, "wb");
    if (!f) {
        set_error("failed to open RP2040 staging file");
        return false;
    }
    FileDownloadState s = { .file = f, .hash = true, .total = component->size };
    mbedtls_sha256_init(&s.sha);
    mbedtls_sha256_starts(&s.sha, 0);
    s_update.progress_total = component->size;
    s_update.progress_done = 0;

    esp_http_client_config_t cfg = {};
    cfg.url = component->url;
    cfg.timeout_ms = 60000;
    cfg.crt_bundle_attach = esp_crt_bundle_attach;
    cfg.event_handler = file_event_handler;
    cfg.user_data = &s;
    cfg.method = HTTP_METHOD_GET;
    cfg.buffer_size = 4096;
    cfg.buffer_size_tx = 1024;
    cfg.max_redirection_count = 5;
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        fclose(f);
        mbedtls_sha256_free(&s.sha);
        set_error("failed to init HTTP client");
        return false;
    }
    esp_http_client_set_header(client, "User-Agent", "BugBuster-Update/1");
    esp_err_t err = esp_http_client_perform(client);
    int status = (err == ESP_OK) ? esp_http_client_get_status_code(client) : -1;
    esp_http_client_cleanup(client);
    fclose(f);

    uint8_t sha[32];
    mbedtls_sha256_finish(&s.sha, sha);
    mbedtls_sha256_free(&s.sha);
    if (err != ESP_OK || status < 200 || status >= 300 || s.failed ||
        s.written != component->size || !sha256_matches(component->sha256_hex, sha)) {
        remove(path);
        set_error("RP2040 asset download/verify failed");
        return false;
    }
    return true;
}

static bool file_crc32(const char *path, uint32_t *out)
{
    if (!out) return false;
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    uint8_t buf[512];
    uint32_t crc = 0;
    for (;;) {
        size_t n = fread(buf, 1, sizeof(buf), f);
        if (n > 0) crc = crc32_update(crc, buf, n);
        if (n < sizeof(buf)) {
            if (ferror(f)) {
                fclose(f);
                return false;
            }
            break;
        }
    }
    fclose(f);
    *out = crc;
    return true;
}

static bool flash_rp2040_from_file(const UpdateComponent *component, const char *stage_path)
{
    FILE *f = fopen(stage_path, "rb");
    if (!f) {
        set_error("RP2040 staged file missing");
        return false;
    }
    uint32_t expected_crc = component->crc32;
    if (!expected_crc && !file_crc32(stage_path, &expected_crc)) {
        fclose(f);
        set_error("failed to compute RP2040 staged CRC");
        return false;
    }

    if (!hat_fw_begin(component->size, expected_crc)) {
        fclose(f);
        set_error("RP2040 update begin failed");
        return false;
    }

    uint8_t buf[HAT_FRAME_MAX_LEN - 4];
    uint32_t offset = 0;
    s_update.progress_total = component->size;
    s_update.progress_done = 0;
    while (offset < component->size) {
        size_t n = fread(buf, 1, sizeof(buf), f);
        if (n == 0) break;
        uint32_t ack = 0;
        if (!hat_fw_chunk(offset, buf, (uint8_t)n, &ack) || ack != offset + n) {
            fclose(f);
            set_error("RP2040 update chunk failed");
            return false;
        }
        offset += (uint32_t)n;
        s_update.progress_done = offset;
        vTaskDelay(pdMS_TO_TICKS(1));
    }
    fclose(f);
    if (offset != component->size) {
        set_error("RP2040 staged read failed");
        return false;
    }
    HatFwUpdateStatus st = {};
    if (!hat_fw_status(&st) || st.state != 2 || st.actual_crc32 != expected_crc) {
        set_error("RP2040 staged CRC mismatch");
        return false;
    }
    if (!hat_fw_commit()) {
        set_error("RP2040 commit command failed");
        return false;
    }

    for (int i = 0; i < 80; i++) {
        vTaskDelay(pdMS_TO_TICKS(250));
        hat_connect();
        const HatState *hs = hat_get_state();
        if (hs && hs->connected) {
            remove(stage_path);
            return true;
        }
    }
    set_error("RP2040 did not reconnect after update");
    return false;
}

static esp_err_t ota_event_handler(esp_http_client_event_t *evt)
{
    OtaDownloadState *s = (OtaDownloadState *)evt->user_data;
    if (!s || evt->event_id != HTTP_EVENT_ON_DATA || s->failed) return ESP_OK;
    esp_err_t err = esp_ota_write(s->ota, evt->data, evt->data_len);
    if (err != ESP_OK) {
        s->failed = true;
        return ESP_OK;
    }
    mbedtls_sha256_update(&s->sha, (const unsigned char *)evt->data, evt->data_len);
    s->written += evt->data_len;
    s_update.progress_done = s->written;
    return ESP_OK;
}

static bool apply_esp32_ota(const UpdateComponent *component)
{
    const esp_partition_t *part = esp_ota_get_next_update_partition(NULL);
    if (!part) {
        set_error("no ESP32 OTA partition available");
        return false;
    }
    esp_ota_handle_t ota = 0;
    if (esp_ota_begin(part, OTA_WITH_SEQUENTIAL_WRITES, &ota) != ESP_OK) {
        set_error("esp_ota_begin failed");
        return false;
    }
    OtaDownloadState s = { .ota = ota, .total = component->size };
    mbedtls_sha256_init(&s.sha);
    mbedtls_sha256_starts(&s.sha, 0);
    s_update.progress_total = component->size;
    s_update.progress_done = 0;

    esp_http_client_config_t cfg = {};
    cfg.url = component->url;
    cfg.timeout_ms = 120000;
    cfg.crt_bundle_attach = esp_crt_bundle_attach;
    cfg.event_handler = ota_event_handler;
    cfg.user_data = &s;
    cfg.method = HTTP_METHOD_GET;
    cfg.buffer_size = 4096;
    cfg.buffer_size_tx = 1024;
    cfg.max_redirection_count = 5;
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        esp_ota_abort(ota);
        mbedtls_sha256_free(&s.sha);
        set_error("failed to init ESP32 OTA HTTP client");
        return false;
    }
    esp_http_client_set_header(client, "User-Agent", "BugBuster-Update/1");
    esp_err_t err = esp_http_client_perform(client);
    int status = (err == ESP_OK) ? esp_http_client_get_status_code(client) : -1;
    esp_http_client_cleanup(client);

    uint8_t sha[32];
    mbedtls_sha256_finish(&s.sha, sha);
    mbedtls_sha256_free(&s.sha);
    if (err != ESP_OK || status < 200 || status >= 300 || s.failed ||
        s.written != component->size || !sha256_matches(component->sha256_hex, sha)) {
        esp_ota_abort(ota);
        set_error("ESP32 OTA download/verify failed");
        return false;
    }
    if (esp_ota_end(ota) != ESP_OK || esp_ota_set_boot_partition(part) != ESP_OK) {
        set_error("ESP32 OTA finalize failed");
        return false;
    }
    set_state(UPDATE_STATE_REBOOTING, "rebooting");
    s_reboot_pending = true;
    return true;
}

void update_manager_init(void)
{
    s_update.state = UPDATE_STATE_IDLE;
    s_reboot_pending = false;
    s_update.last_error[0] = '\0';
    snprintf(s_update.current_step, sizeof(s_update.current_step), "idle");
}

esp_err_t update_manager_check(cJSON **out)
{
    if (!out) return ESP_ERR_INVALID_ARG;
    set_state(UPDATE_STATE_CHECKING, "checking");
    UpdateManifest manifest = {};
    if (!fetch_manifest(&manifest)) return ESP_FAIL;
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "channel", "nightly");
    cJSON_AddStringToObject(root, "manifestBuildId", manifest.build_id);
    cJSON_AddStringToObject(root, "commit", manifest.commit);
    cJSON_AddItemToObject(root, "rp2040", component_json(current_rp2040_build_id(), &manifest.rp2040));
    cJSON_AddItemToObject(root, "esp32", component_json(current_esp32_build_id(), &manifest.esp32));
    set_state(UPDATE_STATE_IDLE, "idle");
    *out = root;
    return ESP_OK;
}

esp_err_t update_manager_apply(bool update_rp2040, bool update_esp32, cJSON **out)
{
    if (!out) return ESP_ERR_INVALID_ARG;
    set_state(UPDATE_STATE_CHECKING, "checking");
    s_update.last_error[0] = '\0';

    bool has_local_rp = false;
    struct stat local_st;
    if (stat(RP2040_STAGE_PATH, &local_st) == 0 && local_st.st_size >= 16384) {
        has_local_rp = true;
    } else if (stat("/scripts/update-rp2040.py", &local_st) == 0 && local_st.st_size >= 16384) {
        has_local_rp = true;
    }

    bool need_manifest = update_esp32 || (update_rp2040 && !has_local_rp);

    UpdateManifest manifest = {};
    if (need_manifest) {
        if (!fetch_manifest(&manifest)) return ESP_FAIL;
    }

    bool rp_newer = component_available(&manifest.rp2040) &&

                    strcmp(current_rp2040_build_id(), manifest.rp2040.build_id) != 0 &&
                    strcmp(current_rp2040_build_id(), manifest.rp2040.version) != 0;
    bool esp_newer = component_available(&manifest.esp32) &&
                     strcmp(current_esp32_build_id(), manifest.esp32.build_id) != 0 &&
                     strcmp(current_esp32_build_id(), manifest.esp32.version) != 0;
    
    ESP_LOGI(TAG, "Update check - ESP32: current = %s, available = %s (newer=%d)",
             current_esp32_build_id(), manifest.esp32.build_id, esp_newer);
    ESP_LOGI(TAG, "Update check - RP2040: current = %s, available = %s (newer=%d)",
             current_rp2040_build_id(), manifest.rp2040.build_id, rp_newer);

    bool did_rp = false;
    bool did_esp = false;

    if (update_rp2040) {
        bool has_local = false;
        char local_path[64];
        struct stat st_buf;
        
        strcpy(local_path, RP2040_STAGE_PATH);
        if (stat(local_path, &st_buf) == 0 && st_buf.st_size > 0) {
            uint32_t expected_size = manifest.rp2040.size;
            if (expected_size > 0 && st_buf.st_size != expected_size) {
                ESP_LOGW(TAG, "Local staged image %s size mismatch: %lu vs expected %lu. Discarding.",
                         local_path, (unsigned long)st_buf.st_size, (unsigned long)expected_size);
                remove(local_path);
            } else if (st_buf.st_size < 16384) {
                ESP_LOGW(TAG, "Local staged image %s size is too small: %lu. Discarding.",
                         local_path, (unsigned long)st_buf.st_size);
                remove(local_path);
            } else {
                has_local = true;
            }
        }
        
        if (!has_local) {
            strcpy(local_path, "/scripts/update-rp2040.py");
            if (stat(local_path, &st_buf) == 0 && st_buf.st_size > 0) {
                uint32_t expected_size = manifest.rp2040.size;
                if (expected_size > 0 && st_buf.st_size != expected_size) {
                    ESP_LOGW(TAG, "Local staged image %s size mismatch: %lu vs expected %lu. Discarding.",
                             local_path, (unsigned long)st_buf.st_size, (unsigned long)expected_size);
                    remove(local_path);
                } else if (st_buf.st_size < 16384) {
                    ESP_LOGW(TAG, "Local staged image %s size is too small: %lu. Discarding.",
                             local_path, (unsigned long)st_buf.st_size);
                    remove(local_path);
                } else {
                    has_local = true;
                }
            }
        }

        if (has_local) {
            ESP_LOGI(TAG, "Found local staged RP2040 image %s (%ld bytes), flashing directly", local_path, st_buf.st_size);
            UpdateComponent local_comp = {};
            local_comp.size = st_buf.st_size;
            if (!file_crc32(local_path, &local_comp.crc32)) {
                set_error("failed to compute local staged CRC");
                return ESP_FAIL;
            }
            set_state(UPDATE_STATE_FLASHING_RP2040, "flash_rp2040");
            if (!flash_rp2040_from_file(&local_comp, local_path)) return ESP_FAIL;
            did_rp = true;
        } else if (rp_newer) {
            set_state(UPDATE_STATE_DOWNLOADING_RP2040, "download_rp2040");
            if (!download_file(&manifest.rp2040, RP2040_STAGE_PATH)) return ESP_FAIL;
            set_state(UPDATE_STATE_FLASHING_RP2040, "flash_rp2040");
            if (!flash_rp2040_from_file(&manifest.rp2040, RP2040_STAGE_PATH)) return ESP_FAIL;
            did_rp = true;
        }
    }

    if (update_esp32 && esp_newer) {
        did_esp = true;
        set_state(UPDATE_STATE_DOWNLOADING_ESP32, "download_esp32");
        if (!apply_esp32_ota(&manifest.esp32)) return ESP_FAIL;
    }

    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "success", true);
    cJSON_AddBoolToObject(root, "rp2040Updated", did_rp);
    cJSON_AddBoolToObject(root, "esp32Updated", did_esp);
    cJSON_AddItemToObject(root, "status", update_manager_status_json());
    if (!did_esp) {
        set_state(UPDATE_STATE_IDLE, "idle");
    }
    *out = root;
    return ESP_OK;
}

esp_err_t update_manager_release_options(uint8_t max_options, cJSON **out)
{
    if (!out || max_options == 0) return ESP_ERR_INVALID_ARG;
    if (max_options > 5) max_options = 5;
    set_state(UPDATE_STATE_CHECKING, "checking_releases");
    s_update.last_error[0] = '\0';

    UpdateOption *options = (UpdateOption *)malloc(sizeof(UpdateOption) * 5);
    if (!options) { set_error("OOM for release options"); return ESP_ERR_NO_MEM; }
    uint8_t count = 0;
    if (!collect_release_options(options, max_options, &count)) {
        free(options);
        return ESP_FAIL;
    }

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "currentRp2040", current_rp2040_build_id());
    cJSON_AddStringToObject(root, "currentEsp32", current_esp32_build_id());
    cJSON *arr = cJSON_CreateArray();
    for (uint8_t i = 0; i < count; i++) {
        cJSON_AddItemToArray(arr, option_json(i, &options[i]));
    }
    cJSON_AddItemToObject(root, "options", arr);
    free(options);
    set_state(UPDATE_STATE_IDLE, "idle");
    *out = root;
    return ESP_OK;
}

esp_err_t update_manager_apply_release_index(uint8_t index, bool update_rp2040,
                                             bool update_esp32, cJSON **out)
{
    if (!out || index >= 5) return ESP_ERR_INVALID_ARG;
    set_state(UPDATE_STATE_CHECKING, "checking_releases");
    s_update.last_error[0] = '\0';

    UpdateOption *options = (UpdateOption *)malloc(sizeof(UpdateOption) * 5);
    if (!options) { set_error("OOM for release options"); return ESP_ERR_NO_MEM; }
    uint8_t count = 0;
    if (!collect_release_options(options, 5, &count)) { free(options); return ESP_FAIL; }
    if (index >= count) {
        free(options);
        set_error("selected firmware release is no longer available");
        return ESP_FAIL;
    }

    UpdateManifest manifest = options[index].manifest;
    char selected_label[128] = {};
    snprintf(selected_label, sizeof(selected_label), "%s", options[index].label);
    free(options);
    bool did_rp = false;
    bool did_esp = false;
    bool rp_newer = component_available(&manifest.rp2040) &&
                    strcmp(current_rp2040_build_id(), manifest.rp2040.build_id) != 0 &&
                    strcmp(current_rp2040_build_id(), manifest.rp2040.version) != 0;
    bool esp_newer = component_available(&manifest.esp32) &&
                     strcmp(current_esp32_build_id(), manifest.esp32.build_id) != 0 &&
                     strcmp(current_esp32_build_id(), manifest.esp32.version) != 0;

    ESP_LOGI(TAG, "Applying release - ESP32: current = %s, target = %s (newer=%d)",
             current_esp32_build_id(), manifest.esp32.build_id, esp_newer);
    ESP_LOGI(TAG, "Applying release - RP2040: current = %s, target = %s (newer=%d)",
             current_rp2040_build_id(), manifest.rp2040.build_id, rp_newer);

    if (update_rp2040 && rp_newer) {
        set_state(UPDATE_STATE_DOWNLOADING_RP2040, "download_rp2040");
        if (!download_file(&manifest.rp2040, RP2040_STAGE_PATH)) return ESP_FAIL;
        set_state(UPDATE_STATE_FLASHING_RP2040, "flash_rp2040");
        if (!flash_rp2040_from_file(&manifest.rp2040, RP2040_STAGE_PATH)) return ESP_FAIL;
        did_rp = true;
    }

    if (update_esp32 && esp_newer) {
        did_esp = true;
        set_state(UPDATE_STATE_DOWNLOADING_ESP32, "download_esp32");
        if (!apply_esp32_ota(&manifest.esp32)) return ESP_FAIL;
    }

    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "success", true);
    cJSON_AddStringToObject(root, "selected", selected_label);
    cJSON_AddBoolToObject(root, "rp2040Updated", did_rp);
    cJSON_AddBoolToObject(root, "esp32Updated", did_esp);
    cJSON_AddItemToObject(root, "status", update_manager_status_json());
    if (!did_esp) set_state(UPDATE_STATE_IDLE, "idle");
    *out = root;
    return ESP_OK;
}

cJSON *update_manager_status_json(void)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "state", s_update.state);
    cJSON_AddStringToObject(root, "step", s_update.current_step);
    cJSON_AddStringToObject(root, "lastError", s_update.last_error);
    cJSON_AddNumberToObject(root, "progressDone", s_update.progress_done);
    cJSON_AddNumberToObject(root, "progressTotal", s_update.progress_total);
    cJSON_AddStringToObject(root, "currentRp2040", current_rp2040_build_id());
    cJSON_AddStringToObject(root, "currentEsp32", current_esp32_build_id());
    return root;
}

bool update_manager_reboot_pending(void)
{
    return s_reboot_pending;
}
