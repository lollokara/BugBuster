// =============================================================================
// wifi_manager.cpp - WiFi AP+STA management (ESP-IDF native)
// =============================================================================

#include "wifi_manager.h"
#include "config.h"
#include <string.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "freertos/timers.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"

static const char* TAG = "wifi";
static const char* NVS_NAMESPACE = "wifi_cfg";

static EventGroupHandle_t s_wifi_event_group = NULL;
#define WIFI_CONNECTED_BIT BIT0

static bool s_sta_connected = false;
static bool s_connecting    = false;   // true while wifi_connect() is in progress
static TimerHandle_t s_reconnect_timer = NULL;
static char s_sta_ip[20]    = "0.0.0.0";
static char s_ap_ip[20]     = "192.168.4.1";
static char s_ap_mac[20]    = "";
static char s_sta_ssid[64]  = "";

static void reconnect_timer_cb(TimerHandle_t xTimer)
{
    if (!s_connecting && s_sta_ssid[0]) {
        ESP_LOGI(TAG, "Reconnect timer fired, attempting connection...");
        esp_wifi_connect();
    }
}

// ---- NVS helpers ----

static void nvs_save_sta_credentials(const char* ssid, const char* pass)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_str(h, "sta_ssid", ssid);
        nvs_set_str(h, "sta_pass", pass);
        nvs_commit(h);
        nvs_close(h);
        ESP_LOGI(TAG, "STA credentials saved to NVS");
    }
}

static bool nvs_load_sta_credentials(char* ssid, size_t ssid_sz,
                                      char* pass, size_t pass_sz)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) return false;

    size_t len = ssid_sz;
    bool ok = (nvs_get_str(h, "sta_ssid", ssid, &len) == ESP_OK && len > 1);
    if (ok) {
        len = pass_sz;
        if (nvs_get_str(h, "sta_pass", pass, &len) != ESP_OK) pass[0] = '\0';
    }
    nvs_close(h);
    return ok;
}

// ---- Event handler ----

