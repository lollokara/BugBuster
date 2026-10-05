#pragma once
// =============================================================================
// crash_report.h - boot diagnostics package, saved-coredump access and the
// pre-crash breadcrumb.
//
//  * crash_report_init()          first thing in app_main: reads the previous
//                                 breadcrumb and the reset reason.
//  * crash_report_phase()/tick()  keep an RTC-RAM snapshot (heap, tightest
//                                 stack, boot phase) fresh, so a crash on the
//                                 NEXT boot can say what the system looked
//                                 like just before it died.
//  * crash_report_boot_complete() end of app_main. From mainLoopTask's tick the
//                                 module loads the coredump summary, waits for
//                                 the system to settle and emits the report as
//                                 "BOOTRPT <boot> <section> <i>/<n> <json>" log
//                                 lines, which the remote log shipper forwards.
//  * crash_report_api_*()         JSON entry points used by api_core (HTTP + BLE).
//  * crash_report_dump_*()        raw coredump access for the HTTP binary download.
// =============================================================================

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Seconds of steady state after boot before the report goes out. */
#define CRASH_REPORT_SETTLE_MS  30000u

void crash_report_init(void);
void crash_report_phase(uint8_t phase);          /* CRASH_PHASE_* in crash_util.h */
void crash_report_tick(uint32_t now_ms);         /* cheap; call from mainLoopTask */
void crash_report_boot_complete(void);
/* Boot counter and last reset (name, raw code, abnormal) for the hub health record. */
void crash_report_boot_info(uint32_t *boot, const char **reason, int *code, bool *abnormal);

/* GET /api/system/crash[?report=1 | ?offset=N&len=M]. Returns cJSON_free()-able JSON. */
char *crash_report_api_get(const char *path);
/* POST /api/system/crash/clear: erase the stored coredump. */
char *crash_report_api_clear(void);

/* Raw coredump access (HTTP streams it as application/octet-stream). The size is
 * 0 when no valid dump is stored or the boot-time check has not finished. */
size_t    crash_report_dump_size(void);
esp_err_t crash_report_dump_read(size_t offset, void *buf, size_t len);

#ifdef __cplusplus
}
#endif
