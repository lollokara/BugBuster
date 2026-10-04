#pragma once

// =============================================================================
// hub_health.h - periodic HEALTH record (Docs/hub-health-record.md). Pure C, no
// ESP-IDF: the record is built from a plain struct so tests/firmware_host can
// compile it. The hub task fills the struct, builds the JSON and queues it in
// the log ring as chunked "HEALTH <boot> <seq> <i>/<n> <json>" lines (the same
// convention as the boot report's BOOTRPT lines), so offline buffering and
// backfill are the log shipper's.
// =============================================================================

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define HUB_HEALTH_VERSION        1
#define HUB_HEALTH_FIRST_DELAY_MS 60000u      /* first record this long after the hub link is up */
#define HUB_HEALTH_INTERVAL_MS    3600000u    /* then once an hour */
#define HUB_HEALTH_ARM_FALLBACK_MS 300000u    /* hub never reachable: start buffering anyway after 5 min of uptime */
#define HUB_HEALTH_PART_MAX       360u        /* bytes of JSON per log line (same as BOOTRPT) */
#define HUB_HEALTH_MAX_TASKS      12
#define HUB_HEALTH_NAME_MAX       16
#define HUB_HEALTH_JSON_MAX       1536u

typedef struct {
    char    name[HUB_HEALTH_NAME_MAX];
    int32_t min_free;                         /* stack high-water mark: never-used bytes; -1 = task not running */
} hub_health_task_t;

typedef struct {
    uint32_t free, min_free, largest;
} hub_health_pool_t;

typedef struct {
    uint32_t seq;                             /* per-boot record counter, 1-based */
    bool     first;                           /* baseline record ("trig":"boot") vs "hourly" */
    uint32_t epoch_s;                         /* unix s, 0 = wall clock not known yet */
    uint32_t up_ms;
    char     dev[16];                         /* 12 hex: the STA MAC */
    char     fw[32];
    char     elf[20];                         /* ELF sha256 prefix = build id */
    char     idf[32];
    uint32_t boot;
    char     reset_reason[24];
    int      reset_code;
    bool     reset_abnormal;
    hub_health_pool_t heap_int, heap_psram;
    uint8_t  n_tasks;
    hub_health_task_t tasks[HUB_HEALTH_MAX_TASKS];
    bool     coredump;                        /* a valid coredump image is stored */
    uint32_t hat_timeouts;                    /* HAT UART command timeouts since boot */
    uint8_t  hat_streak;                      /* consecutive timeouts right now */
    bool     hat_degraded;
    uint32_t hub_fail;                        /* failed hub exchanges since boot */
    uint32_t log_drop;                        /* log ring overwrites + busy drops since boot */
    uint32_t script_drop;                     /* scripting_log lines dropped (busy) since boot */
    uint32_t wifi_reconn;                     /* STA disconnects since boot */
} hub_health_t;

/** Compact JSON object ({"kind":"health","v":1,...}). Returns its length, 0 if `cap` is too small. */
size_t hub_health_json(char *out, size_t cap, const hub_health_t *h);

/** "HEALTH <boot> <seq> <i>/<n> " + part `i` (0-based) of `json`. Returns bytes written, 0 if it does not fit / bad index. */
size_t hub_health_line(char *out, size_t cap, uint32_t boot, uint32_t seq, const char *json, size_t json_len,
                       size_t i, size_t *n_parts);

/** When to emit: arm on first "link up" (+60 s) or after the 5 min fallback, then every interval. */
typedef struct { bool armed; uint32_t next_ms; } hub_health_sched_t;
bool hub_health_sched_due(hub_health_sched_t *s, uint32_t now_ms, bool link_ready);

#ifdef __cplusplus
}
#endif
