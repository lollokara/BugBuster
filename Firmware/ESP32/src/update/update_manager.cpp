#include "update_manager.h"

#include <atomic>
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
    // Which MCU the current phase is working on, so a client showing a progress
    // bar for a multi-target sequence can say WHAT is being updated rather than
    // just how far along the whole run is. 0 when idle.
    uint32_t active_target;
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
    // DAQ HAT images. Optional: no workflow publishes these yet (that is
    // sub-project 2), so a manifest without them must still parse.
    UpdateComponent p4;
    UpdateComponent c6;
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

// ---------------------------------------------------------------------------
// Apply reentrancy guard.
//
// There are FOUR independent entry points into an apply: the HTTP worker
// (webserver.cpp handle_post_update_apply), the BLE dispatcher
// (api_core.cpp api_ota_apply), the CLI command (cli_cmds_sys.cpp) and the
// TUI menu (cli_menu.cpp). The latter two each had their own private
// done/busy flag; HTTP and BLE had none, and no flag was visible to any other
// entry point.
//
// Two concurrent applies resolve the SAME esp_ota_get_next_update_partition()
// and both esp_ota_begin() it -- the second erases the partition under the
// first -- then interleave 4 KB writes into it. Whichever finishes first sets
// the boot partition to a byte-interleaved image of two downloads. That is a
// rollback at best and a brick where rollback is disabled. HTTP-apply
// concurrent with TUI-menu-apply is genuinely reachable: different tasks,
// different guards.
//
// Guarding inside update_manager itself (rather than at each call site) means
// every present and future entry point is covered without having to remember.
// See docs/superpowers/reviews/2026-08-03-design-sweep.md finding S1-3.
static std::atomic<bool> s_apply_busy{false};

// RAII so that every early return in the long apply functions releases the
// guard. esp_restart() paths skip the destructor, but the device is rebooting.
namespace {
struct ApplyGuard {
    bool held;
    ApplyGuard()
    {
        bool expected = false;
        held = s_apply_busy.compare_exchange_strong(expected, true);
    }
    ~ApplyGuard()
    {
        if (held) s_apply_busy.store(false);
    }
    ApplyGuard(const ApplyGuard &) = delete;
    ApplyGuard &operator=(const ApplyGuard &) = delete;
};
}  // namespace

// Phase name for a target, used in the status JSON alongside the numeric mask.
static const char *target_name(uint32_t target)
{
    switch (target) {
        case UPDATE_TARGET_RP2040: return "rp2040";
        case UPDATE_TARGET_ESP32:  return "esp32";
        case UPDATE_TARGET_P4:     return "p4";
        case UPDATE_TARGET_C6:     return "c6";
        default:                   return "";
    }
}

static void set_target(uint32_t target)
{
    s_update.active_target = target;
}

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
    // Optional by design -- absence means "this release has no DAQ HAT image",
    // not a malformed manifest. read_component zeroes nothing on failure, so
    // clear first and let component_available() report them as missing.
    memset(&manifest->p4, 0, sizeof(manifest->p4));
    memset(&manifest->c6, 0, sizeof(manifest->c6));
    (void)read_component(root, "p4", &manifest->p4);
    (void)read_component(root, "c6", &manifest->c6);
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

