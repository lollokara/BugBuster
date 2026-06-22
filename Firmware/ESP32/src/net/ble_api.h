#pragma once

// =============================================================================
// ble_api.h — JSON request dispatcher for the BLE API tunnel.
//
// The BLE "API Request" characteristic hands a path + optional body here; this
// module gathers data from device state / drivers and returns a compact JSON
// response string. It mirrors the small-data subset of the HTTP API so the iOS
// app can issue the same logical requests over BLE (everything except the
// high-rate scope/LA/DAQ-waveform streams, which stay on WiFi/USB).
//
// Extending: add a branch in ble_api_dispatch(). Keep responses compact — they
// are chunked over BLE notifications, so smaller is better.
// =============================================================================

#include "cJSON.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Dispatch a BLE API request.
 *
 * @param path  Logical path, e.g. "/api/status", "/api/hat", "/api/idac/voltage".
 * @param body  Parsed JSON body for write-style requests (may be NULL).
 * @return Newly-allocated, NUL-terminated JSON response string. The caller owns
 *         it and must free it with cJSON_free(). Never returns NULL except on
 *         allocation failure; unknown paths and errors come back as
 *         {"ok":false,"error":"..."}.
 */
char *ble_api_dispatch(const char *path, const cJSON *body);

#ifdef __cplusplus
}
#endif
