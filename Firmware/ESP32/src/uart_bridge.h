#pragma once

// =============================================================================
// uart_bridge.h - UART ↔ USB CDC bridge (multi-instance)
//
// Each bridge connects a USB CDC port to an ESP32 UART peripheral.
// Config is persisted in NVS and can be changed via web API.
// =============================================================================

#include <stdint.h>
#include <stdbool.h>
#include "usb_cdc.h"

#ifdef __cplusplus
extern "C" {
#endif

struct UartBridgeConfig {
    uint8_t  uart_num;     // ESP32 UART number (0, 1, or 2)
    int      tx_pin;       // GPIO for TX
    int      rx_pin;       // GPIO for RX
    uint32_t baudrate;     // 300..3000000
    uint8_t  data_bits;    // 5, 6, 7, 8
    uint8_t  parity;       // 0=none, 1=odd, 2=even
    uint8_t  stop_bits;    // 1 or 2
    bool     enabled;      // Bridge active
};

/**
 * @brief Initialize all UART bridges (load config from NVS, install drivers).
 *        Call after usb_cdc_init() and before starting the main loop.
 */
void uart_bridge_init(void);

/**
 * @brief Start the FreeRTOS bridge tasks (one per bridge).
 */
void uart_bridge_start(void);

/**
 * @brief Get current config for a bridge.
 * @param id Bridge index (0..CDC_BRIDGE_COUNT-1)
 */
bool uart_bridge_get_config(int id, UartBridgeConfig *cfg);

/**
 * @brief Apply new config to a bridge (reconfigures UART, saves to NVS).
 * @param id Bridge index (0..CDC_BRIDGE_COUNT-1)
 */
bool uart_bridge_set_config(int id, const UartBridgeConfig *cfg);

/**
 * @brief Check if bridge DTR is active (host terminal open).
 */
bool uart_bridge_is_connected(int id);

/**
 * @brief Get list of GPIO pins available for UART (excludes used pins).
 * @param out_pins Array to fill with available pin numbers
 * @param max_pins Size of out_pins array
 * @return Number of pins written
 */
int uart_bridge_get_available_pins(int *out_pins, int max_pins);

#ifdef __cplusplus
}
#endif
