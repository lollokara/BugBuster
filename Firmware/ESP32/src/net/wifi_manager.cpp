// =============================================================================
// wifi_manager.cpp - WiFi AP+STA management (ESP-IDF native)
// =============================================================================

#include "wifi_manager.h"
#include "wifi_connect_seq.h"
#include "config.h"
#include <string.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "freertos/idf_additions.h"  // xTaskCreateWithCaps
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_task_wdt.h"

static const char* TAG = "wifi";
static const char* NVS_NAMESPACE = "wifi_cfg";

// Module-level cached AP password (loaded from NVS at boot; updated by wifi_set_ap_password).
// Using the compile-time default until NVS overrides it.
static char s_ap_pass[65] = WIFI_PASSWORD;

static EventGroupHandle_t s_wifi_event_group = NULL;
#define WIFI_CONNECTED_BIT     BIT0
#define WIFI_RECONNECT_REQ_BIT BIT1

static portMUX_TYPE s_wifi_state_mux = portMUX_INITIALIZER_UNLOCKED;
static bool s_sta_connected = false;
static bool s_connecting    = false;   // true while wifi_connect() is in progress
static char s_sta_ip[20]    = "0.0.0.0";
static char s_ap_ip[20]     = "192.168.4.1";
static char s_ap_mac[20]    = "";
static char s_sta_ssid[64]  = "";

// Reconnect runs on a dedicated worker, NOT on the FreeRTOS Timer Service task.
// `esp_wifi_connect()` on WPA3-SAE pushes elliptic-curve crypto frames onto the
// caller's stack, overflowing Tmr Svc's 2 KB stack and panicking with "stack
// overflow in task Tmr Svc" (observed coredump 2026-05-07, pc=0x4037cec2).
// Worker waits on WIFI_RECONNECT_REQ_BIT, sleeps 2 s, then attempts connect.
static void wifi_reconnect_task(void* arg)
{
    for (;;) {
        EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
                                               WIFI_RECONNECT_REQ_BIT,
                                               pdTRUE,        // clear on exit
                                               pdFALSE,       // wait for any
                                               portMAX_DELAY);
        if ((bits & WIFI_RECONNECT_REQ_BIT) == 0) continue;
        vTaskDelay(pdMS_TO_TICKS(2000));
        if (!s_connecting && s_sta_ssid[0]) {
            ESP_LOGI(TAG, "Reconnect worker: attempting connection...");
            esp_wifi_connect();
        }
    }
}

// ---- NVS helpers ----

// H06: returns true only when all NVS writes and commit succeed.
// Bails before commit if either set fails so partial data is never committed.
static bool nvs_save_sta_credentials(const char* ssid, const char* pass)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open(%s) failed: %s", NVS_NAMESPACE, esp_err_to_name(err));
        return false;
    }
    esp_err_t set_err = nvs_set_str(h, "sta_ssid", ssid);
    if (set_err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_set_str(sta_ssid) failed: %s", esp_err_to_name(set_err));
        nvs_close(h);
        return false;
    }
    set_err = nvs_set_str(h, "sta_pass", pass);
    if (set_err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_set_str(sta_pass) failed: %s", esp_err_to_name(set_err));
        nvs_close(h);
        return false;
    }
    esp_err_t commit_err = nvs_commit(h);
    nvs_close(h);
    if (commit_err == ESP_OK) {
        ESP_LOGI(TAG, "STA credentials saved to NVS");
        return true;
    } else {
        ESP_LOGE(TAG, "nvs_commit failed for STA credentials: %s",
                 esp_err_to_name(commit_err));
        return false;
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

static bool nvs_save_ap_password(const char* pass)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open(%s) failed saving ap_pass: %s", NVS_NAMESPACE, esp_err_to_name(err));
        return false;
    }
    esp_err_t set_err = nvs_set_str(h, "ap_pass", pass);
    if (set_err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_set_str(ap_pass) failed: %s", esp_err_to_name(set_err));
        nvs_close(h);
        return false;
    }
    esp_err_t commit_err = nvs_commit(h);
    nvs_close(h);
    if (commit_err == ESP_OK) {
        ESP_LOGI(TAG, "AP password saved to NVS");
        return true;
    } else {
        ESP_LOGE(TAG, "nvs_commit failed for ap_pass: %s", esp_err_to_name(commit_err));
        return false;
    }
}

