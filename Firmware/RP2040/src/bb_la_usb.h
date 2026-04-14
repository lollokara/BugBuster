#pragma once

// =============================================================================
// bb_la_usb.h — Logic Analyzer USB transport helpers
//
// la_dbg() — Non-blocking CDC debug helper shared by bb_la_usb.c and bb_la.c.
// Drops output silently when CDC TX buffer is near-full to avoid blocking the
// USB task. MUST NOT be called from ISR context (dma_irq_handler etc.).
//
// Vendor bulk is the primary LA data path. One-shot capture readout keeps its
// existing length-prefixed bulk format; live streaming uses packetized bulk
// frames over the same interface.
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

#define LA_USB_STREAM_PKT_START 0x01
#define LA_USB_STREAM_PKT_DATA  0x02
#define LA_USB_STREAM_PKT_STOP  0x03
#define LA_USB_STREAM_PKT_ERROR 0x04

#define LA_USB_STREAM_INFO_NONE           0x00
#define LA_USB_STREAM_INFO_START_REJECTED 0x80

// Recovery state for the LA vendor endpoint re-arm path.
// This is intentionally a single byte so it can ride on the existing LA status
// payload without widening the frame shape.
#define BB_LA_USB_RECOVERY_IDLE      0
#define BB_LA_USB_RECOVERY_PENDING   1
#define BB_LA_USB_RECOVERY_COMPLETED 2

/**
 * @brief Non-blocking CDC debug print.
 *        Drops output silently when CDC TX buffer < 64 bytes.
 *        MUST NOT be called from ISR context.
 */
void bb_la_dbg(const char *fmt, ...);

/**
 * @brief Send a log message via HAT UART → ESP32 → host.
 *        Gated by s_la_log_enabled (default off).
 *        MUST NOT be called from ISR context.
 */
void bb_la_log(const char *fmt, ...);

/**
 * @brief Initialize LA USB streaming.
 */
void bb_la_usb_init(void);

/**
 * @brief Reset the live-stream packet sequence counter.
 */
void bb_la_usb_live_reset_sequence(void);

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
 * @brief Stream packetized live LA data over the vendor bulk endpoint.
 *        Each packet uses the format:
 *        [type:u8][seq:u8][payload_len:u8][info:u8][payload bytes...]
 *        where DATA payload_len is <= 60 bytes.
 * @param buf        Data buffer
 * @param total_bytes  Total bytes to send
 * @return Payload bytes actually sent
 */
uint32_t bb_la_usb_write_live(const uint8_t *buf, uint32_t total_bytes);

/**
 * @brief Send a live-stream control marker on the vendor bulk endpoint.
 *        Used for START / STOP / ERROR notifications.
 * @param packet_type  One of LA_USB_STREAM_PKT_*
 * @param info         Stop/error info byte
 * @return true if the marker was written in full
 */
bool bb_la_usb_send_stream_marker(uint8_t packet_type, uint8_t info);

/**
 * @brief Poll CDC/vendor control endpoints for stream commands.
 *        Commands: 0x01 = start stream, 0x00 = stop.
 *        Called from the USB thread so TinyUSB reads/writes stay serialized.
 */
void bb_la_usb_poll_commands(void);

/**
 * @brief Call from the USB thread (tud_task context) to send pending vendor bulk data.
 */
void bb_la_usb_send_pending(void);

/**
 * @brief Register a large buffer to be streamed via USB bulk IN.
 *        Used for one-shot readout (HAT_CMD_LA_USB_SEND).
 * @param buf         The buffer to send
 * @param total_bytes Total bytes
 */
void bb_la_usb_register_readout(const uint8_t *buf, uint32_t total_bytes);

/**
 * @brief Reset the bulk transmit state (aborting any ongoing readout or live stream).
 */
void bb_la_usb_abort_bulk(void);

/**
 * @brief Request a deferred PKT_STOP.  The marker will be emitted by
 *        bb_la_usb_send_pending() only after the current data buffer has been
 *        fully drained, guaranteeing clean 64-byte packet alignment on the wire.
 *        Safe to call from any task context.
 * @param info  Stop-reason info byte (LA_STREAM_STOP_HOST / DMA_OVERRUN / …)
 */
void bb_la_usb_request_deferred_stop(uint8_t info);

/**
 * @brief Call from the USB thread (tud_task context) to send pending legacy CDC data.
 *        TinyUSB requires CDC writes from the same task as tud_task.
 */
void bb_la_cdc_send_pending(void);

/// Flush the CDC TX ring buffer (call on stream stop to clear stale data).
void bb_la_cdc_flush_ring(void);

/**
 * @brief Wake the USB task from an ISR context (e.g. DMA completion IRQ).
 *        Calls xTaskNotifyFromISR so bb_la_usb_send_pending() runs
 *        immediately on the next scheduler tick rather than waiting up to
 *        1 ms for the USB task's periodic timeout.
 *        No-op when not compiled with DEBUGPROBE_INTEGRATION.
 */
void bb_la_usb_notify_task_from_isr(void);

/**
 * @brief Returns true while a live stream is actively sending data.
 *        Used by the USB task loop to switch to a tight polling mode
 *        that maximises bulk throughput.
 */
bool bb_la_usb_is_streaming(void);

/**
 * @brief Check if there is pending data to send (active buffer, ctrl markers,
 *        or deferred stop).  Used by the USB task fast-path to keep pumping
 *        until the current half-buffer is fully drained.
 */
bool bb_la_usb_has_pending_data(void);

/**
 * @brief True while a STOP/abort-triggered endpoint re-arm is still pending.
 */
bool bb_la_usb_rearm_pending(void);

/**
 * @brief Small saturating counters for STOP-based endpoint recovery.
 *        Kept at 1 byte each so LA status stays within the HAT frame budget.
 */
uint8_t bb_la_usb_rearm_request_count(void);
uint8_t bb_la_usb_rearm_complete_count(void);
