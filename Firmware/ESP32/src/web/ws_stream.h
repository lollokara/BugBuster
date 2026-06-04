#pragma once

// =============================================================================
// ws_stream.h — Binary WebSocket streaming endpoint at /api/ws/stream.
//
// Closes the "HTTP transport doesn't support streaming" gap: ADC/scope/LA
// metadata frames are also forwarded to a connected WS client, so the desktop
// app, Python lib, and on-device web UI can stream live samples over WiFi
// without depending on USB CDC.
//
// Wire format for outbound frames (binary WS messages):
//   [opcode:u8][stream_id:u8][len:u16 LE][payload[len]]
// Stream IDs:
//   0x00  WS_STREAM_SCOPE   — scope buckets (mirrors BBP_EVT_SCOPE_DATA payload)
//   0x01  WS_STREAM_ADC     — ADC samples   (mirrors BBP_EVT_ADC_DATA payload)
//   0x02  WS_STREAM_LA_META — LA capture metadata
//
// Auth: first text frame after handshake must be the bearer admin token.
// Subscribe: subsequent text frames are JSON, e.g. {"subscribe":["scope","adc"]}
// =============================================================================

#include <stddef.h>
#include <stdint.h>
#include "esp_http_server.h"

#ifdef __cplusplus
extern "C" {
#endif

#define WS_STREAM_SCOPE     0x00u
#define WS_STREAM_ADC       0x01u
#define WS_STREAM_LA_META   0x02u
#define WS_STREAM_ADC_DSP   0x03u  // DSP stream: stats + FFT peaks + spikes

// Subscription bitmask values (1 << stream_id).
#define WS_SUB_SCOPE        (1u << WS_STREAM_SCOPE)
#define WS_SUB_ADC          (1u << WS_STREAM_ADC)
#define WS_SUB_LA_META      (1u << WS_STREAM_LA_META)
#define WS_SUB_ADC_DSP      (1u << WS_STREAM_ADC_DSP)

/**
 * Register /api/ws/stream on @p server. Call once from startWebServer().
 */
void ws_stream_register(httpd_handle_t server);

/**
 * Push a payload to the active WS client if it has subscribed to @p stream_id.
 * Safe to call from any FreeRTOS task (uses an internal mutex). No-op when
 * there is no session, the client hasn't authenticated, or the subscription
 * mask doesn't include this stream.
 *
 * @param stream_id  WS_STREAM_* identifier.
 * @param payload    Bytes to forward (will be wrapped in the 4-byte header).
 * @param len        Payload length in bytes (max ~1.5 KB to fit one MSS).
 */
void ws_stream_forward(uint8_t stream_id, const uint8_t* payload, size_t len);

/** Return non-zero if a client is currently subscribed to @p stream_id. */
int  ws_stream_has_subscriber(uint8_t stream_id);

#ifdef __cplusplus
}
#endif