static void wifi_event_handler(void* arg, esp_event_base_t event_base,
                                int32_t event_id, void* event_data)
{
    if (event_base == WIFI_EVENT) {
        if (event_id == WIFI_EVENT_STA_START) {
            // Only auto-connect if we have an SSID configured and not mid-connect
            if (!s_connecting && s_sta_ssid[0]) {
                esp_wifi_connect();
            }
        } else if (event_id == WIFI_EVENT_STA_DISCONNECTED) {
            s_sta_connected = false;
            strncpy(s_sta_ip, "0.0.0.0", sizeof(s_sta_ip));
            // Don't auto-retry if wifi_connect() is driving the sequence
            if (!s_connecting && s_sta_ssid[0] && s_reconnect_timer) {
                ESP_LOGI(TAG, "STA disconnected, retrying in 2s...");
                xTimerStart(s_reconnect_timer, 0);  // non-blocking, fires after 2s
            }
        } else if (event_id == WIFI_EVENT_AP_START) {
            esp_netif_ip_info_t ip_info;
            esp_netif_t* ap_netif = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
            if (ap_netif && esp_netif_get_ip_info(ap_netif, &ip_info) == ESP_OK) {
                snprintf(s_ap_ip, sizeof(s_ap_ip), IPSTR, IP2STR(&ip_info.ip));
            }
            uint8_t mac[6];
            esp_wifi_get_mac(WIFI_IF_AP, mac);
            snprintf(s_ap_mac, sizeof(s_ap_mac), "%02X:%02X:%02X:%02X:%02X:%02X",
                     mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*)event_data;
        snprintf(s_sta_ip, sizeof(s_sta_ip), IPSTR, IP2STR(&event->ip_info.ip));
        s_sta_connected = true;
        ESP_LOGI(TAG, "STA connected, IP: %s", s_sta_ip);
        if (s_wifi_event_group) {
            xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
        }
    }
}

void wifi_init(const char* ap_ssid, const char* ap_pass,
               const char* sta_ssid, const char* sta_pass)
{
    // NVS (required by WiFi)
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }

    s_wifi_event_group = xEventGroupCreate();
    s_reconnect_timer = xTimerCreate("wifi_rc", pdMS_TO_TICKS(2000), pdFALSE, NULL, reconnect_timer_cb);

    // Network interface
    esp_netif_init();
    esp_event_loop_create_default();
    esp_netif_create_default_wifi_ap();
    esp_netif_create_default_wifi_sta();

    // WiFi driver
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&cfg);

    // Event handlers
    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL);

    esp_wifi_set_mode(WIFI_MODE_APSTA);

    // AP config
    wifi_config_t ap_config = {};
    strncpy((char*)ap_config.ap.ssid, ap_ssid, sizeof(ap_config.ap.ssid) - 1);
    strncpy((char*)ap_config.ap.password, ap_pass, sizeof(ap_config.ap.password) - 1);
    ap_config.ap.ssid_len       = strlen(ap_ssid);
    ap_config.ap.channel        = 1;
    ap_config.ap.max_connection = 4;
    ap_config.ap.authmode       = WIFI_AUTH_WPA2_PSK;
    esp_wifi_set_config(WIFI_IF_AP, &ap_config);

    // Determine STA credentials: NVS first, then compile-time defaults
    char nvs_ssid[64] = {};
    char nvs_pass[65] = {};
    const char* use_ssid = sta_ssid;
    const char* use_pass = sta_pass;

    if (nvs_load_sta_credentials(nvs_ssid, sizeof(nvs_ssid), nvs_pass, sizeof(nvs_pass))) {
        ESP_LOGI(TAG, "Loaded STA credentials from NVS: '%s'", nvs_ssid);
        use_ssid = nvs_ssid;
        use_pass = nvs_pass;
    }

    // STA config
    if (use_ssid && use_ssid[0]) {
        wifi_config_t sta_config = {};
        strncpy((char*)sta_config.sta.ssid, use_ssid, sizeof(sta_config.sta.ssid) - 1);
        strncpy((char*)sta_config.sta.password, use_pass ? use_pass : "",
                sizeof(sta_config.sta.password) - 1);
        sta_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
        sta_config.sta.pmf_cfg.capable    = true;
        sta_config.sta.pmf_cfg.required   = false;
        esp_wifi_set_config(WIFI_IF_STA, &sta_config);
        strncpy(s_sta_ssid, use_ssid, sizeof(s_sta_ssid) - 1);
    }

    esp_wifi_start();

    // Wait for connection (up to 10 seconds)
    if (use_ssid && use_ssid[0]) {
        ESP_LOGI(TAG, "Connecting to '%s'...", use_ssid);
        EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
            WIFI_CONNECTED_BIT, pdFALSE, pdTRUE, pdMS_TO_TICKS(10000));
        if (!(bits & WIFI_CONNECTED_BIT)) {
            ESP_LOGW(TAG, "STA connection timeout");
        }
    }
}

