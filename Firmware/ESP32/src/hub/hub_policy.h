#pragma once
#include "hub_types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum { HUB_URLSRC_NONE = 0, HUB_URLSRC_EXPLICIT, HUB_URLSRC_MDNS, HUB_URLSRC_CACHED, HUB_URLSRC_DEFAULT } hub_urlsrc_t;

/** Trim, lowercase scheme+host, drop one trailing '/'. Accepts only http://host[:port] (no path, no TLS: the
 *  hub is LAN-only plain HTTP). Returns the length written to out, or 0 if invalid or it does not fit. */
size_t hub_url_normalize(const char *in, char *out, size_t cap);
/** Spec order: explicit NVS url > fresh mDNS result > cached > default. Invalid candidates are skipped. */
hub_urlsrc_t hub_url_pick(const char *explicit_url, const char *discovered_url, const char *cached_url,
                          const char *default_url, char *out, size_t cap);
const char *hub_urlsrc_name(hub_urlsrc_t s);

typedef enum { HUB_RES_OK = 0, HUB_RES_RETRY, HUB_RES_DROP } hub_res_t;
/** Transport failure, 408, 429 and 5xx are retried; 2xx succeeds; any other status will never succeed: drop the batch. */
hub_res_t hub_classify_status(int transport_ok, int http_status);
/** 0 -> 5000, then doubling, capped at 60000 ms. */
uint32_t hub_backoff_next(uint32_t cur_ms);
/** HAT mutex busy backoff: 100 ms -> 200 ms -> 400 ms -> 800 ms -> capped at 1000 ms. 0 returns 100. */
uint32_t hub_hat_backoff_next(uint32_t cur_ms);
/** True if the periodic pacing interval has elapsed (handles uptime rollover). */
bool hub_pace_due(uint32_t now_ms, uint32_t last_ms, uint32_t period_ms);

#ifdef __cplusplus
}
#endif
