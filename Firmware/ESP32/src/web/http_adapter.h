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
 * Currently registers DAC routes:
 *   POST /api/registry/set_dac_code
 *   POST /api/registry/set_dac_voltage
 *   POST /api/registry/set_dac_current
 *   GET  /api/registry/get_dac_readback
 */
void http_adapter_register(httpd_handle_t server);

#ifdef __cplusplus
}
#endif
