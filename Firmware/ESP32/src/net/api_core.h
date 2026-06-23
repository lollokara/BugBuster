#pragma once

// =============================================================================
// api_core.h — transport-agnostic JSON API surface.
//
// Single implementation of the device's JSON request/response API, shared by
// every transport (the HTTP webserver and the BLE "API Request" tunnel). A
// caller hands a method + path + optional parsed body; this module gathers data
// from device state / drivers and returns a newly-allocated JSON response
// string. The caller is responsible for authentication and for actually
// sending the bytes over its transport.
//
// This is the canonical place to add or change a control-plane endpoint: do it
// once here and both HTTP and BLE get it, so the two transports can never drift
// apart. High-rate / binary / streaming surfaces (scope/LA/DAQ waveform,
// scripts WebSocket, static files, binary OTA upload) stay transport-specific
// and are NOT routed through here.
// =============================================================================

#include "cJSON.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Handle a JSON API request, independent of transport.
 *
 * @param method HTTP-style verb ("GET"/"POST"), or NULL when the caller does
 *               not distinguish (the BLE tunnel) — routing is primarily by path.
 * @param path   Logical path, e.g. "/api/status", "/api/idac/voltage".
 * @param body   Parsed JSON body for write-style requests (may be NULL).
 * @return Newly-allocated, NUL-terminated JSON response string. The caller owns
 *         it and must free it with cJSON_free(). Unknown paths and errors come
 *         back as {"ok":false,"error":"..."}. Returns NULL only on allocation
 *         failure.
 */
char *api_core_handle(const char *method, const char *path, const cJSON *body);

#ifdef __cplusplus
}
#endif
