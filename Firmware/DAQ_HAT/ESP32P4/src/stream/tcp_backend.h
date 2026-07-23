#pragma once

// =============================================================================
// tcp_backend.h — TCP socket backend for the measurement stream (iOS WiFi
// path), binding the SAME transport-agnostic usb_stream_t framer used by the
// USB-HS backend (usb_backend.c) to a plain TCP socket instead. Reuses the v2
// wire frames (usb_proto.h) unchanged -- only the transport differs.
//
// Only meaningful once the P4 has a WiFi link up (wifi_ap.h, ESP-Hosted over
// the C6 SDIO link). One client at a time: the stream has a single registered
// transport (usb_stream_set_transport), so starting the TCP backend replaces
// whatever transport was previously registered (USB and this WiFi path are
// alternate modes of the same stream, not simultaneous, matching the C6
// display mode-switch that also gates this -- see
// .mex/patterns/daq-hat-ios-wifi-streaming.md).
// =============================================================================

#include <stdint.h>
#include "esp_err.h"
#include "usb_stream.h"

#ifdef __cplusplus
extern "C" {
#endif

// Start the TCP listener task on @p port and register its transport with
// @p stream once a client connects. Call after the WiFi AP/STA link is up
// (wifi_ap_start()). Only one client is served at a time; a new connection
// replaces the previous one.
esp_err_t tcp_backend_start(usb_stream_t *stream, uint16_t port);

// Stop the listener task and drop any connected client.
void tcp_backend_stop(void);

// True while a client is connected.
bool tcp_backend_connected(void);

#ifdef __cplusplus
}
#endif