bool wifi_connect(const char* ssid, const char* pass)
{
    s_connecting = true;

    // Full stop/restart for a clean connection
    esp_wifi_disconnect();
    esp_wifi_stop();
    vTaskDelay(pdMS_TO_TICKS(200));

    // STA-only mode for connection (avoids AP channel conflicts)
    esp_wifi_set_mode(WIFI_MODE_STA);

    wifi_config_t sta_config = {};
    strncpy((char*)sta_config.sta.ssid, ssid, sizeof(sta_config.sta.ssid) - 1);
    strncpy((char*)sta_config.sta.password, pass ? pass : "",
            sizeof(sta_config.sta.password) - 1);
    sta_config.sta.threshold.authmode = WIFI_AUTH_WPA_WPA2_PSK;
    sta_config.sta.pmf_cfg.capable    = false;
    sta_config.sta.pmf_cfg.required   = false;
    esp_wifi_set_config(WIFI_IF_STA, &sta_config);
    strncpy(s_sta_ssid, ssid, sizeof(s_sta_ssid) - 1);

    s_sta_connected = false;
    xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT);

    esp_wifi_start();
    vTaskDelay(pdMS_TO_TICKS(200));

    // Retry up to 5 times with increasing delay
    bool connected = false;
    for (int attempt = 0; attempt < 5 && !connected; attempt++) {
        if (attempt > 0) {
            esp_wifi_disconnect();
            vTaskDelay(pdMS_TO_TICKS(500 + attempt * 500));
            ESP_LOGI(TAG, "Retry %d/5...", attempt + 1);
        }
        xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT);

        esp_err_t err = esp_wifi_connect();
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "esp_wifi_connect() failed: %s", esp_err_to_name(err));
            continue;
        }

        EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
            WIFI_CONNECTED_BIT, pdFALSE, pdTRUE, pdMS_TO_TICKS(10000));
        connected = (bits & WIFI_CONNECTED_BIT) != 0;
    }

    s_connecting = false;

    // Restore AP+STA mode
    if (!connected) esp_wifi_disconnect();
    esp_wifi_stop();
    vTaskDelay(pdMS_TO_TICKS(100));
    esp_wifi_set_mode(WIFI_MODE_APSTA);

    // Re-apply AP config
    {
        wifi_config_t ap_config = {};
        strncpy((char*)ap_config.ap.ssid, WIFI_SSID, sizeof(ap_config.ap.ssid) - 1);
        strncpy((char*)ap_config.ap.password, WIFI_PASSWORD, sizeof(ap_config.ap.password) - 1);
        ap_config.ap.ssid_len       = strlen(WIFI_SSID);
        ap_config.ap.max_connection = 4;
        ap_config.ap.authmode       = WIFI_AUTH_WPA2_PSK;
        esp_wifi_set_config(WIFI_IF_AP, &ap_config);
    }
    // Re-apply STA config (keep the network we just connected to)
    {
        wifi_config_t sta_cfg = {};
        strncpy((char*)sta_cfg.sta.ssid, ssid, sizeof(sta_cfg.sta.ssid) - 1);
        strncpy((char*)sta_cfg.sta.password, pass ? pass : "",
                sizeof(sta_cfg.sta.password) - 1);
        sta_cfg.sta.threshold.authmode = WIFI_AUTH_WPA2_WPA3_PSK;
        sta_cfg.sta.sae_pwe_h2e       = WPA3_SAE_PWE_BOTH;
        sta_cfg.sta.pmf_cfg.capable    = true;
        sta_cfg.sta.pmf_cfg.required   = false;
        esp_wifi_set_config(WIFI_IF_STA, &sta_cfg);
    }
    esp_wifi_start();

    // If we were connected in STA-only mode, reconnect in APSTA mode
    if (connected) {
        xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
        esp_wifi_connect();
        EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
            WIFI_CONNECTED_BIT, pdFALSE, pdTRUE, pdMS_TO_TICKS(10000));
        connected = (bits & WIFI_CONNECTED_BIT) != 0;
    }

    bool ok = connected;
    if (ok) {
        // Save credentials to NVS on success
        nvs_save_sta_credentials(ssid, pass ? pass : "");
        ESP_LOGI(TAG, "Connected to '%s' — credentials saved", ssid);
    } else {
        ESP_LOGW(TAG, "Failed to connect to '%s'", ssid);
    }
    return ok;
}

bool wifi_is_connected(void)      { return s_sta_connected; }
const char* wifi_get_sta_ip(void)  { return s_sta_ip; }
const char* wifi_get_ap_ip(void)   { return s_ap_ip; }
const char* wifi_get_ap_mac(void)  { return s_ap_mac; }
const char* wifi_get_sta_ssid(void){ return s_sta_ssid; }

int wifi_get_rssi(void) {
    wifi_ap_record_t info;
    if (esp_wifi_sta_get_ap_info(&info) == ESP_OK) return info.rssi;
    return 0;
}

int wifi_scan(wifi_scan_result_t* results, int max_results)
{
    wifi_scan_config_t scan_cfg = {};
    scan_cfg.show_hidden = false;

    esp_err_t err = esp_wifi_scan_start(&scan_cfg, true);  // blocking
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Scan failed: %s", esp_err_to_name(err));
        return 0;
    }

    uint16_t ap_count = 0;
    esp_wifi_scan_get_ap_num(&ap_count);
    if (ap_count == 0) return 0;

    uint16_t fetch = (ap_count > (uint16_t)max_results) ? (uint16_t)max_results : ap_count;
    wifi_ap_record_t* records = (wifi_ap_record_t*)malloc(fetch * sizeof(wifi_ap_record_t));
    if (!records) return 0;

    esp_wifi_scan_get_ap_records(&fetch, records);

    for (int i = 0; i < (int)fetch; i++) {
        strncpy(results[i].ssid, (const char*)records[i].ssid, 32);
        results[i].ssid[32] = '\0';
        results[i].rssi = records[i].rssi;
        results[i].auth = (int)records[i].authmode;
    }

    free(records);
    return (int)fetch;
}
