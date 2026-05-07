// =============================================================================
// mdns_responder.cpp — see mdns_responder.h for the contract.
// =============================================================================

#include "mdns_responder.h"
#include "bbp.h"          // BBP_PROTO_VERSION + firmware version constants
#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_wifi.h"
#include "mdns.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char* TAG = "mdns";
static const char* NVS_NS  = "mdns_cfg";
static const char* NVS_KEY = "hostname";
static const char* INSTANCE_NAME = "BugBuster";
static const char* MODEL_NAME    = "bugbuster-s3";

static char s_hostname[MDNS_HOSTNAME_MAX] = {};
static bool s_initialised = false;

// Validate hostname: lowercase letters, digits, hyphen; must start with letter
// or digit; max length below MDNS_HOSTNAME_MAX. RFC-952 / RFC-1123 friendly.
static bool hostname_is_valid(const char* h)
{
    if (!h || !h[0]) return false;
    size_t n = strlen(h);
    if (n >= MDNS_HOSTNAME_MAX) return false;
    if (h[0] == '-' || h[n-1] == '-') return false;
    for (size_t i = 0; i < n; i++) {
        char c = h[i];
        bool ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-';
        if (!ok) return false;
    }
    return true;
}

static void derive_default_hostname(char* out, size_t out_sz)
{
    uint8_t mac[6] = {};
    // AP MAC is fixed regardless of STA state — gives a stable identifier.
    if (esp_wifi_get_mac(WIFI_IF_AP, mac) != ESP_OK) {
        // Fallback to base MAC if the wifi driver hasn't started yet.
        esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    }
    snprintf(out, out_sz, "bugbuster-%02x%02x%02x", mac[3], mac[4], mac[5]);
}

static bool nvs_load_hostname(char* out, size_t out_sz)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return false;
    size_t len = out_sz;
    esp_err_t err = nvs_get_str(h, NVS_KEY, out, &len);
    nvs_close(h);
    return err == ESP_OK && len > 1 && hostname_is_valid(out);
}

static bool nvs_save_hostname(const char* h)
{
    nvs_handle_t nh;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &nh);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open(%s) failed: %s", NVS_NS, esp_err_to_name(err));
        return false;
    }
    esp_err_t set_err;
    if (h && h[0]) {
        set_err = nvs_set_str(nh, NVS_KEY, h);
    } else {
        // Empty / NULL → erase override so we revert to the derived default.
        set_err = nvs_erase_key(nh, NVS_KEY);
        if (set_err == ESP_ERR_NVS_NOT_FOUND) set_err = ESP_OK;
    }
    if (set_err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_set_str(hostname) failed: %s", esp_err_to_name(set_err));
        nvs_close(nh);
        return false;
    }
    esp_err_t commit_err = nvs_commit(nh);
    nvs_close(nh);
    if (commit_err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_commit(hostname) failed: %s", esp_err_to_name(commit_err));
        return false;
    }
    return true;
}

