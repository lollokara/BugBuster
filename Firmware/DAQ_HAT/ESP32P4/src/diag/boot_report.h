#pragma once

// =============================================================================
// boot_report.h - P4 reset reason + crash report, shipped to the hub.
//
// Pure C (no ESP-IDF): formats the records and keeps the RTC breadcrumb logic so
// tests/firmware_host/test_p4_boot_report.py can run it on the host. The ESP glue
// (boot_report_esp.c) feeds it esp_reset_reason(), the coredump summary and the
// breadcrumb, and pushes each line into the log ring the S3 pulls
// (HATP_CMD_LOG_PULL) and ships to the hub as a source=p4 record.
//
// Record format, one line each, `key=value` pairs separated by single spaces,
// <= LOG_MSG_MAX (79) chars so it fits one ring slot. Same idea as the S3
// BOOTRPT lines; a parser keys on the tag and then splits on spaces and '='.
//
//   tag p4rst   (level I for a normal boot, W when abnormal=1)
//     reset=<NAME> code=<esp_reset_reason_t> abnormal=<0|1> boot=<count since power-up>
//   tag p4rst   (previous-boot breadcrumb; only when it was valid)
//     last_act=<DAQ_ACT_* id, 0 = none> run=<DAQ_K_BS_RUN_SELECT> inflight=<0|1> up_ms=<prev uptime>
//   tag p4crash (level W; only when a coredump was found)
//     task=<name> pc=0x<hex> ra=0x<hex>
//   tag p4crash
//     sp=0x<hex> cause=0x<mcause> tval=0x<mtval>
//   tag p4crash
//     elf=<first 9 hex of the app ELF sha256> depth=<n candidate return addresses>
//   tag p4crash
//     why=<IDF panic reason text, truncated>
//   tag p4crash (0..n lines, 5 addresses each)
//     bt<i>=0x<hex>,0x<hex>,...   code-looking words found on the crashed task's stack,
//                                 innermost first; resolve against the ELF named by elf=
//                                 (RISC-V cannot unwind on the device).
//
// `inflight=1` with an abnormal reset means the P4 died inside that S3-requested
// action (e.g. last_act=12 is DAQ_ACT_BS_RUN_LOAD).
// =============================================================================

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define BR_CRUMB_MAGIC 0xB007C0DEu

/** Lives in RTC NOINIT RAM: survives a software/panic/WDT reset, garbage after power-up. */
typedef struct {
    uint32_t magic;       /* BR_CRUMB_MAGIC ^ boot, see br_crumb_valid() */
    uint32_t boot;        /* boots since the last power-up */
    uint32_t up_ms;       /* uptime at the last update */
    uint16_t run;         /* run id selected when the action began */
    uint8_t  last_act;    /* DAQ_ACT_* of the last S3 action */
    uint8_t  inflight;    /* 1 between begin and end */
} br_crumb_t;

/** esp_reset_reason_t value -> name ("PANIC", "TASK_WDT"...); "UNKNOWN" if out of range. */
const char *br_reset_name(int reason);
/** 1 for PANIC / INT_WDT / TASK_WDT / WDT / BROWNOUT / CPU_LOCKUP / PWR_GLITCH. */
int         br_reset_abnormal(int reason);

/** Power-up (breadcrumb invalid) or a reset: update `c` for this boot and return the previous one. */
int  br_crumb_boot(br_crumb_t *c, br_crumb_t *prev_out);
void br_crumb_begin(br_crumb_t *c, uint8_t act, uint16_t run, uint32_t up_ms);
void br_crumb_end(br_crumb_t *c, uint32_t up_ms);

size_t br_fmt_reset(char *out, size_t cap, int reason, uint32_t boot);
size_t br_fmt_crumb(char *out, size_t cap, const br_crumb_t *prev);
size_t br_fmt_crash(char *out, size_t cap, const char *task, uint32_t pc, uint32_t ra);
size_t br_fmt_fault(char *out, size_t cap, uint32_t sp, uint32_t cause, uint32_t tval);
size_t br_fmt_elf(char *out, size_t cap, const char *sha_hex, uint32_t depth);
size_t br_fmt_why(char *out, size_t cap, const char *reason);
/** Code-looking words (0x40000000..0x4fffffff) on a stack dump, in order, de-duplicated. Returns count. */
size_t br_scan_stack(const uint8_t *dump, size_t size, uint32_t *out, size_t max);
/** Next line of `bt<i>=...` starting at *idx (advanced). 0 when all `depth` addresses are out. */
size_t br_fmt_bt(char *out, size_t cap, const uint32_t *bt, uint32_t depth, uint32_t *idx, uint32_t line);

#ifdef __cplusplus
}
#endif

#ifdef __cplusplus
extern "C" {
#endif
/** Call right after log_forward_init() in app_main. Pushes the p4rst / p4crash records. */
void boot_report_init(void);
/** S3-requested action bracket (s3_link): the next boot reports which action was in flight. */
void boot_report_action_begin(uint8_t act, uint16_t run);
void boot_report_action_end(void);
#ifdef __cplusplus
}
#endif
