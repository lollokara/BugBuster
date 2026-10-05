#pragma once

// =============================================================================
// hub_logs.h - log shipper (spec 2026-10-03 section 6): the S3 esp_log hook, the
// MicroPython output tee and the P4 pull all feed one 64 KB PSRAM ring; the hub
// task ships it in batches of 5 s or 4 KB. Producers never block: if the ring
// lock is busy the line is dropped (and counted).
// =============================================================================

#include "hub_net.h"
#include "hub_log.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Idempotent. Allocates the ring and installs the esp_log vprintf hook + script tee. Call early in app_main. */
bool     hub_logs_init(void);
/** S3 firmware log threshold (E W I D). Script and P4 records are always shipped. */
void     hub_logs_set_level(char level);
/** Any task, non-blocking. `src` is HUB_LOGSRC_*; the S3 level filter applies to HUB_LOGSRC_S3 only. */
void     hub_logs_push(uint8_t src, char level, const char *tag, const char *msg, size_t len);
/** Hub task: pull the P4's ERROR/important records into the ring (no HTTP). */
void     hub_logs_pull_p4(void);
/** Hub task: ship one batch if one is due (4 KB or 5 s) or `force`. RETRY = hub unreachable / 5xx. */
hub_step_t hub_logs_ship(const char *base, bool force);
uint32_t hub_logs_backlog(void);
/** Ring overwrites + producer-busy drops since boot. */
uint32_t hub_logs_dropped_total(void);
/** Hub task: queue one "HEALTH ..." line (hub_health.h) regardless of the level threshold / rate limiter. */
void     hub_logs_push_health(const char *msg, size_t len);

#ifdef __cplusplus
}
#endif
