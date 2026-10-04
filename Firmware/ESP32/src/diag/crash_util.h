#pragma once
// =============================================================================
// crash_util.h - pure-C helpers for the crash/boot diagnostics (crash_report.cpp).
//
// No ESP-IDF dependency, so tests/firmware_host compiles and runs this file as is.
//   * pre-crash "breadcrumb" record kept in RTC NOINIT RAM (magic + CRC)
//   * names for the reset reason and the Xtensa EXCCAUSE codes
//   * range clamping for the chunked coredump read
//   * splitting a JSON section into fixed-size log-line parts
// =============================================================================

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CRASH_CRUMB_MAGIC    0x42424352u   /* 'BBCR' */
#define CRASH_CRUMB_VERSION  1u
#define CRASH_TASK_NAME_MAX  16

/* Largest raw slice returned per chunk request. 768 B -> 1024 B of base64,
 * which fits one BLE tunnel reply and one small heap buffer on the httpd task. */
#define CRASH_CHUNK_MAX      768u

/* Snapshot of the system state, refreshed every few seconds while running. It
 * lives in RTC NOINIT RAM, so after a panic / watchdog reset the next boot can
 * report the last known heap, stack and boot-phase state. Written only by
 * crash_report_tick(), sealed last so a torn write fails the CRC. */
typedef struct {
    uint32_t magic;
    uint32_t crc;              /* CRC-32 over every byte after this field */
    uint16_t version;
    uint16_t size;             /* sizeof(crash_crumb_t) */
    uint32_t boot_count;       /* increments every boot, cleared on power-on */
    uint32_t crash_streak;     /* consecutive abnormal resets, cleared on a clean one */
    uint32_t uptime_ms;        /* uptime of the boot this snapshot belongs to */
    uint32_t int_free;         /* internal heap: free / lowest ever / largest block */
    uint32_t int_min;
    uint32_t int_largest;
    uint32_t psram_free;
    uint32_t psram_min;
    uint32_t min_stack_free;   /* tightest registered task stack, bytes free */
    uint8_t  phase;            /* CRASH_PHASE_* */
    uint8_t  flags;            /* CRASH_FLAG_* */
    uint8_t  reserved[2];
    char     min_stack_task[CRASH_TASK_NAME_MAX];
} crash_crumb_t;

enum {
    CRASH_PHASE_NONE = 0,
    CRASH_PHASE_EARLY,        /* app_main entered */
    CRASH_PHASE_NET,          /* WiFi / BLE / mDNS */
    CRASH_PHASE_DRIVERS,      /* SPI, I2C, tasks, HAT */
    CRASH_PHASE_WEB,          /* web server start */
    CRASH_PHASE_SCRIPTING,    /* MicroPython init */
    CRASH_PHASE_RUNNING,      /* boot complete, steady state */
    CRASH_PHASE_COUNT
};

enum {
    CRASH_FLAG_WIFI_STA   = 1u << 0,
    CRASH_FLAG_SCRIPT_RUN = 1u << 1,
    CRASH_FLAG_HAT        = 1u << 2,
    CRASH_FLAG_USB        = 1u << 3,
};

uint32_t    crash_crc32(const void *data, size_t len);
void        crash_crumb_seal(crash_crumb_t *c);
bool        crash_crumb_valid(const crash_crumb_t *c);

const char *crash_phase_name(unsigned phase);
/* Numeric values of esp_reset_reason_t (stable across IDF 5.x). */
const char *crash_reset_reason_name(int reason);
/* True when the reset indicates a fault: panic, any watchdog, brownout, CPU lockup. */
bool        crash_reset_is_abnormal(int reason);
/* Xtensa EXCCAUSE (esp_core_dump_summary_extra_info_t.exc_cause). NULL if unknown. */
const char *crash_exccause_name(uint32_t cause);

/* Validate a chunk request against a dump of `total` bytes. On success returns
 * true and sets *out_len to the number of bytes to read (clamped to
 * CRASH_CHUNK_MAX and to the end of the dump). Fails for offset >= total or
 * len == 0. */
bool        crash_clamp_range(size_t total, size_t offset, size_t len, size_t *out_len);

/* Number of parts needed to carry `len` bytes in lines of at most `max_part` bytes. */
size_t      crash_part_count(size_t len, size_t max_part);

#ifdef __cplusplus
}
#endif
