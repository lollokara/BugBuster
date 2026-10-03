#pragma once

// =============================================================================
// hub_sync.h - battery-run discovery and backfill (spec 2026-10-03 section 6,
// "Run auto-discovery and backfill"). Every run on the DAQ HAT gets its metadata
// upserted on the hub and its stored history sent at the best resolution
// available (s1 > minute > q15), skipping what the hub's coverage already has.
// Resumable and idempotent: a reboot or a hub outage just re-diffs coverage.
// =============================================================================

#include "hub_net.h"

#ifdef __cplusplus
extern "C" {
#endif

bool       hub_sync_init(void);
/** One unit of work (a run listing, one metadata POST, or one batch of <= 96 records). MORE = call again
 *  soon, RETRY = the hub failed (back off), IDLE = nothing to do until the next listing. */
hub_step_t hub_sync_step(const char *base);
/** Forget every "done" mark and relist (POST /api/hub/resync, hub reconnect, new hub). */
void       hub_sync_reset(void);

#ifdef __cplusplus
}
#endif
