#pragma once

// =============================================================================
// wifi_ap.h — P4 softAP bring-up over ESP-Hosted (C6 as WiFi radio, SDIO link).
//
// The ESP32-P4 has no radio of its own; espressif/esp_wifi_remote +
// espressif/esp_hosted (see idf_component.yml) make the standard esp_wifi.h
// API work transparently over the SDIO link to the on-module C6, which acts
// as a pure WiFi co-processor while in this mode. This module owns the one-
// time netif/event-loop init and the softAP start/stop pair the iOS DAQ
// streaming path hangs off of.
//
// NOTE: the SDIO bus (C6_SDIO_* pins, config.h) is also used by c6_flasher.c
// (raw sdmmc_host_init/sdmmc_card_init) to flash the C6, and by `c6diag sdio`
// for bench probing. Those and wifi_ap_start() are mutually exclusive users
// of the same SDMMC host/slot 1 hardware -- never call wifi_ap_start() while
// a C6 flash/probe is in progress, and vice versa. See
// .mex/patterns/daq-hat-ios-wifi-streaming.md.
// =============================================================================

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// One-time init: esp_netif + default event loop + the AP netif object. Safe
// to call once at boot; wifi_ap_start()/_stop() can be called repeatedly
// afterwards without re-calling this.
esp_err_t wifi_ap_init(void);

// Bring up the softAP (ssid/pass) over ESP-Hosted/SDIO. Call
// ddp_master_set_wifi_stream_mode(true) around this at the call site so the
// C6 shows its static streaming screen while the radio is in use.
esp_err_t wifi_ap_start(const char *ssid, const char *password);

// Tear down the softAP.
esp_err_t wifi_ap_stop(void);

bool wifi_ap_is_up(void);

// Number of stations currently associated to the softAP.
//
// This is the idle-teardown timer's early liveness signal: a client associates
// well before it completes DHCP and opens the TCP stream socket, so counting
// only TCP connections made an actively-joining phone look identical to an
// empty room -- and the AP was torn down mid-join. Returns 0 whenever the AP
// is down.
uint32_t wifi_ap_sta_count(void);

#ifdef __cplusplus
}
#endif
