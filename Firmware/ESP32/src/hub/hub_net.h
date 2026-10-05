#pragma once

// =============================================================================
// hub_net.h - everything the hub task needs to talk to the ESPFleet hub and to the
// P4: plain-HTTP helper, mDNS discovery, register (agent heartbeat), clock, BS ops.
// All calls block and are made from the single hub task. Spec 2026-10-03 section 6.
// =============================================================================

#include "hub_status.h"
#include "hub_runs.h"

#ifdef __cplusplus
extern "C" {
#endif

#define HUB_BODY_CAP (40u * 1024u)    /* shared PSRAM request body */
#define HUB_RESP_CAP (8u * 1024u)     /* shared PSRAM response buffer */

/** What one unit of work asks the scheduler to do next. */
typedef enum { HUB_STEP_IDLE = 0, HUB_STEP_MORE, HUB_STEP_RETRY } hub_step_t;

/** Allocate the shared PSRAM buffers once. False on out of memory. */
bool        hub_net_init(void);
char       *hub_net_body(void);                  /* HUB_BODY_CAP bytes */
char       *hub_net_resp(size_t *cap);           /* HUB_RESP_CAP bytes */

const char *hub_device_id(void);                 /* 12 lowercase hex: the STA MAC */
bool        hub_clock_valid(void);               /* wall clock >= 2020-01-01 */
void        hub_clock_source(hub_clk_src_t *src, uint32_t *unc_ms);
uint64_t    hub_wall_ms(void);
uint32_t    hub_uptime_ms(void);
/** "<device_id>-<run_id>-<created_epoch>" (the hub's run_uid). */
bool        hub_run_uid(char *out, size_t cap, uint16_t run_id, uint32_t created_epoch);

/** One blocking HTTP exchange (4 s timeout, plain HTTP). True if a response arrived, whatever its status. */
bool hub_http(const char *method, const char *base, const char *path, const char *device_id,
              const char *body, size_t body_len, char *resp, size_t resp_cap, int *status);
/** Hub exchanges that failed (no response or 5xx) since boot. */
uint32_t hub_http_fail_count(void);
/** Browse _espfleet._tcp for the hub (TXT api present, no id). Writes "http://a.b.c.d:port". */
bool hub_discover(char *url_out, size_t cap);
/** POST /api/v1/agent/heartbeat. Sets the wall clock from the reply when it is invalid and says so. */
bool hub_register(const char *base, bool *clock_was_set);
/** Stamp the P4 with the wall clock (BS_HOP_SET_EPOCH) so runs created before any host connected get a date. */
void hub_push_epoch_to_p4(void);

/** Battery-sim request [op][args] to the P4. Reply length, or <0 (see hat_bs_request). */
int  hub_bs(uint8_t op, const uint8_t *args, uint8_t nargs, uint8_t *rsp, uint16_t cap);
/** meta.bin of a run (false if unreadable). */
bool hub_p4_meta(uint16_t run, hub_meta_t *m);
/** Build the run-time -> unix-time map from the run's ev.bin START events. */
void hub_p4_wallmap(uint16_t run, uint32_t created, hub_wallmap_t *wm);
/** BS_HOP_READ: up to 236 bytes of a run file. Bytes read (0 = EOF) or <0. */
int  hub_bs_read(uint16_t run, uint16_t file, uint32_t off, uint8_t len, uint8_t *out);

#ifdef __cplusplus
}
#endif
