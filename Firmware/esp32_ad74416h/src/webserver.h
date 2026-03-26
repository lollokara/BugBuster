#pragma once

// =============================================================================
// webserver.h - HTTP API server for AD74416H controller
// =============================================================================

#include <ESPAsyncWebServer.h>
#include "ad74416h_regs.h"

/**
 * @brief Convert a ChannelFunction enum value to a human-readable string.
 */
String channelFunctionToString(ChannelFunction f);

/**
 * @brief Register all HTTP routes on the provided AsyncWebServer.
 *        Call once from setup() after SPIFFS.begin() and initTasks().
 */
void initWebServer(AsyncWebServer& server);
