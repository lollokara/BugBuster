#pragma once

// =============================================================================
// hub_status.h - what the hub task reports through GET /api/hub/status, and the
// resync request flag POST /api/hub/resync raises. Thread-safe: the routes run on
// the HTTP / BLE workers, the writers on the hub task.
// =============================================================================

#include "hub_config.h"
#include "hub_policy.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool          ok;                       /* the last exchange with the hub succeeded */
    char          url[HUB_URL_MAX];
    hub_urlsrc_t  source;
    uint32_t      last_push;                /* unix s of the last accepted batch, 0 = never */
    uint32_t      backlog_logs;             /* log entries waiting in the ring */
    uint32_t      backlog_samples;          /* live samples waiting in the backlog */
    uint16_t      runs_known, runs_synced;  /* runs on the P4 / runs the hub has in full */
    char          last_error[48];
} hub_status_t;

void hub_status_get(hub_status_t *out);
void hub_status_set_conn(const char *url, hub_urlsrc_t src);
/** ok=true stamps last_push with unix_s and clears last_error; ok=false keeps last_push and records err. */
void hub_status_note_push(bool ok, uint32_t unix_s, const char *err);
void hub_status_set_backlog(uint32_t logs, uint32_t samples);
void hub_status_set_runs(uint16_t known, uint16_t synced);
void hub_resync_request(void);
bool hub_resync_take(void);

#ifdef __cplusplus
}
#endif
