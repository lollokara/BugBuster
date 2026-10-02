#pragma once
// =============================================================================
// http_adapter.h — HTTP transport adapter for the command registry
// =============================================================================
#include "esp_http_server.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Register all registry-backed HTTP routes on the given server handle.
 * Called from initWebServer() after the server is started.
 * Currently registers:
 *   GET  /api/registry/get_dac_readback
 * (the unauthenticated set_dac_* POSTs were removed - PLT-01; DAC writes go
 * through the authenticated /api/channel/<n>/dac route).
 */
void http_adapter_register(httpd_handle_t server);

#ifdef __cplusplus
}
#endif
