#include "hub_ratelimit.h"
#include <stdio.h>
#include <string.h>

void hub_ratelimit_init(hub_ratelimit_t *rl, uint32_t max_burst, uint32_t window_ms)
{
    if (!rl) return;
    memset(rl, 0, sizeof(*rl));
    rl->max_burst = max_burst ? max_burst : HUB_RATELIMIT_DEFAULT_BURST;
    rl->window_ms = window_ms ? window_ms : HUB_RATELIMIT_DEFAULT_WINDOW_MS;
}

void hub_ratelimit_reset(hub_ratelimit_t *rl)
{
    if (!rl) return;
    for (size_t i = 0; i < HUB_RATELIMIT_MAX_ENTRIES; i++) {
        rl->entries[i].active = false;
        rl->entries[i].count = 0;
        rl->entries[i].suppressed_count = 0;
    }
}

static int find_entry(const hub_ratelimit_t *rl, const char *tag, const char *msg, size_t len)
{
    for (size_t i = 0; i < HUB_RATELIMIT_MAX_ENTRIES; i++) {
        if (!rl->entries[i].active) continue;
        if (strcmp(rl->entries[i].tag, tag) == 0 &&
            rl->entries[i].msg_len == len &&
            memcmp(rl->entries[i].msg, msg, len) == 0) {
            return (int)i;
        }
    }
    return -1;
}

static int alloc_entry(const hub_ratelimit_t *rl, uint32_t now_ms)
{
    int oldest_idx = -1;
    uint32_t oldest_start = 0;

    for (size_t i = 0; i < HUB_RATELIMIT_MAX_ENTRIES; i++) {
        if (!rl->entries[i].active) return (int)i;
        uint32_t age = now_ms - rl->entries[i].window_start_ms;
        if (oldest_idx < 0 || age > oldest_start) {
            oldest_idx = (int)i;
            oldest_start = age;
        }
    }
    return oldest_idx;
}

bool hub_ratelimit_filter(hub_ratelimit_t *rl, uint32_t now_ms, uint8_t src, char level,
                          const char *tag, const char *msg, size_t len)
{
    if (!rl || !tag || !msg) return true;

    size_t tl = strlen(tag);
    if (tl > HUB_LOG_TAG_MAX) tl = HUB_LOG_TAG_MAX;
    if (len > HUB_LOG_MSG_MAX) len = HUB_LOG_MSG_MAX;

    int idx = find_entry(rl, tag, msg, len);
    if (idx >= 0) {
        hub_ratelimit_entry_t *e = &rl->entries[idx];
        if (now_ms - e->window_start_ms >= rl->window_ms) {
            // Window expired
            e->window_start_ms = now_ms;
            e->count = 1;
            e->suppressed_count = 0;
            return true;
        }
        if (e->count < rl->max_burst) {
            e->count++;
            return true;
        }
        // Exceeded burst limit
        e->suppressed_count++;
        return false;
    }

    // New entry
    idx = alloc_entry(rl, now_ms);
    if (idx < 0) return true;

    hub_ratelimit_entry_t *e = &rl->entries[idx];
    e->active = true;
    e->src = src;
    e->level = level;
    e->window_start_ms = now_ms;
    e->count = 1;
    e->suppressed_count = 0;
    strncpy(e->tag, tag, HUB_LOG_TAG_MAX);
    e->tag[HUB_LOG_TAG_MAX] = '\0';
    e->msg_len = (uint16_t)len;
    memcpy(e->msg, msg, len);
    e->msg[len] = '\0';

    return true;
}

void hub_ratelimit_flush_expired(hub_ratelimit_t *rl, uint32_t now_ms,
                                 hub_ratelimit_emit_fn emit_fn, void *user)
{
    if (!rl || !emit_fn) return;

    for (size_t i = 0; i < HUB_RATELIMIT_MAX_ENTRIES; i++) {
        hub_ratelimit_entry_t *e = &rl->entries[i];
        if (!e->active) continue;

        if (now_ms - e->window_start_ms >= rl->window_ms) {
            if (e->suppressed_count > 0) {
                char summary[HUB_LOG_MSG_MAX + 1];
                int n = snprintf(summary, sizeof(summary),
                                 "suppressed %u x %s: %.*s",
                                 (unsigned)e->suppressed_count,
                                 e->tag,
                                 (int)e->msg_len,
                                 e->msg);
                if (n > 0) {
                    size_t slen = (size_t)n;
                    if (slen > HUB_LOG_MSG_MAX) slen = HUB_LOG_MSG_MAX;
                    emit_fn(user, now_ms, e->src, e->level, e->tag, summary, slen);
                }
            }
            e->active = false;
            e->count = 0;
            e->suppressed_count = 0;
        }
    }
}
