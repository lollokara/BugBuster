#pragma once

// =============================================================================
// bb_la_usb.h — Logic Analyzer USB transport helpers
//
// Gapless streaming uses CDC because the original vendor-bulk stream path was
// unreliable in practice. The vendor interface is still used for bulk capture
// readout after a completed capture.
// =============================================================================

#include <stdint.h>
#include <stdbool.h>

// TinyUSB vendor instance index for LA in the built-in vendor class
// driver's namespace. BB_LA is the ONLY interface managed by the
// built-in driver — CMSIS-DAP is claimed by the custom DAP class driver
// in lib/debugprobe/src/tusb_edpt_handler.c, so it does NOT count as a
// built-in vendor instance. BB_LA is therefore instance 0.
//
// Prior to 2026-04-09 this was 1, but the subclass-patch in
// bb_usb_descriptors.c now correctly lets the custom DAP driver claim
// CMSIS-DAP only, leaving BB_LA as the built-in driver's sole tenant.
#define BB_LA_VENDOR_ITF    0

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
 * @brief Write raw data to the CDC streaming path (no header).
 *        Used for gapless streaming — sends packed samples directly.
 * @param buf        Data buffer
 * @param total_bytes  Total bytes to send
 * @return Bytes sent
 */
uint32_t bb_la_usb_write_raw(const uint8_t *buf, uint32_t total_bytes);

/**
 * @brief Poll CDC/vendor control endpoints for stream commands.
 *        Commands: 0x01 = start stream, 0x00 = stop.
 *        Called from the USB thread so TinyUSB reads/writes stay serialized.
 */
void bb_la_usb_poll_commands(void);

/**
 * @brief Call from the USB thread (tud_task context) to send pending CDC data.
 *        TinyUSB requires USB writes from the same task as tud_task.
 *        bb_cmd_task queues data, usb_thread calls this to actually send it.
 */
void bb_la_cdc_send_pending(void);

/// Flush the CDC TX ring buffer (call on stream stop to clear stale data).
void bb_la_cdc_flush_ring(void);
