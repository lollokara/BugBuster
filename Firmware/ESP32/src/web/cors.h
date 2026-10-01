// =============================================================================
// cors.h - origin allow-list for the on-device HTTP server (pure, host-testable)
// =============================================================================
#pragma once

#include <string.h>

// True if a browser Origin may be echoed back in Access-Control-Allow-Origin.
// Only loopback dev servers (e.g. `vite dev`) are allowed.
static inline bool cors_origin_allowed(const char *origin)
{
    if (!origin) return false;
    return strncmp(origin, "http://localhost", 16) == 0 ||
           strncmp(origin, "http://127.0.0.1", 16) == 0;
}
