#pragma once

// =============================================================================
// ble_service.h — NimBLE peripheral for the low-rate iOS control plane.
//
// Exposes a single custom GATT service so a phone can:
//   - read device identity (model, MAC, firmware/proto version)   [Phase 1]
//   - authenticate with the admin token (same token as HTTP)      [Phase 1]
//   - provision WiFi, set supplies, read sensors                  [later phases]
//
// This is NOT a streaming transport. Payloads are small; the stack is kept
// lean (NimBLE, single connection, peripheral-only) to protect the scarce
// internal DMA SRAM the WiFi/HTTPD/USB workload already pressures.
//
// Lifecycle: call ble_service_init() once during app_main, after auth_init()
// and wifi_init() (it reads the admin token and the station MAC). Safe to call
// before WiFi has an IP — BLE is independent of the LAN.
// =============================================================================

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Bring up the NimBLE host, register the BugBuster GATT service, and
 *        start connectable advertising as "BugBuster-XXYYZZ".
 *
 * @return true if the NimBLE port initialised and the host task started;
 *         false on failure (BLE then stays off — the rest of the firmware is
 *         unaffected).
 */
bool ble_service_init(void);

/**
 * @brief Whether a central is currently connected over BLE.
 */
bool ble_service_is_connected(void);

/**
 * @brief Whether the currently connected central has presented a valid admin
 *        token via the Auth characteristic. Always false when disconnected.
 *        Control-write characteristics (added in later phases) must gate on this.
 */
bool ble_service_is_authenticated(void);

#ifdef __cplusplus
}
#endif
