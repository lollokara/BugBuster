// =============================================================================
// wifi_ap.c — P4 softAP over ESP-Hosted (see wifi_ap.h).
// =============================================================================

#include "wifi_ap.h"

#include <string.h>
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_wifi.h"
#include "esp_mac.h"   // MACSTR / MAC2STR
#include "esp_hosted.h"

static const char *TAG = "wifi_ap";

static bool s_inited;
static bool s_up;
static esp_netif_t *s_ap_netif;

static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    if (base != WIFI_EVENT) return;
    switch (id) {
    case WIFI_EVENT_AP_STACONNECTED: {
        wifi_event_ap_staconnected_t *e = (wifi_event_ap_staconnected_t *)data;
        ESP_LOGI(TAG, "station " MACSTR " joined, aid=%d", MAC2STR(e->mac), e->aid);
        break;
    }
    case WIFI_EVENT_AP_STADISCONNECTED: {
        wifi_event_ap_stadisconnected_t *e = (wifi_event_ap_stadisconnected_t *)data;
        ESP_LOGI(TAG, "station " MACSTR " left, aid=%d", MAC2STR(e->mac), e->aid);
        break;
    }
    default:
        break;
    }
}

esp_err_t wifi_ap_init(void)
{
    if (s_inited) return ESP_OK;

    // The P4 has no native WiFi radio -- esp_wifi_init() below routes through
    // ESP-Hosted's "remote" wifi component over the SDIO link to the C6. That
    // component requires the host-side hosted transport brought up first via
    // esp_hosted_init(); skipping this makes esp_wifi_remote_init() fail with
    // "Transport not initialized" / "ESP-Hosted link not yet up".
    int herr = esp_hosted_init();
    if (herr != 0) {
        ESP_LOGE(TAG, "esp_hosted_init failed: %d", herr);
        return ESP_FAIL;
    }

    esp_err_t err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return err;

    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return err;

    s_ap_netif = esp_netif_create_default_wifi_ap();
    if (!s_ap_netif) return ESP_FAIL;

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&cfg);
    if (err != ESP_OK) return err;

    err = esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                              &wifi_event_handler, NULL, NULL);
    if (err != ESP_OK) return err;

    s_inited = true;
    ESP_LOGI(TAG, "WiFi (ESP-Hosted/C6) initialised");
    return ESP_OK;
}

esp_err_t wifi_ap_start(const char *ssid, const char *password)
{
    esp_err_t err = wifi_ap_init();
    if (err != ESP_OK) return err;
    if (s_up) return ESP_OK;

    size_t ssid_len = strlen(ssid);
    size_t pass_len = strlen(password);
    if (ssid_len == 0 || ssid_len > sizeof(((wifi_config_t *)0)->ap.ssid))
        return ESP_ERR_INVALID_ARG;
    // WPA2-PSK requires an 8+ char password; 0 means "open" (not offered here
    // -- the DAQ stream carries live measurement data, always authenticate).
    if (pass_len < 8 || pass_len > sizeof(((wifi_config_t *)0)->ap.password))
        return ESP_ERR_INVALID_ARG;

    wifi_config_t wifi_cfg = {
        .ap = {
            .channel        = 6,
            .max_connection = 2,
            .authmode       = WIFI_AUTH_WPA2_PSK,
            .pmf_cfg        = { .required = true },
        },
    };
    memcpy(wifi_cfg.ap.ssid, ssid, ssid_len);
    wifi_cfg.ap.ssid_len = (uint8_t)ssid_len;
    memcpy(wifi_cfg.ap.password, password, pass_len);

    err = esp_wifi_set_mode(WIFI_MODE_AP);
    if (err != ESP_OK) return err;
    err = esp_wifi_set_config(WIFI_IF_AP, &wifi_cfg);
    if (err != ESP_OK) return err;
    err = esp_wifi_start();
    if (err != ESP_OK) return err;

    s_up = true;
    ESP_LOGI(TAG, "softAP up: ssid=\"%s\" channel=%d", ssid, wifi_cfg.ap.channel);
    return ESP_OK;
}

esp_err_t wifi_ap_stop(void)
{
    if (!s_up) return ESP_OK;
    esp_err_t err = esp_wifi_stop();
    if (err != ESP_OK) return err;
    s_up = false;
    ESP_LOGI(TAG, "softAP down");
    return ESP_OK;
}

bool wifi_ap_is_up(void)
{
    return s_up;
}
