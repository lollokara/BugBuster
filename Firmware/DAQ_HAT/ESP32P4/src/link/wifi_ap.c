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

// Per-step init guards. One aggregate s_inited flag was not enough: any step
// failing mid-sequence left s_inited false, so a retry replayed the earlier,
// NON-IDEMPOTENT steps. esp_netif_create_default_wifi_ap() replayed that way
// trips ESP-IDF's duplicate-key assert and hard-aborts the board -- which is
// why s_ap_netif already had its own guard. Every step gets one now.
static bool s_hosted_ok;    // esp_hosted_init()
static bool s_netif_ok;     // esp_netif_init() + default event loop
static bool s_wifi_ok;      // esp_wifi_init()
static bool s_handler_ok;   // esp_event_handler_instance_register()
static bool s_inited;       // all of the above complete
static bool s_up;           // softAP actually started
static esp_netif_t *s_ap_netif;

// Stations currently associated to the softAP. Maintained from the
// AP_STACONNECTED/AP_STADISCONNECTED events below, which were previously only
// logged and discarded.
//
// This is the liveness signal the idle-teardown timer in daq_board.c needs. It
// used to count only tcp_backend_connected(), so a phone that had associated
// but not yet finished DHCP / opened the socket was indistinguishable from an
// empty room -- and the AP got torn down out from under a client that was
// actively joining. Association happens well before the socket, so it is the
// earliest honest evidence that somebody is there.
//
// Single-writer: only the WiFi event handler task assigns this.
static volatile uint32_t s_sta_count;

static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    if (base != WIFI_EVENT) return;
    switch (id) {
    case WIFI_EVENT_AP_STACONNECTED: {
        wifi_event_ap_staconnected_t *e = (wifi_event_ap_staconnected_t *)data;
        s_sta_count++;
        ESP_LOGI(TAG, "station " MACSTR " joined, aid=%d (%u associated)",
                 MAC2STR(e->mac), e->aid, (unsigned)s_sta_count);
        break;
    }
    case WIFI_EVENT_AP_STADISCONNECTED: {
        wifi_event_ap_stadisconnected_t *e = (wifi_event_ap_stadisconnected_t *)data;
        // Saturate at 0: a DISCONNECTED without a matching CONNECTED (possible
        // across an esp_wifi_stop()/start() cycle) must not underflow this to
        // ~4 billion, which would pin the AP "alive" forever and defeat the
        // idle teardown entirely.
        if (s_sta_count > 0) s_sta_count--;
        ESP_LOGI(TAG, "station " MACSTR " left, aid=%d (%u associated)",
                 MAC2STR(e->mac), e->aid, (unsigned)s_sta_count);
        break;
    }
    default:
        break;
    }
}

esp_err_t wifi_ap_init(void)
{
    if (s_inited) return ESP_OK;

    if (!s_hosted_ok) {
        // The P4 has no native WiFi radio -- the wifi driver init step below
        // routes through ESP-Hosted's "remote" wifi component over the SDIO
        // link to the C6. That component requires the host-side hosted
        // transport brought up first via esp_hosted_init(); skipping this
        // makes esp_wifi_remote_init() fail with "Transport not initialized"
        // / "ESP-Hosted link not yet up".
        int herr = esp_hosted_init();
        if (herr != 0) {
            ESP_LOGE(TAG, "esp_hosted_init failed: %d", herr);
            return ESP_FAIL;
        }
        s_hosted_ok = true;
    }

    if (!s_netif_ok) {
        esp_err_t nerr = esp_netif_init();
        if (nerr != ESP_OK && nerr != ESP_ERR_INVALID_STATE) return nerr;
        nerr = esp_event_loop_create_default();
        if (nerr != ESP_OK && nerr != ESP_ERR_INVALID_STATE) return nerr;
        s_netif_ok = true;
    }

    // Guarded independently of s_inited: the wifi driver init step below can
    // fail (e.g. the ESP-Hosted SDIO link to the C6 isn't up yet) after this
    // already succeeded, leaving s_inited false but the netif still
    // registered under its fixed "WIFI_AP_DEF" key. A retry that
    // unconditionally called esp_netif_create_default_wifi_ap() again would
    // hit ESP-IDF's duplicate-key assert and hard-abort the whole board --
    // so only create it once, ever, and reuse it across retries.
    if (!s_ap_netif) {
        s_ap_netif = esp_netif_create_default_wifi_ap();
        if (!s_ap_netif) return ESP_FAIL;
    }

    if (!s_wifi_ok) {
        wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
        esp_err_t werr = esp_wifi_init(&cfg);
        // The C6's ESP-Hosted stack may not be listening yet; the caller
        // retries. Everything above stays done, so the retry resumes here.
        if (werr != ESP_OK) return werr;
        s_wifi_ok = true;
    }

    if (!s_handler_ok) {
        esp_err_t herr2 = esp_event_handler_instance_register(
            WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL);
        if (herr2 != ESP_OK) return herr2;
        s_handler_ok = true;
    }

    s_inited = true;
    ESP_LOGI(TAG, "WiFi (ESP-Hosted/C6) initialised");
    return ESP_OK;
}

