#pragma once

// =============================================================================
// wifi_manager.h - WiFi AP+STA management (ESP-IDF native)
// =============================================================================

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize WiFi in AP+STA mode.
 * @param ap_ssid     Access point SSID
 * @param ap_pass     Access point password
 * @param sta_ssid    Station SSID (NULL to skip STA)
 * @param sta_pass    Station password
 */
void wifi_init(const char* ap_ssid, const char* ap_pass,
               const char* sta_ssid, const char* sta_pass);

/**
 * @brief Connect to a new WiFi network (STA mode).
 * @param ssid  Network SSID
 * @param pass  Network password
 * @return true if connected successfully
 */
bool wifi_connect(const char* ssid, const char* pass);

/** @brief Check if STA is connected. */
bool wifi_is_connected(void);

/** @brief Get STA IP address string (static buffer). */
const char* wifi_get_sta_ip(void);

/** @brief Get AP IP address string (static buffer). */
const char* wifi_get_ap_ip(void);

/** @brief Get AP MAC address string (static buffer). */
const char* wifi_get_ap_mac(void);

/** @brief Get STA SSID (static buffer). */
const char* wifi_get_sta_ssid(void);

/** @brief Get STA RSSI in dBm. */
int wifi_get_rssi(void);

/** Scan result entry. */
typedef struct {
    char ssid[33];
    int  rssi;
    int  auth;  // wifi_auth_mode_t
} wifi_scan_result_t;

/**
 * @brief Scan for WiFi networks (blocking, ~3s).
 * @param results   Output array
 * @param max_results  Max entries to return
 * @return Number of networks found (capped to max_results)
 */
int wifi_scan(wifi_scan_result_t* results, int max_results);

#ifdef __cplusplus
}
#endif
