#pragma once

// =============================================================================
// bb_la_usb.h — Logic Analyzer USB bulk data streaming
//
// Streams captured LA data over USB vendor bulk endpoint (interface 3).
// The host reads data at ~1.2 MB/s (USB Full-Speed bulk).
// =============================================================================

#include <stdint.h>
#include <stdbool.h>

// TinyUSB vendor instance index for LA (0 = CMSIS-DAP, 1 = LA)
#define BB_LA_VENDOR_ITF    1

/**
 * @brief Initialize LA USB streaming.
 */
void bb_la_usb_init(void);

/**
 * @brief Send a chunk of LA data over USB bulk IN endpoint.
 *        Called from bb_la_read_data or a streaming task.
 * @param data  Pointer to data
 * @param len   Length in bytes (max 64 per packet)
 * @return Bytes actually written to USB FIFO
 */
uint32_t bb_la_usb_write(const uint8_t *data, uint32_t len);

/**
 * @brief Check if USB host has connected to the LA interface.
 */
bool bb_la_usb_connected(void);

/**
 * @brief Stream entire LA capture buffer over USB.
 *        Sends a 4-byte header first (total length LE), then data.
 *        Blocks until all data is sent or timeout.
 * @param buf        Capture buffer
 * @param total_bytes  Total bytes to send
 * @return Bytes sent
 */
uint32_t bb_la_usb_stream_buffer(const uint8_t *buf, uint32_t total_bytes);

/**
 * @brief Write raw data over USB bulk IN (no header).
 *        Used for gapless streaming — sends packed samples directly.
 * @param buf        Data buffer
 * @param total_bytes  Total bytes to send
 * @return Bytes sent
 */
uint32_t bb_la_usb_write_raw(const uint8_t *buf, uint32_t total_bytes);

/**
 * @brief Poll the vendor OUT endpoint for commands from the USB host.
 *        Commands: 0x01 = start stream, 0x00 = stop.
 *        Called from the main loop to enable gapless USB streaming
 *        without ESP32 involvement.
 */
void bb_la_usb_poll_commands(void);
