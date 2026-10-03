#pragma once
#include "hub_types.h"
#include "hub_log.h"

#ifdef __cplusplus
extern "C" {
#endif

#define HUB_RATELIMIT_MAX_ENTRIES 32u
#define HUB_RATELIMIT_DEFAULT_BURST 5u
#define HUB_RATELIMIT_DEFAULT_WINDOW_MS 10000u

typedef struct {
    uint32_t count;
    uint32_t window_start_ms;
    uint32_t suppressed_count;
    uint8_t  src;
    char     level;
    char     tag[HUB_LOG_TAG_MAX + 1];
    uint16_t msg_len;
    char     msg[HUB_LOG_MSG_MAX + 1];
    bool     active;
} hub_ratelimit_entry_t;

typedef struct {
    hub_ratelimit_entry_t entries[HUB_RATELIMIT_MAX_ENTRIES];
    uint32_t max_burst;
    uint32_t window_ms;
} hub_ratelimit_t;

typedef void (*hub_ratelimit_emit_fn)(void *user, uint32_t ms, uint8_t src, char level,
                                     const char *tag, const char *msg, size_t len);

/** Initialize the rate limiter with bursting rules. */
void hub_ratelimit_init(hub_ratelimit_t *rl, uint32_t max_burst, uint32_t window_ms);

/** Reset all tracking state. */
void hub_ratelimit_reset(hub_ratelimit_t *rl);

/**
 * Filter an incoming log message.
 * Returns true if the message should be passed through immediately.
 * Returns false if the message is suppressed under the rate limit.
 */
bool hub_ratelimit_filter(hub_ratelimit_t *rl, uint32_t now_ms, uint8_t src, char level,
                          const char *tag, const char *msg, size_t len);

/**
 * Check active entries and emit summary records for any closed windows
 * that had suppressed messages.
 */
void hub_ratelimit_flush_expired(hub_ratelimit_t *rl, uint32_t now_ms,
                                 hub_ratelimit_emit_fn emit_fn, void *user);

#ifdef __cplusplus
}
#endif
