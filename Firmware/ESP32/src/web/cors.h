// =============================================================================
// cors.h - origin allow-list for the on-device HTTP server (pure, host-testable)
// =============================================================================
#pragma once

#include <string.h>

// True if a browser Origin may be echoed back in Access-Control-Allow-Origin.
// Only loopback dev servers (e.g. `vite dev`): exact host, optional numeric port.
static inline bool cors_origin_allowed(const char *origin)
{
    if (!origin) return false;
    static const char *const hosts[] = { "http://localhost", "http://127.0.0.1" };
    for (size_t i = 0; i < sizeof(hosts) / sizeof(hosts[0]); i++) {
        const char *h = hosts[i];
        size_t n = strlen(h);
        if (strncmp(origin, h, n) != 0) continue;
        const char *p = origin + n;
        if (*p == '\0') return true;
        if (*p++ != ':' || *p == '\0') return false;
        for (; *p; p++) {
            if (*p < '0' || *p > '9') return false;
        }
        return true;
    }
    return false;
}
