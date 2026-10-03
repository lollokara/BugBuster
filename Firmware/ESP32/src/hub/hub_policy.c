#include "hub_policy.h"
#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

size_t hub_url_normalize(const char *in, char *out, size_t cap)
{
    if (!in) return 0;
    while (isspace((unsigned char)*in)) in++;
    size_t n = strlen(in);
    while (n && isspace((unsigned char)in[n - 1])) n--;
    if (n && in[n - 1] == '/') n--;
    if (n < 8 || n > 95 || n + 1 > cap || strncasecmp(in, "http://", 7) != 0) return 0;
    const char *host = in + 7;
    size_t hl = 0;
    while (7 + hl < n && in[7 + hl] != ':') {
        char c = in[7 + hl];
        if (!(isalnum((unsigned char)c) || c == '.' || c == '-')) return 0;
        hl++;
    }
    if (hl == 0) return 0;
    if (7 + hl < n) {                                   /* ":port" */
        const char *port = in + 7 + hl + 1;
        size_t pl = n - (7 + hl + 1);
        if (pl == 0 || pl > 5) return 0;
        long v = 0;
        for (size_t k = 0; k < pl; k++) {
            if (!isdigit((unsigned char)port[k])) return 0;
            v = v * 10 + (port[k] - '0');
        }
        if (v < 1 || v > 65535) return 0;
    }
    memcpy(out, "http://", 7);
    for (size_t k = 0; k < hl; k++) out[7 + k] = (char)tolower((unsigned char)host[k]);
    memcpy(out + 7 + hl, in + 7 + hl, n - 7 - hl);
    out[n] = '\0';
    return n;
}

hub_urlsrc_t hub_url_pick(const char *explicit_url, const char *discovered_url, const char *cached_url,
                          const char *default_url, char *out, size_t cap)
{
    const char *cand[4] = { explicit_url, discovered_url, cached_url, default_url };
    static const hub_urlsrc_t SRC[4] = { HUB_URLSRC_EXPLICIT, HUB_URLSRC_MDNS, HUB_URLSRC_CACHED, HUB_URLSRC_DEFAULT };
    for (int k = 0; k < 4; k++) {
        if (hub_url_normalize(cand[k], out, cap)) return SRC[k];
    }
    if (cap) out[0] = '\0';
    return HUB_URLSRC_NONE;
}

const char *hub_urlsrc_name(hub_urlsrc_t s)
{
    switch (s) {
    case HUB_URLSRC_EXPLICIT: return "explicit";
    case HUB_URLSRC_MDNS:     return "mdns";
    case HUB_URLSRC_CACHED:   return "cached";
    case HUB_URLSRC_DEFAULT:  return "default";
    default:                  return "none";
    }
}

hub_res_t hub_classify_status(int transport_ok, int http_status)
{
    if (!transport_ok) return HUB_RES_RETRY;
    if (http_status >= 200 && http_status < 300) return HUB_RES_OK;
    if (http_status == 408 || http_status == 429 || http_status >= 500) return HUB_RES_RETRY;
    return HUB_RES_DROP;
}

uint32_t hub_backoff_next(uint32_t cur_ms)
{
    if (cur_ms == 0) return 5000;
    return cur_ms >= 30000 ? 60000 : cur_ms * 2;
}
