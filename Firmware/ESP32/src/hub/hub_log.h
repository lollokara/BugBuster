#pragma once
#include "hub_types.h"

#ifdef __cplusplus
extern "C" {
#endif

enum { HUB_LOGSRC_S3 = 0, HUB_LOGSRC_MPY = 1, HUB_LOGSRC_P4 = 2 };

#define HUB_LOG_TAG_MAX 32u      /* hub truncates longer tags */
#define HUB_LOG_MSG_MAX 400u     /* ring entries stay small; the hub allows 2000 */

/** Unpacked view; tag/msg point into the packed record (not NUL-terminated). */
typedef struct {
    uint32_t    ms;              /* S3 uptime ms at the event */
    uint8_t     src;
    char        level;           /* E W I D */
    uint8_t     tag_len;
    uint16_t    msg_len;
    const char *tag;
    const char *msg;
} hub_log_rec_t;

/** Record layout: u32 ms | u8 src | u8 level | u8 tag_len | u16 msg_len | tag | msg. Returns bytes, 0 if cap is too small. */
size_t hub_log_pack(uint8_t *out, size_t cap, uint32_t ms, uint8_t src, char level,
                    const char *tag, const char *msg, size_t msg_len);
bool   hub_log_unpack(const uint8_t *rec, uint16_t len, hub_log_rec_t *out);
/** E < W < I < D < V. A level above the threshold is filtered out; an unknown level never passes. */
bool   hub_level_enabled(char level, char threshold);
/** "W (123) tag: text" (optionally ANSI-wrapped). msg/msg_len point into `line`. 0 if not an ESP log line. */
int    hub_log_parse_esp(const char *line, char *level, char *tag, size_t tag_cap,
                         const char **msg, size_t *msg_len);

/** "<ts_ms> <L> <src> <text>\n" as sr_format_line() writes MicroPython output (script_runtime.h).
 *  src/msg point into `line`. 0 if the line is malformed. */
int    hub_log_parse_mpy(const char *line, size_t len, char *level, const char **src, size_t *src_len,
                         const char **msg, size_t *msg_len);

#ifdef __cplusplus
}
#endif
