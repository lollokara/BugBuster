// =============================================================================
// wifi_manager.cpp - WiFi AP+STA management (ESP-IDF native)
// =============================================================================

#include "wifi_manager.h"
#include <string.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"

static const char* TAG = "wifi";

static EventGroupHandle_t s_wifi_event_group = NULL;
#define WIFI_CONNECTED_BIT BIT0

static bool s_sta_connected = false;
static char s_sta_ip[20]   = "0.0.0.0";
static char s_ap_ip[20]    = "192.168.4.1";
static char s_ap_mac[20]   = "";
static char s_sta_ssid[64] = "";

static void wifi_event_handler(void* arg, esp_event_base_t event_base,
                                int32_t event_id, void* event_data)
{
    if (event_base == WIFI_EVENT) {
        if (event_id == WIFI_EVENT_STA_START) {
            esp_wifi_connect();
        } else if (event_id == WIFI_EVENT_STA_DISCONNECTED) {
            s_sta_connected = false;
            ESP_LOGI(TAG, "STA disconnected, retrying...");
            esp_wifi_connect();
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

    // STA config
    if (sta_ssid && sta_ssid[0]) {
        wifi_config_t sta_config = {};
        strncpy((char*)sta_config.sta.ssid, sta_ssid, sizeof(sta_config.sta.ssid) - 1);
        strncpy((char*)sta_config.sta.password, sta_pass ? sta_pass : "",
                sizeof(sta_config.sta.password) - 1);
        esp_wifi_set_config(WIFI_IF_STA, &sta_config);
        strncpy(s_sta_ssid, sta_ssid, sizeof(s_sta_ssid) - 1);
    }

    esp_wifi_start();

    // Wait for connection (up to 10 seconds)
    if (sta_ssid && sta_ssid[0]) {
        ESP_LOGI(TAG, "Connecting to '%s'...", sta_ssid);
        EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
            WIFI_CONNECTED_BIT, pdFALSE, pdTRUE, pdMS_TO_TICKS(10000));
        if (!(bits & WIFI_CONNECTED_BIT)) {
            ESP_LOGW(TAG, "STA connection timeout");
        }
    }
}

bool wifi_connect(const char* ssid, const char* pass)
{
    esp_wifi_disconnect();

    wifi_config_t sta_config = {};
    strncpy((char*)sta_config.sta.ssid, ssid, sizeof(sta_config.sta.ssid) - 1);
    strncpy((char*)sta_config.sta.password, pass ? pass : "",
            sizeof(sta_config.sta.password) - 1);
    esp_wifi_set_config(WIFI_IF_STA, &sta_config);
    strncpy(s_sta_ssid, ssid, sizeof(s_sta_ssid) - 1);

    s_sta_connected = false;
    xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    esp_wifi_connect();

    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
        WIFI_CONNECTED_BIT, pdFALSE, pdTRUE, pdMS_TO_TICKS(10000));
    return (bits & WIFI_CONNECTED_BIT) != 0;
}

bool wifi_is_connected(void)     { return s_sta_connected; }
const char* wifi_get_sta_ip(void)  { return s_sta_ip; }
const char* wifi_get_ap_ip(void)   { return s_ap_ip; }
const char* wifi_get_ap_mac(void)  { return s_ap_mac; }
const char* wifi_get_sta_ssid(void){ return s_sta_ssid; }

int wifi_get_rssi(void) {
    wifi_ap_record_t info;
    if (esp_wifi_sta_get_ap_info(&info) == ESP_OK) return info.rssi;
    return 0;
}
