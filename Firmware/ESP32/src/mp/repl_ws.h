#pragma once
// =============================================================================
// repl_ws.h — WebSocket REPL public API.
//
// Registers the /api/scripts/repl/ws WebSocket handler with the running httpd
// instance and provides the output-forward hook called by scripting_log_push.
// =============================================================================

#include "esp_http_server.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Register the /api/scripts/repl/ws WebSocket URI handler with @p server.
 * Call once from startWebServer() after the server is running.
 */
void repl_ws_register(httpd_handle_t server);

/**
 * Forward @p len bytes of MicroPython stdout to the active REPL WebSocket
 * session, if one is connected.  Called from scripting_log_push() — must be
 * safe to call from any FreeRTOS task.  No-op when no session is active.
 */
void repl_ws_forward(const char *str, size_t len);

#ifdef __cplusplus
}
#endif