// Build TXT records once per registration. ESP-IDF copies them internally so a
// stack-allocated array is fine.
static esp_err_t register_services(void)
{
    uint8_t mac[6] = {};
    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    char mac_str[18];
    snprintf(mac_str, sizeof(mac_str), "%02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    char version_str[16];
    snprintf(version_str, sizeof(version_str), "%u.%u.%u",
             BBP_FW_VERSION_MAJOR, BBP_FW_VERSION_MINOR, BBP_FW_VERSION_PATCH);

    char proto_str[8];
    snprintf(proto_str, sizeof(proto_str), "%u", BBP_PROTO_VERSION);

    mdns_txt_item_t txt[] = {
        {"version", version_str},
        {"mac",     mac_str},
        {"proto",   proto_str},
        {"model",   (char*)MODEL_NAME},
    };
    const size_t txt_n = sizeof(txt) / sizeof(txt[0]);

    // _http._tcp — generic HTTP discovery.
    esp_err_t err = mdns_service_add(INSTANCE_NAME, "_http", "_tcp", 80, txt, txt_n);
    if (err != ESP_OK && err != ESP_ERR_NO_MEM) {
        ESP_LOGE(TAG, "mdns_service_add(_http): %s", esp_err_to_name(err));
        return err;
    }

    // _bugbuster._tcp — lets BugBuster-aware tooling filter cleanly even when
    // other HTTP devices share the LAN.
    err = mdns_service_add(INSTANCE_NAME, "_bugbuster", "_tcp", 80, txt, txt_n);
    if (err != ESP_OK && err != ESP_ERR_NO_MEM) {
        ESP_LOGE(TAG, "mdns_service_add(_bugbuster): %s", esp_err_to_name(err));
        return err;
    }
    return ESP_OK;
}

static void unregister_services(void)
{
    mdns_service_remove("_http", "_tcp");
    mdns_service_remove("_bugbuster", "_tcp");
}

bool mdns_responder_init(void)
{
    if (s_initialised) return true;

    esp_err_t err = mdns_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "mdns_init failed: %s", esp_err_to_name(err));
        return false;
    }

    char loaded[MDNS_HOSTNAME_MAX] = {};
    if (nvs_load_hostname(loaded, sizeof(loaded))) {
        strncpy(s_hostname, loaded, sizeof(s_hostname) - 1);
        ESP_LOGI(TAG, "Loaded hostname override from NVS: '%s'", s_hostname);
    } else {
        derive_default_hostname(s_hostname, sizeof(s_hostname));
        ESP_LOGI(TAG, "Using derived hostname: '%s'", s_hostname);
    }

    if ((err = mdns_hostname_set(s_hostname)) != ESP_OK) {
        ESP_LOGE(TAG, "mdns_hostname_set: %s", esp_err_to_name(err));
        return false;
    }
    if ((err = mdns_instance_name_set(INSTANCE_NAME)) != ESP_OK) {
        ESP_LOGE(TAG, "mdns_instance_name_set: %s", esp_err_to_name(err));
        return false;
    }

    if (register_services() != ESP_OK) return false;

    s_initialised = true;
    ESP_LOGI(TAG, "mDNS up: %s.local (_http._tcp, _bugbuster._tcp)", s_hostname);
    return true;
}

bool mdns_responder_set_hostname(const char* hostname)
{
    if (!s_initialised) return false;

    char next[MDNS_HOSTNAME_MAX] = {};
    if (!hostname || !hostname[0]) {
        derive_default_hostname(next, sizeof(next));
    } else {
        if (!hostname_is_valid(hostname)) {
            ESP_LOGE(TAG, "set_hostname rejected: '%s' fails validation", hostname);
            return false;
        }
        strncpy(next, hostname, sizeof(next) - 1);
    }

    if (strcmp(next, s_hostname) == 0) {
        // No-op but still persist the user's intent (e.g. they explicitly set
        // the derived value to "lock" it). Cheap.
        nvs_save_hostname(hostname);
        return true;
    }

    esp_err_t err = mdns_hostname_set(next);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "mdns_hostname_set: %s", esp_err_to_name(err));
        return false;
    }

    // Re-register services so browsers refresh the SRV/TXT bindings against
    // the new A record.
    unregister_services();
    if (register_services() != ESP_OK) return false;

    strncpy(s_hostname, next, sizeof(s_hostname) - 1);
    s_hostname[sizeof(s_hostname) - 1] = '\0';

    if (!nvs_save_hostname(hostname)) {
        // Live change applied; persistence failed. Caller can surface this.
        ESP_LOGW(TAG, "Hostname '%s' active but NVS save failed", s_hostname);
    }
    ESP_LOGI(TAG, "Hostname now '%s.local'", s_hostname);
    return true;
}

size_t mdns_responder_get_hostname(char* out, size_t out_sz)
{
    if (!s_initialised || !out || out_sz == 0) return 0;
    size_t n = strlen(s_hostname);
    if (n >= out_sz) n = out_sz - 1;
    memcpy(out, s_hostname, n);
    out[n] = '\0';
    return n;
}