static void nvs_load_ap_password(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) return;

    char buf[65] = {};
    size_t len = sizeof(buf);
    esp_err_t err = nvs_get_str(h, "ap_pass", buf, &len);
    nvs_close(h);

    if (err == ESP_OK && len >= 9) {  // len includes null terminator; 9 = 8 chars + '\0'
        strncpy(s_ap_pass, buf, sizeof(s_ap_pass) - 1);
        s_ap_pass[sizeof(s_ap_pass) - 1] = '\0';
        ESP_LOGI(TAG, "Loaded AP password from NVS");
    } else if (err == ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGI(TAG, "No AP password in NVS, using default");
    } else {
        ESP_LOGW(TAG, "AP password in NVS invalid (len=%zu err=%s), using default", len, esp_err_to_name(err));
    }
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
            portENTER_CRITICAL(&s_wifi_state_mux);
            s_sta_connected = false;
            strncpy(s_sta_ip, "0.0.0.0", sizeof(s_sta_ip));
            portEXIT_CRITICAL(&s_wifi_state_mux);
            // Don't auto-retry if wifi_connect() is driving the sequence
            if (!s_connecting && s_sta_ssid[0] && s_wifi_event_group) {
                ESP_LOGI(TAG, "STA disconnected, retrying in 2s...");
                // Defer to wifi_reconnect_task — sleeping 2 s here would block
                // the event task; previously we used a FreeRTOS timer, but its
                // 2 KB Tmr Svc stack overflows during WPA3-SAE crypto.
                xEventGroupSetBits(s_wifi_event_group, WIFI_RECONNECT_REQ_BIT);
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
        portENTER_CRITICAL(&s_wifi_state_mux);
        snprintf(s_sta_ip, sizeof(s_sta_ip), IPSTR, IP2STR(&event->ip_info.ip));
        s_sta_connected = true;
        portEXIT_CRITICAL(&s_wifi_state_mux);
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

    // Seed the cached AP password from the caller arg (compile-time default),
    // then override with NVS value if one was previously saved.
    strncpy(s_ap_pass, ap_pass, sizeof(s_ap_pass) - 1);
    s_ap_pass[sizeof(s_ap_pass) - 1] = '\0';
    nvs_load_ap_password();

    s_wifi_event_group = xEventGroupCreate();
    // Reconnect worker — 4 KB stack accommodates WPA3-SAE EC frames inside
    // esp_wifi_connect() that overflow the 2 KB FreeRTOS Tmr Svc stack.
    // Allocate the stack in PSRAM (xTaskCreateWithCaps) so we don't consume
    // the ~7 KB internal-heap budget that httpd_start needs for its own task
    // stack — putting this 4 KB stack in internal RAM drops the largest
    // contiguous internal block below 4 KB and breaks httpd boot.
    // The worker only runs after wifi disconnects (no flash-cache-disabled
    // path touches it), so PSRAM stack is safe.
    xTaskCreateWithCaps(wifi_reconnect_task, "wifi_rc", 4096, NULL,
                        tskIDLE_PRIORITY + 5, NULL, MALLOC_CAP_SPIRAM);

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

    // AP config — use the cached password (NVS-overridden or compile-time default)
    wifi_config_t ap_config = {};
    strncpy((char*)ap_config.ap.ssid, ap_ssid, sizeof(ap_config.ap.ssid) - 1);
    strncpy((char*)ap_config.ap.password, s_ap_pass, sizeof(ap_config.ap.password) - 1);
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

    // Disable modem-sleep power save. BugBuster is a mains-powered bench
    // instrument, not a battery device -- power save buys nothing here and
    // actively hurts: the default PS mode sends an 802.11 NULL data frame
    // on every awake/asleep transition to signal power-save state to the AP,
    // and each of those is a fresh small allocation out of the same tight
    // internal-DMA-RAM pool the rest of WiFi's buffers share. Under any
    // internal-RAM pressure that allocation can fail, logged by the closed-
    // source WiFi blob as "wifi:m f null" ("mem fail" building a "null"
    // frame) -- confirmed by the string table in libnet80211.a, which lists
    // the same "m f <frame-type>" pattern for beacon/deauth/auth/assoc/probe
    // frames too. Sustained failures here are what starve the link into the
    // esp-tls "select() timeout" seen during GitHub update checks.
    // Persists across the esp_wifi_stop()/esp_wifi_start() cycle used by the
    // STA-credential-probe path below (only esp_wifi_deinit() clears it, and
    // nothing in this firmware calls that), so this one call covers every
    // WiFi (re)start.
    esp_wifi_set_ps(WIFI_PS_NONE);

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

// ---- wifi_connect: esp_wifi bindings for the pure sequence (wifi_connect_seq) ----

static bool s_seq_sta_only = false;

static void op_disconnect(void) { esp_wifi_disconnect(); }
static void op_stop(void)       { esp_wifi_stop(); }
static void op_start(void)      { esp_wifi_start(); }
static void op_delay(uint32_t ms) { vTaskDelay(pdMS_TO_TICKS(ms)); }

static void op_set_mode(bool sta_only)
{
    s_seq_sta_only = sta_only;
    esp_wifi_set_mode(sta_only ? WIFI_MODE_STA : WIFI_MODE_APSTA);
}

static void op_set_sta(const char *ssid, const char *pass)
{
    wifi_config_t c = {};
    strncpy((char*)c.sta.ssid, ssid ? ssid : "", sizeof(c.sta.ssid) - 1);
    strncpy((char*)c.sta.password, pass ? pass : "", sizeof(c.sta.password) - 1);
    if (s_seq_sta_only) {
        c.sta.threshold.authmode = WIFI_AUTH_WPA_WPA2_PSK;
        c.sta.pmf_cfg.capable    = false;
        c.sta.pmf_cfg.required   = false;
    } else {
        c.sta.threshold.authmode = WIFI_AUTH_WPA2_WPA3_PSK;
        c.sta.sae_pwe_h2e        = WPA3_SAE_PWE_BOTH;
        c.sta.pmf_cfg.capable    = true;
        c.sta.pmf_cfg.required   = false;
    }
    esp_wifi_set_config(WIFI_IF_STA, &c);
    strncpy(s_sta_ssid, ssid ? ssid : "", sizeof(s_sta_ssid) - 1);
    s_sta_ssid[sizeof(s_sta_ssid) - 1] = '\0';
}

static void op_get_sta(char *ssid, size_t ssid_sz, char *pass, size_t pass_sz)
{
    wifi_config_t c = {};
    ssid[0] = pass[0] = '\0';
    if (esp_wifi_get_config(WIFI_IF_STA, &c) != ESP_OK) return;
    strncpy(ssid, (const char*)c.sta.ssid, ssid_sz - 1);
    ssid[ssid_sz - 1] = '\0';
    strncpy(pass, (const char*)c.sta.password, pass_sz - 1);
    pass[pass_sz - 1] = '\0';
}

static bool op_try_connect(uint32_t timeout_ms)
{
    portENTER_CRITICAL(&s_wifi_state_mux);
    s_sta_connected = false;
    portEXIT_CRITICAL(&s_wifi_state_mux);
    xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    esp_err_t err = esp_wifi_connect();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_connect() failed: %s", esp_err_to_name(err));
        return false;
    }
    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
        WIFI_CONNECTED_BIT, pdFALSE, pdTRUE, pdMS_TO_TICKS(timeout_ms));
    return (bits & WIFI_CONNECTED_BIT) != 0;
}