esp_err_t update_manager_flash_rp2040_stage(uint32_t expected_size)
{
    UpdateComponent component = {};
    snprintf(component.build_id, sizeof(component.build_id), "usb-stage");
    component.size = expected_size;
    component.crc32 = 0;
    if (!flash_rp2040_from_file(&component, RP2040_STAGE_PATH)) {
        return ESP_FAIL;
    }
    return ESP_OK;
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

// ---------------------------------------------------------------------------
// DAQ HAT (P4 / C6) image streaming
//
// Bytes go straight from the HTTPS event handler onto the HAT link; there is no
// S3-side staging file. The P4's `staging` partition is already the durable,
// SHA-verified, resumable buffer, and a 2 MB image would eat two thirds of the
// 3 MB `scripts` SPIFFS that holds user MicroPython files.
//
// The S3 does NOT hash the image. The SHA-256 travels in the OTA_BEGIN meta and
// the P4 verifies it at OTA_END, which is also the only correct place: on a
// Range-resume the S3 never sees the earlier bytes, so any hash it computed
// would cover the wrong range.
// ---------------------------------------------------------------------------

typedef struct {
    uint32_t offset;     // next image offset to send, and the resume point
    bool     failed;
} HatOtaDownloadState;

static esp_err_t hat_ota_event_handler(esp_http_client_event_t *evt)
{
    HatOtaDownloadState *s = (HatOtaDownloadState *)evt->user_data;
    if (!s || evt->event_id != HTTP_EVENT_ON_DATA || s->failed) return ESP_OK;

    // The HTTP buffer hands over up to buffer_size bytes at a time; the DAQ link
    // carries HAT_OTA_CHUNK_MAX (236) per frame, so split rather than truncate.
    const uint8_t *p = (const uint8_t *)evt->data;
    int remaining = evt->data_len;
    while (remaining > 0) {
        uint8_t n = (remaining > HAT_OTA_CHUNK_MAX) ? HAT_OTA_CHUNK_MAX : (uint8_t)remaining;
        if (!hat_ota_data(s->offset, p, n)) {
            // Do not retry blindly here: the P4 rejects out-of-order offsets, so
            // recovery has to re-query the received count and resume from there.
            s->failed = true;
            return ESP_OK;
        }
        s->offset += n;
        p += n;
        remaining -= n;
        s_update.progress_done = s->offset;
    }
    return ESP_OK;
}

// Authoritative resume point, straight from the P4. The two targets track it in
// different modules: a P4-target transfer streams to the A/B slot and reports
// `received`, while a staged (C6) transfer reports `relay_staged_bytes`, which
// may trail by up to ~64 KB because relay_stage persists to NVS at that
// interval. Re-sending already-staged bytes is harmless; skipping any is not.
static uint32_t daq_resume_offset(uint8_t hat_target)
{
    hat_daq_ota_status_t st = {};
    if (!hat_daq_ota_status(&st)) return 0;
    return (hat_target == HAT_OTA_TARGET_P4) ? st.received : st.relay_staged_bytes;
}

static bool version_to_u32(const char *v, uint32_t *out)
{
    unsigned a = 0, b = 0, c = 0;
    if (!v || sscanf(v, "%u.%u.%u", &a, &b, &c) < 2) return false;
    *out = ((a & 0xFF) << 16) | ((b & 0xFF) << 8) | (c & 0xFF);
    return true;
}

#define DAQ_OTA_MAX_ATTEMPTS 5

// Stream one image to the DAQ HAT, resuming with an HTTP Range request on a
// stall. @hat_target is HAT_OTA_TARGET_P4 or _STAGE (the C6 goes through
// staging so its image is SHA-verified before the ROM-loader push touches it).
static bool apply_daq_ota(const UpdateComponent *component, uint8_t hat_target)
{
    uint8_t sha[32];
    if (!parse_sha256(component->sha256_hex, sha)) {
        set_error("DAQ image manifest has no usable SHA-256");
        return false;
    }

    hat_ota_meta_t meta = {};
    meta.image_size = component->size;
    (void)version_to_u32(component->version, &meta.version_u32);
    memcpy(meta.sha256, sha, sizeof(meta.sha256));
    snprintf(meta.product_id, sizeof(meta.product_id), "%s",
             (hat_target == HAT_OTA_TARGET_P4) ? "bugbuster-p4" : "bugbuster-c6");

    if (!hat_ota_begin(hat_target, &meta)) {
        set_error("DAQ HAT rejected OTA_BEGIN");
        return false;
    }

    s_update.progress_total = component->size;
    s_update.progress_done = 0;

    bool ok = false;
    for (int attempt = 0; attempt < DAQ_OTA_MAX_ATTEMPTS && !ok; attempt++) {
        // Always resume from the P4's own count, never from a locally tracked
        // one -- after a failed frame the S3 cannot know how much the P4 kept.
        HatOtaDownloadState s = {};
        s.offset = (attempt == 0) ? 0 : daq_resume_offset(hat_target);
        if (s.offset >= component->size) { ok = true; break; }

        esp_http_client_config_t cfg = {};
        cfg.url = component->url;
        cfg.timeout_ms = 120000;
        cfg.crt_bundle_attach = esp_crt_bundle_attach;
        cfg.event_handler = hat_ota_event_handler;
        cfg.user_data = &s;
        cfg.method = HTTP_METHOD_GET;
        cfg.buffer_size = 4096;
        cfg.buffer_size_tx = 1024;
        cfg.max_redirection_count = 5;

        esp_http_client_handle_t client = esp_http_client_init(&cfg);
        if (!client) {
            set_error("failed to init DAQ OTA HTTP client");
            hat_ota_abort();
            return false;
        }
        esp_http_client_set_header(client, "User-Agent", "BugBuster-Update/1");
        char range[48];
        if (s.offset > 0) {
            snprintf(range, sizeof(range), "bytes=%lu-", (unsigned long)s.offset);
            esp_http_client_set_header(client, "Range", range);
        }

        esp_err_t err = esp_http_client_perform(client);
        int status = (err == ESP_OK) ? esp_http_client_get_status_code(client) : -1;
        esp_http_client_cleanup(client);

        // 206 on a resumed request, 200 on a fresh one. A server that ignores
        // Range answers 200 with the whole file, which would restart the P4 at
        // a non-zero offset and be rejected -- treat it as a failed attempt.
        bool http_ok = (err == ESP_OK) &&
                       ((s.offset == 0 && status == 200) || (s.offset > 0 && status == 206));
        ok = http_ok && !s.failed && s.offset >= component->size;
        if (!ok) {
            ESP_LOGW(TAG, "DAQ OTA attempt %d/%d failed (err=%d status=%d sent=%lu/%lu)",
                     attempt + 1, DAQ_OTA_MAX_ATTEMPTS, (int)err, status,
                     (unsigned long)s.offset, (unsigned long)component->size);
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }

    if (!ok) {
        hat_ota_abort();
        set_error("DAQ HAT image transfer failed after retries");
        return false;
    }

    // The P4 verifies SHA-256 here; a mismatch fails the whole transfer.
    if (!hat_ota_end()) {
        set_error("DAQ HAT image failed verification");
        return false;
    }
    return true;
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

// Alternative ESP32 OTA chunk source: pulls the staged image from the DAQ
// HAT (P4)'s `staging` partition over the S3<->P4 UART link (HAT_CMD_STAGE_READ
// / HAT_RSP_STAGE_DATA) instead of downloading it over HTTPS. Used when the
// firmware image arrived at the P4 via the USB protocol path rather than the
// S3's own network connection. Not yet wired to a caller (source selection is
// out of scope for this task) — this only adds the capability.
//
// NOTE: unlike apply_esp32_ota()'s HTTP path, the P4 link caps each transfer
// at HAT_FRAME_MAX_LEN (32) bytes per hat_stage_read() call, so this reads in
// much smaller chunks than the HTTP buffer_size=4096 path above.
esp_err_t apply_esp32_ota_from_p4_stage(void)
{
    // Staged-state safeguard. Without it this function will happily pull and
    // boot whatever bytes happen to be sitting in the P4's staging partition.
    //
    // Two distinct hazards, both fatal to the mainboard:
    //   - state != RELAY_STAGED means the image is partial or its SHA-256 check
    //     has not passed. relay_stage only reaches STAGED after that check.
    //   - target != S3 means those bytes are a C6 image. Writing a C6 image into
    //     the S3's own OTA slot and setting it bootable bricks this board, and
    //     unlike the DAQ HAT there is no second MCU left to recover it.
    hat_daq_ota_status_t st = {};
    if (!hat_daq_ota_status(&st)) {
        set_error("cannot read DAQ HAT staging state");
        return ESP_ERR_INVALID_STATE;
    }
    if (st.relay_state != HAT_RELAY_STAGED) {
        set_error("P4 staging is not in the verified STAGED state");
        return ESP_ERR_INVALID_STATE;
    }
    if (st.relay_target != HAT_RELAY_TARGET_S3) {
        set_error("P4 staging holds an image for another target, not the S3");
        return ESP_ERR_INVALID_STATE;
    }

    const esp_partition_t *part = esp_ota_get_next_update_partition(NULL);
    if (!part) {
        set_error("no ESP32 OTA partition available");
        return ESP_ERR_NOT_FOUND;
    }
    esp_ota_handle_t ota = 0;
    esp_err_t err = esp_ota_begin(part, OTA_WITH_SEQUENTIAL_WRITES, &ota);
    if (err != ESP_OK) {
        set_error("esp_ota_begin failed");
        return err;
    }

    s_update.progress_total = 0; // unknown until EOF from the P4 stage stream
    s_update.progress_done = 0;

    uint8_t buf[HAT_FRAME_MAX_LEN];
    uint32_t offset = 0;
    const int kMaxRetriesPerChunk = 5;

    for (;;) {
        int n = -1;
        for (int attempt = 0; attempt < kMaxRetriesPerChunk && n < 0; attempt++) {
            n = hat_stage_read(offset, buf, sizeof(buf));
            if (n < 0) {
                vTaskDelay(pdMS_TO_TICKS(100 * (attempt + 1)));
            }
        }
        if (n < 0) {
            esp_ota_abort(ota);
            set_error("P4 stage read failed after retries");
            return ESP_FAIL;
        }
        if (n == 0) break; // end of staged image

        err = esp_ota_write(ota, buf, (size_t)n);
        if (err != ESP_OK) {
            esp_ota_abort(ota);
            set_error("esp_ota_write failed (P4 stage source)");
            return err;
        }
        offset += (uint32_t)n;
        s_update.progress_done = offset;
        vTaskDelay(pdMS_TO_TICKS(1));
    }

    err = esp_ota_end(ota);
    if (err != ESP_OK) {
        set_error("ESP32 OTA finalize failed (P4 stage source)");
        return err;
    }
    err = esp_ota_set_boot_partition(part);
    if (err != ESP_OK) {
        set_error("ESP32 OTA set boot partition failed (P4 stage source)");
        return err;
    }
    set_state(UPDATE_STATE_REBOOTING, "rebooting");
    s_reboot_pending = true;
    return ESP_OK;
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

// Bring a freshly-installed P4 image into service.
//
// ota_end() only ARMS the image: it calls esp_ota_set_boot_partition() and
// deliberately does not reboot. With CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y
// the new app boots in PENDING_VERIFY, and the bootloader reverts it on the
// NEXT boot unless ota_confirm() runs. Nothing used to do that, so every P4
// update silently reverted.
//
// Order matters and is asserted by tests: reset -> wait for the link -> read
// the running version -> only then confirm. Confirming a build we have not
// seen boot would defeat rollback, which is the one thing protecting the P4
// from a bad image.
#define DAQ_RELINK_TIMEOUT_MS 20000
#define DAQ_RELINK_POLL_MS      250

static bool daq_activate_p4(char *err, size_t err_len,
                            DaqProgressFn emit, void *ctx)
{
    char line[192];

    uint32_t prev_ver = 0;
    char prev_str[32] = {0};
    (void)hat_get_version(&prev_ver, prev_str, sizeof(prev_str));

    if (emit) emit(ctx, "{\"stage\":\"reset\"}");
    if (!hat_reset()) {
        snprintf(err, err_len, "P4 reset failed");
        return false;
    }

    // The link drops here. Wait for it to come back BEFORE reading the
    // version, so "link not up yet" cannot be mistaken for "old image still
    // running".
    uint32_t waited = 0;
    bool relinked = false;
    while (waited < DAQ_RELINK_TIMEOUT_MS) {
        vTaskDelay(pdMS_TO_TICKS(DAQ_RELINK_POLL_MS));
        waited += DAQ_RELINK_POLL_MS;
        const HatState *hs = hat_get_state();
        if (hs && hs->connected && hs->type == HAT_TYPE_DAQ_POWER) {
            relinked = true;
            break;
        }
    }
    if (!relinked) {
        snprintf(err, err_len, "DAQ HAT did not come back after reset");
        return false;   // unconfirmed -> the bootloader reverts on its own
    }
    if (emit) {
        snprintf(line, sizeof(line),
                 "{\"stage\":\"relink\",\"elapsed_ms\":%lu}", (unsigned long)waited);
        emit(ctx, line);
    }

    uint32_t new_ver = 0;
    char new_str[32] = {0};
    if (!hat_get_version(&new_ver, new_str, sizeof(new_str))) {
        snprintf(err, err_len, "could not read P4 version after reset");
        return false;
    }
    if (emit) {
        snprintf(line, sizeof(line),
                 "{\"stage\":\"version\",\"running\":\"%s\",\"previous\":\"%s\"}",
                 new_str, prev_str);
        emit(ctx, line);
    }

    if (emit) emit(ctx, "{\"stage\":\"confirm\"}");
    if (!hat_ota_confirm()) {
        snprintf(err, err_len, "P4 refused OTA_CONFIRM; image will revert");
        return false;
    }
    return true;
}

// The C6 and P4 legs of a multi-target apply, in UPDATE_TARGET_ORDER. Shared by
// both apply entry points so the ordering rule and the "release has no image"
// checks cannot drift between them.
static bool apply_daq_targets(uint32_t targets, const UpdateManifest *manifest,
                              bool *did_p4, bool *did_c6)
{
    // C6 first: its ROM-loader push is driven BY the P4, so the P4 must still be
    // running its current image to perform it.
    if (targets & UPDATE_TARGET_C6) {
        if (!component_available(&manifest->c6)) {
            set_error("release has no C6 image");
            return false;
        }
        set_target(UPDATE_TARGET_C6);
        set_state(UPDATE_STATE_DOWNLOADING_C6, "download_c6");
        // Staged rather than streamed straight at the C6: staging gets the image
        // SHA-verified inside the P4's partition BEFORE the ROM-loader push
        // starts, and makes that push resumable from pushed_bytes. An aborted
        // push strands the C6 in ROM download mode, so verifying first matters.
        if (!apply_daq_ota(&manifest->c6, HAT_OTA_TARGET_STAGE)) return false;
        set_target(UPDATE_TARGET_C6);
        set_state(UPDATE_STATE_APPLYING_C6, "apply_c6");
        if (!hat_daq_relay_apply()) {
            set_error("DAQ HAT refused the C6 relay apply");
            return false;
        }
        *did_c6 = true;
    }

    if (targets & UPDATE_TARGET_P4) {
        if (!component_available(&manifest->p4)) {
            set_error("release has no P4 image");
            return false;
        }
        set_target(UPDATE_TARGET_P4);
        set_state(UPDATE_STATE_DOWNLOADING_P4, "download_p4");
        if (!apply_daq_ota(&manifest->p4, HAT_OTA_TARGET_P4)) return false;
        char aerr[96] = {0};
        if (!daq_activate_p4(aerr, sizeof(aerr), NULL, NULL)) {
            set_error(aerr);
            return false;
        }
        *did_p4 = true;
    }
    return true;
}

uint32_t update_manager_available_targets(void)
{
    uint32_t mask = UPDATE_TARGET_RP2040 | UPDATE_TARGET_ESP32;
    const HatState *hs = hat_get_state();
    if (hs && hs->connected && hs->type == HAT_TYPE_DAQ_POWER) {
        mask |= UPDATE_TARGETS_DAQ_HAT;
    }
    return mask;
}

// Reject rather than silently drop unavailable targets: a client that asked to
// update the P4 must not be told the update succeeded when there is no DAQ HAT.
static bool targets_are_valid(uint32_t targets, char *err, size_t err_len)
{
    if (targets == 0) {
        snprintf(err, err_len, "no update target selected");
        return false;
    }
    uint32_t unknown = targets & ~(uint32_t)UPDATE_TARGETS_ALL;
    if (unknown) {
        snprintf(err, err_len, "unknown update target bits 0x%lx", (unsigned long)unknown);
        return false;
    }
    uint32_t missing = targets & ~update_manager_available_targets();
    if (missing) {
        snprintf(err, err_len, "target unavailable (no DAQ HAT attached?): bits 0x%lx",
                 (unsigned long)missing);
        return false;
    }
    return true;
}

esp_err_t update_manager_apply(uint32_t targets, cJSON **out)
{
    if (!out) return ESP_ERR_INVALID_ARG;

    ApplyGuard guard;
    if (!guard.held) {
        set_error("an update is already in progress");
        return ESP_ERR_INVALID_STATE;
    }

    char why[96];
    if (!targets_are_valid(targets, why, sizeof(why))) {
        set_error(why);
        return ESP_ERR_INVALID_ARG;
    }
    const bool update_rp2040 = (targets & UPDATE_TARGET_RP2040) != 0;
    const bool update_esp32  = (targets & UPDATE_TARGET_ESP32) != 0;

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
            set_target(UPDATE_TARGET_RP2040);
        set_state(UPDATE_STATE_DOWNLOADING_RP2040, "download_rp2040");
            if (!download_file(&manifest.rp2040, RP2040_STAGE_PATH)) return ESP_FAIL;
            set_state(UPDATE_STATE_FLASHING_RP2040, "flash_rp2040");
            if (!flash_rp2040_from_file(&manifest.rp2040, RP2040_STAGE_PATH)) return ESP_FAIL;
            did_rp = true;
        }
    }

    bool did_c6 = false;
    bool did_p4 = false;
    if (!apply_daq_targets(targets, &manifest, &did_p4, &did_c6)) return ESP_FAIL;

    // ESP32 last: rebooting this MCU ends the sequence.
    if (update_esp32 && esp_newer) {
        did_esp = true;
        set_target(UPDATE_TARGET_ESP32);
        set_state(UPDATE_STATE_DOWNLOADING_ESP32, "download_esp32");
        if (!apply_esp32_ota(&manifest.esp32)) return ESP_FAIL;
    }

    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "success", true);
    cJSON_AddBoolToObject(root, "rp2040Updated", did_rp);
    cJSON_AddBoolToObject(root, "esp32Updated", did_esp);
    cJSON_AddBoolToObject(root, "p4Updated", did_p4);
    cJSON_AddBoolToObject(root, "c6Updated", did_c6);
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

esp_err_t update_manager_apply_release_index(uint8_t index, uint32_t targets, cJSON **out)
{
    if (!out || index >= 5) return ESP_ERR_INVALID_ARG;

    ApplyGuard guard;
    if (!guard.held) {
        set_error("an update is already in progress");
        return ESP_ERR_INVALID_STATE;
    }

    char why[96];
    if (!targets_are_valid(targets, why, sizeof(why))) {
        set_error(why);
        return ESP_ERR_INVALID_ARG;
    }
    const bool update_rp2040 = (targets & UPDATE_TARGET_RP2040) != 0;
    const bool update_esp32  = (targets & UPDATE_TARGET_ESP32) != 0;
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
        set_target(UPDATE_TARGET_RP2040);
        set_state(UPDATE_STATE_DOWNLOADING_RP2040, "download_rp2040");
        if (!download_file(&manifest.rp2040, RP2040_STAGE_PATH)) return ESP_FAIL;
        set_state(UPDATE_STATE_FLASHING_RP2040, "flash_rp2040");
        if (!flash_rp2040_from_file(&manifest.rp2040, RP2040_STAGE_PATH)) return ESP_FAIL;
        did_rp = true;
    }

    bool did_c6 = false;
    bool did_p4 = false;
    if (!apply_daq_targets(targets, &manifest, &did_p4, &did_c6)) return ESP_FAIL;

    if (update_esp32 && esp_newer) {
        did_esp = true;
        set_target(UPDATE_TARGET_ESP32);
        set_state(UPDATE_STATE_DOWNLOADING_ESP32, "download_esp32");
        if (!apply_esp32_ota(&manifest.esp32)) return ESP_FAIL;
    }

    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "success", true);
    cJSON_AddStringToObject(root, "selected", selected_label);
    cJSON_AddBoolToObject(root, "rp2040Updated", did_rp);
    cJSON_AddBoolToObject(root, "esp32Updated", did_esp);
    cJSON_AddBoolToObject(root, "p4Updated", did_p4);
    cJSON_AddBoolToObject(root, "c6Updated", did_c6);
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
    cJSON_AddNumberToObject(root, "activeTarget", s_update.active_target);
    cJSON_AddStringToObject(root, "activeTargetName", target_name(s_update.active_target));
    cJSON_AddNumberToObject(root, "availableTargets", update_manager_available_targets());
    return root;
}

bool update_manager_reboot_pending(void)
{
    return s_reboot_pending;
}
