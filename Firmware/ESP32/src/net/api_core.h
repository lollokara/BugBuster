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
#include "esp_err.h"
#include "update/update_manager.h"

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

// ---------------------------------------------------------------------------
// Non-blocking firmware snapshot, for callers that cannot wait on a GitHub
// query — currently the DAQ HAT mainboard tunnel poll (hat_daq_poll_mb).
//
// These own the cache and reuse this file's single TLS worker; do not add
// another one (.mex/patterns/tls-call-needs-dedicated-worker.md).
// ---------------------------------------------------------------------------

/** @brief Copy the cached snapshot, with installed versions refreshed. Never blocks. */
void api_core_fw_get_snapshot(update_snapshot_t *out);

/** @brief Kick a background release-list refresh. No-op while one is in flight. */
void api_core_fw_refresh_async(void);

/**
 * @brief Apply release @p index to @p targets on a background task.
 * @return ESP_ERR_INVALID_STATE when a refresh or apply is already running.
 */
esp_err_t api_core_fw_apply_async(uint8_t index, uint32_t targets);

#ifdef __cplusplus
}
#endif