esp_err_t wifi_ap_start(const char *ssid, const char *password)
{
    esp_err_t err = wifi_ap_init();
    if (err != ESP_OK) return err;
    // Verify against the driver rather than trusting the cached flag: s_up
    // can be stale in either direction after a partial failure, and a false
    // "already up" is the wedge described in wifi_ap_stop(). Only skip the
    // start sequence when the driver itself confirms AP mode.
    if (s_up) {
        wifi_mode_t mode = WIFI_MODE_NULL;
        if (esp_wifi_get_mode(&mode) == ESP_OK &&
            (mode == WIFI_MODE_AP || mode == WIFI_MODE_APSTA)) {
            return ESP_OK;
        }
        ESP_LOGW(TAG, "s_up set but driver reports mode=%d; restarting AP", (int)mode);
        s_up = false;
    }

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
            // PMF *required* on a plain WPA2-PSK (non-WPA3-transition) AP is
            // a known cause of "associates then immediately deauths" on some
            // real clients (observed on the bench: iPhone joins then drops
            // within ~400ms, iPad never completes the join at all) -- capable
            // but not required is the standard safe softAP setting.
            .pmf_cfg        = { .capable = true, .required = false },
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
    if (err == ESP_ERR_WIFI_NOT_STOPPED) {
        // The driver still considers a previous session running (e.g. a stop
        // that errored out). Stop it for real, then retry once -- this is a
        // recoverable condition, not a reason to fail the whole bring-up.
        ESP_LOGW(TAG, "esp_wifi_start: not stopped; stopping and retrying");
        esp_wifi_stop();
        err = esp_wifi_start();
    }
    if (err != ESP_OK) return err;

    s_up = true;
    ESP_LOGI(TAG, "softAP up: ssid=\"%s\" channel=%d", ssid, wifi_cfg.ap.channel);
    return ESP_OK;
}

esp_err_t wifi_ap_stop(void)
{
    if (!s_up) return ESP_OK;
    // s_up is cleared UNCONDITIONALLY, even when esp_wifi_stop() reports an
    // error. The old code returned early on failure and left s_up = true,
    // after which wifi_ap_start()'s `if (s_up) return ESP_OK` short-circuit
    // reported success for an AP that was never started -- a softAP wedged
    // until the board was power-cycled, which is exactly the field symptom
    // this fix targets. A stale "up" flag is strictly more dangerous than a
    // stale "down" one: "down" merely causes a redundant start attempt,
    // which esp_wifi_start() handles.
    esp_err_t err = esp_wifi_stop();
    s_up = false;
    // Every station is gone by definition once the AP is down. Cleared
    // unconditionally for the same reason as s_up: a stale non-zero count
    // would keep the next session's idle timer permanently reset.
    s_sta_count = 0;
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "esp_wifi_stop failed: %s (state cleared anyway)",
                 esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "softAP down");
    }
    return ESP_OK;
}

bool wifi_ap_is_up(void)
{
    return s_up;
}

uint32_t wifi_ap_sta_count(void)
{
    return s_sta_count;
}
