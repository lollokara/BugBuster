#pragma once

// =============================================================================
// mdns_responder.h — ESP-IDF mDNS responder for zero-config discovery.
//
// Advertises BugBuster on the LAN as:
//   - <hostname>.local (default: bugbuster-<last3macbytes>)
//   - _http._tcp        port 80   (TXT: version, mac, proto, model)
//   - _bugbuster._tcp   port 80   (custom service for client filtering)
//
// Lifecycle: call mdns_responder_init() once, after wifi_init() returns.
// ESP-IDF's mdns component tracks netif up/down internally — no per-event
// hooks needed in wifi_manager.
// =============================================================================

#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MDNS_HOSTNAME_MAX   32   // POSIX hostname recommendation (excl. .local)

/**
 * Initialise the mDNS responder. Reads any user-overridden hostname from NVS,
 * otherwise derives bugbuster-<last3macbytes> from the AP MAC.
 *
 * Safe to call before WiFi has an IP — ESP-IDF mdns will start advertising as
 * soon as a netif comes up.
 *
 * @return true on success, false if mdns_init failed.
 */
bool mdns_responder_init(void);

/**
 * Replace the advertised hostname. Persists to NVS and re-registers the
 * service set so existing browsers see the change immediately.
 *
 * @param hostname  1..MDNS_HOSTNAME_MAX-1 chars, lowercase letters/digits/'-'.
 *                  Pass NULL or "" to revert to the auto-derived default.
 * @return true on success, false on validation failure or mdns API error.
 */
bool mdns_responder_set_hostname(const char* hostname);

/**
 * Copy the currently advertised hostname (without .local) into @p out.
 * @return number of bytes written (excluding null), or 0 if not initialised.
 */
size_t mdns_responder_get_hostname(char* out, size_t out_sz);

#ifdef __cplusplus
}
#endif