static void op_reapply_ap(void)
{
    // Use s_ap_pass so any NVS-saved or runtime-changed password is preserved.
    wifi_config_t ap_config = {};
    strncpy((char*)ap_config.ap.ssid, WIFI_SSID, sizeof(ap_config.ap.ssid) - 1);
    strncpy((char*)ap_config.ap.password, s_ap_pass, sizeof(ap_config.ap.password) - 1);
    ap_config.ap.ssid_len       = strlen(WIFI_SSID);
    ap_config.ap.max_connection = 4;
    ap_config.ap.authmode       = WIFI_AUTH_WPA2_PSK;
    esp_wifi_set_config(WIFI_IF_AP, &ap_config);
}

static bool op_save_creds(const char *ssid, const char *pass)
{
    bool persisted = nvs_save_sta_credentials(ssid, pass ? pass : "");
    ESP_LOGI(TAG, "Connected to '%s' - credentials %s", ssid,
             persisted ? "saved" : "save FAILED (NVS error)");
    return persisted;
}

static const WifiConnectOps s_wifi_ops = {
    op_disconnect, op_stop, op_start, op_set_mode, op_set_sta, op_get_sta,
    op_try_connect, op_reapply_ap, op_save_creds, op_delay,
};

bool wifi_connect(const char* ssid, const char* pass)
{
    s_connecting = true;
    bool ok = wifi_connect_seq(&s_wifi_ops, ssid, pass);
    s_connecting = false;
    if (!ok) ESP_LOGW(TAG, "Failed to connect to '%s'", ssid);
    return ok;
}

bool wifi_is_connected(void) {
    portENTER_CRITICAL(&s_wifi_state_mux);
    bool v = s_sta_connected;
    portEXIT_CRITICAL(&s_wifi_state_mux);
    return v;
}

const char* wifi_get_sta_ip(void) {
    static char s_ip_copy[20];
    portENTER_CRITICAL(&s_wifi_state_mux);
    memcpy(s_ip_copy, s_sta_ip, sizeof(s_ip_copy));
    portEXIT_CRITICAL(&s_wifi_state_mux);
    return s_ip_copy;
}
const char* wifi_get_ap_ip(void)   { return s_ap_ip; }
const char* wifi_get_ap_mac(void)  { return s_ap_mac; }
const char* wifi_get_sta_ssid(void){ return s_sta_ssid; }

