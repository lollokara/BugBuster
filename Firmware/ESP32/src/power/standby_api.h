#pragma once
// =============================================================================
// standby_api.h - transport-neutral surface of the S3 standby coordinator.
// Pure C: BBP_CMD_STANDBY (0x78) codec, JSON status/request handling for
// /api/standby/*, and the passive-vs-work classification the BBP adapter and
// the HTTP trampoline use for the operation barrier.
// =============================================================================

#include <stddef.h>
#include <stdint.h>

#include "standby_system.h"

#ifdef __cplusplus
extern "C" {
#endif

/* BBP_CMD_STANDBY sub-ops (first payload byte). Every success reply is the
 * 28-byte status below. */
#define STANDBY_SUBOP_STATUS   0u   /* len 1 */
#define STANDBY_SUBOP_PRESENCE 1u   /* len 6: client u32, present u8 */
#define STANDBY_SUBOP_POLICY   2u   /* len 3: timeout seconds u16 */
#define STANDBY_SUBOP_WAKE     3u   /* len 1 */
#define STANDBY_SUBOP_SLEEP    4u   /* len 1 */
#define STANDBY_STATUS_SCHEMA  1u

/* Status layout (little endian):
 *  0 schema u8 | 1 state u8 | 2 ready u8 | 3 stage u8 | 4 generation u32 |
 *  8 timeoutSeconds u16 | 10 clients u8 | 11 failedStage u8 | 12 inhibitors u32 |
 * 16 completed u16 | 18 failed u16 | 20 skipped u16 | 22 reserved u16 |
 * 24 idleRemainingMs u32 */
size_t standby_api_pack_status(const StandbyStatus *st, uint8_t out[STANDBY_STATUS_LEN]);

/* Negative return = -CmdError (cmd_errors.h), otherwise the reply length. `src` is the
 * transport the request really arrived on (decided by the caller, never by the payload). */
int standby_api_bbp(StandbySystem *s, const uint8_t *payload, size_t len, uint8_t *resp,
                    size_t *resp_len, StandbySource src);

/* Map a coordinator result onto a CmdError (0 for OK). */
int standby_api_rc_to_cmd_error(StandbyRc rc);

/* Opcodes served from caches / retained logic only: never counted, never wake. */
int standby_api_bbp_passive(uint8_t opcode);

/* ---- HTTP ---- */
typedef enum {
    STANDBY_HTTP_WORK = 0,      /* counted by the barrier; wakes a sleeping device */
    STANDBY_HTTP_PASSIVE,       /* cached / static, served in any state, not activity */
    STANDBY_HTTP_STANDBY,       /* the standby routes: manage their own admission */
} StandbyHttpClass;

StandbyHttpClass standby_api_http_class(int is_get, const char *uri);

const char *standby_api_state_name(uint8_t state);
size_t standby_api_json_status(const StandbyStatus *st, char *out, size_t cap);

/* Strict body parsers. Return NULL on success or the exact error string. */
const char *standby_api_parse_presence(const char *body, size_t len, uint32_t *client_id,
                                       int *present);
const char *standby_api_parse_policy(const char *body, size_t len, uint32_t *seconds);

#ifdef __cplusplus
}
#endif
