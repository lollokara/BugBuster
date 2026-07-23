#pragma once

// =============================================================================
// daq_wifi_ident.h — generates the P4's own softAP SSID + password for the
// BLE-driven DAQ WiFi streaming bring-up (see .mex/patterns/
// daq-hat-ios-wifi-streaming.md). Called fresh on every
// HATP_CMD_DAQ_WIFI_STREAM_START; never persisted.
// =============================================================================

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Generate a fresh SSID + WPA2 password for this P4's softAP.
 *
 * SSID: "BugBusterDAQ-XXYYZZ" from the last 3 bytes of the P4's own WiFi
 * station MAC (mirrors the S3 mainboard's own "BugBuster-XXYYZZ" device-name
 * convention, ble_service.cpp, just a distinct prefix).
 *
 * Password: 8 random alphanumeric characters (esp_fill_random()), drawn from
 * a charset excluding visually-ambiguous characters. Regenerated on every
 * call -- never cached/persisted. 8 chars sits exactly at wifi_ap_start()'s
 * WPA2-PSK minimum length.
 *
 * @param ssid      output buffer, >= 33 bytes.
 * @param ssid_len  size of @p ssid.
 * @param password  output buffer, >= 65 bytes.
 * @param pass_len  size of @p password.
 */
void daq_wifi_ident_generate(char *ssid, size_t ssid_len, char *password, size_t pass_len);

#ifdef __cplusplus
}
#endif