bool wifi_forget_credentials(void)
{
    esp_wifi_disconnect();
    memset(s_sta_ssid, 0, sizeof(s_sta_ssid));

    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) return false;
    nvs_erase_key(h, "sta_ssid");
    nvs_erase_key(h, "sta_pass");
    esp_err_t err = nvs_commit(h);
    nvs_close(h);
    ESP_LOGI(TAG, "STA credentials erased from NVS");
    return err == ESP_OK;
}

int wifi_get_rssi(void) {
    wifi_ap_record_t info;
    if (esp_wifi_sta_get_ap_info(&info) == ESP_OK) return info.rssi;
    return 0;
}

bool wifi_set_ap_password(const char* new_pass, bool* persisted_out)
{
    if (persisted_out) *persisted_out = false;
    if (!new_pass) return false;
    size_t plen = strlen(new_pass);
    // WPA2-PSK requires 8–63 printable ASCII characters
    if (plen < 8 || plen > 63) {
        ESP_LOGE(TAG, "wifi_set_ap_password: invalid length %zu (must be 8-63)", plen);
        return false;
    }

    // Update cached value
    strncpy(s_ap_pass, new_pass, sizeof(s_ap_pass) - 1);
    s_ap_pass[sizeof(s_ap_pass) - 1] = '\0';

    // Persist to NVS
    bool persisted = nvs_save_ap_password(s_ap_pass);
    if (persisted_out) *persisted_out = persisted;

    // Apply live — this disconnects current AP clients but avoids a reboot.
    wifi_config_t ap_config = {};
    esp_err_t err = esp_wifi_get_config(WIFI_IF_AP, &ap_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_get_config failed: %s", esp_err_to_name(err));
        return false;
    }
    strncpy((char*)ap_config.ap.password, s_ap_pass, sizeof(ap_config.ap.password) - 1);
    ap_config.ap.password[sizeof(ap_config.ap.password) - 1] = '\0';
    err = esp_wifi_set_config(WIFI_IF_AP, &ap_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_set_config failed: %s", esp_err_to_name(err));
        return false;
    }
    ESP_LOGI(TAG, "AP password applied live (persisted=%s)", persisted ? "yes" : "no");
    return true;
}

int wifi_scan(wifi_scan_result_t* results, int max_results)
{
    wifi_scan_config_t scan_cfg = {};
    scan_cfg.show_hidden = false;

    // Feed the task watchdog before the blocking scan (can take >2 s).
    if (esp_task_wdt_status(NULL) == ESP_OK) esp_task_wdt_reset();
    esp_err_t err = esp_wifi_scan_start(&scan_cfg, true);  // blocking
    // Feed again immediately after the scan completes.
    if (esp_task_wdt_status(NULL) == ESP_OK) esp_task_wdt_reset();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Scan failed: %s", esp_err_to_name(err));
        return 0;
    }

    uint16_t ap_count = 0;
    esp_wifi_scan_get_ap_num(&ap_count);
    if (ap_count == 0) return 0;

    uint16_t fetch = (ap_count > (uint16_t)max_results) ? (uint16_t)max_results : ap_count;
    wifi_ap_record_t* records = (wifi_ap_record_t*)malloc(fetch * sizeof(wifi_ap_record_t));
    if (!records) {
        // Clear the driver's internal AP list even on alloc failure so the
        // next scan starts from a clean slate.
        esp_wifi_clear_ap_list();
        return 0;
    }

    // Snapshot fetch so the loop bound is decoupled from any value the
    // driver writes back into the variable mid-call.
    uint16_t requested = fetch;
    esp_err_t get_err = esp_wifi_scan_get_ap_records(&fetch, records);
    if (get_err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_scan_get_ap_records: %s", esp_err_to_name(get_err));
        free(records);
        esp_wifi_clear_ap_list();
        return 0;
    }
    if (fetch > requested) fetch = requested;  // defensive

    for (int i = 0; i < (int)fetch; i++) {
        strncpy(results[i].ssid, (const char*)records[i].ssid, 32);
        results[i].ssid[32] = '\0';
        results[i].rssi = records[i].rssi;
        results[i].auth = (int)records[i].authmode;
        if (esp_task_wdt_status(NULL) == ESP_OK) esp_task_wdt_reset();
    }

    free(records);
    // get_ap_records normally drains the internal buffer; call clear
    // explicitly to make the cleanup contract obvious.
    esp_wifi_clear_ap_list();
    return (int)fetch;
}
