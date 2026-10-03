#pragma once

// =============================================================================
// log_ring.h - tiny fixed-slot ring of ERROR / LOG_IMPORTANT records the S3 pulls
// over HATP_CMD_LOG_PULL (spec 2026-10-03 section 6). Pure C, no locking: the
// log_forward.c wrapper serialises access. Host-tested by
// tests/firmware_host/test_p4_log_ring.py.
//
// Pull reply: u32 uptime_ms_now | u32 first_seq | u32 dropped | u8 n | u8 more |
//             n x { u32 uptime_ms | u8 level | u8 tag_len | u8 msg_len | tag | msg }
// =============================================================================

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define LOG_RING_SLOTS 64u
#define LOG_TAG_MAX    15u
#define LOG_MSG_MAX    79u
#define LOG_PULL_HDR   14u

typedef struct {
    uint32_t seq;                       /* 0 = never written */
    uint32_t ms;                        /* P4 uptime at the event */
    uint8_t  level;                     /* 'E' | 'W' | 'I' */
    char     tag[LOG_TAG_MAX + 1];
    char     msg[LOG_MSG_MAX + 1];
} log_slot_t;

typedef struct {
    log_slot_t slot[LOG_RING_SLOTS];
    uint32_t   next_seq;                /* seq of the next push; starts at 1 */
} log_ring_t;

void     log_ring_init(log_ring_t *r);
/** Append one record (tag/msg truncated). Returns its seq. */
uint32_t log_ring_push(log_ring_t *r, uint32_t ms, char level, const char *tag, const char *msg);
/** Encode records with seq > after_seq into out (<= cap). Returns bytes written (>= LOG_PULL_HDR)
 *  or 0 if cap < LOG_PULL_HDR. A cursor ahead of the newest record (P4 rebooted) restarts at the oldest. */
size_t   log_ring_pull(const log_ring_t *r, uint32_t after_seq, uint32_t now_ms, uint8_t *out, size_t cap);
/** "E (1234) tag: text", optionally wrapped in ANSI colour -> level/tag/msg. 0 if it is not an ESP log line. */
int      log_ring_parse_line(const char *line, char *level, char *tag, size_t tag_cap,
                             char *msg, size_t msg_cap);

#ifdef __cplusplus
}
#endif
