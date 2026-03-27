#pragma once

// =============================================================================
// webserver.h - HTTP API server for AD74416H controller (ESP-IDF httpd)
// =============================================================================

#include "esp_http_server.h"
#include "ad74416h_regs.h"

/**
 * @brief Convert a ChannelFunction enum value to a human-readable string.
 */
const char* channelFunctionToString(ChannelFunction f);

/**
 * @brief Create and start the HTTP server, registering all URI handlers.
 *        Call once from setup() after SPIFFS is mounted and initTasks().
 */
void initWebServer(void);

/**
 * @brief Stop the HTTP server and release resources.
 */
void stopWebServer(void);
