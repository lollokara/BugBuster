#pragma once

// =============================================================================
// usb_cdc.h - TinyUSB CDC composite device (CLI + UART bridges)
//
// CDC #0: CLI console
// CDC #1..N: UART bridge ports (N = CDC_BRIDGE_COUNT)
// =============================================================================

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Number of UART bridge CDC ports (bump to 2 for second bridge)
#define CDC_BRIDGE_COUNT  1

// Total CDC interfaces = 1 (CLI) + bridges
#define CDC_TOTAL_COUNT   (1 + CDC_BRIDGE_COUNT)

/**
 * @brief Initialize TinyUSB with dual CDC descriptors.
 *        Must be called before serial_init() or any CDC access.
 */
void usb_cdc_init(void);

// --- CLI port (CDC #0) ---
bool     usb_cdc_cli_connected(void);
uint32_t usb_cdc_cli_available(void);
uint32_t usb_cdc_cli_read(uint8_t *buf, size_t len);
uint32_t usb_cdc_cli_write(const uint8_t *buf, size_t len);
void     usb_cdc_cli_flush(void);

// --- Bridge ports (CDC #1..N) ---
// bridge_id: 0-based index (0 = CDC #1, 1 = CDC #2, etc.)
bool     usb_cdc_bridge_connected(int bridge_id);
uint32_t usb_cdc_bridge_available(int bridge_id);
uint32_t usb_cdc_bridge_read(int bridge_id, uint8_t *buf, size_t len);
uint32_t usb_cdc_bridge_write(int bridge_id, const uint8_t *buf, size_t len);
void     usb_cdc_bridge_flush(int bridge_id);
bool     usb_cdc_bridge_dtr(int bridge_id);

// Line coding (baud/parity) for bridge port - set by host terminal app
typedef struct {
    uint32_t baudrate;
    uint8_t  data_bits;  // 5,6,7,8
    uint8_t  parity;     // 0=none, 1=odd, 2=even
    uint8_t  stop_bits;  // 0=1bit, 1=1.5bit, 2=2bit
} usb_cdc_line_coding_t;

bool usb_cdc_bridge_get_line_coding(int bridge_id, usb_cdc_line_coding_t *coding);

#ifdef __cplusplus
}
#endif
