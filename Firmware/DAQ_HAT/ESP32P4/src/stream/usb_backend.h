#pragma once

// =============================================================================
// usb_backend.h — TinyUSB HS vendor-bulk backend for the measurement stream.
//
// Binds the transport-agnostic usb_stream framer (usb_stream.*) to the
// ESP32-P4 USB High-Speed OTG controller via esp_tinyusb's vendor class.
//   * Outbound: usb_stream writes frames through the registered transport,
//     which calls tud_vendor_write()/flush() on bulk IN endpoint 0x81.
//   * Inbound:  tud_vendor_rx_cb() drains the bulk OUT endpoint 0x01 and feeds
//     bytes to usb_stream_on_rx() for control-command decoding.
//
// If the TinyUSB vendor class is not compiled in (CFG_TUD_VENDOR == 0), this is
// a no-op that logs a warning, so the firmware still builds and runs.
// =============================================================================

#include "esp_err.h"
#include "usb_stream.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Install the TinyUSB driver (HS vendor class) and register its transport
 *        with @p stream. After this returns, @p stream can send frames over USB.
 */
esp_err_t usb_backend_start(usb_stream_t *stream);

/** @brief True once the USB host has mounted the device. */
bool usb_backend_mounted(void);

/**
 * @brief Periodic USB enumeration watchdog. Call at ~1 Hz.
 *
 * This board has no VBUS-sense line, so the HS PHY runs bus-powered (the D+
 * pull-up is always asserted) and firmware cannot distinguish "no cable" from
 * "cable present but host gave up". If the device stays unmounted for a few
 * seconds, this forces a virtual re-plug (drop + re-assert the pull-up) so the
 * host restarts enumeration — the software substitute for a VBUS-triggered
 * reconnect. It is a no-op while the device is mounted and harmless when no
 * host is attached.
 */
void usb_backend_poll(void);

#ifdef __cplusplus
}
#endif
