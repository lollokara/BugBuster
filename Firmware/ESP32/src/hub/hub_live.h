#pragma once

// =============================================================================
// hub_live.h - live 1 Hz stream of the active battery run (spec 2026-10-03 section 6).
// Polls the P4's 1 s ring (BS_HOP_S1_SINCE) every 2 s into a >= 2 h PSRAM backlog and
// flushes it to the hub every 10 s (or 600 rows). A hub outage only grows the backlog;
// what the ring and backlog cannot cover is later backfilled from the minute files.
// =============================================================================

#include "hub_net.h"

#ifdef __cplusplus
extern "C" {
#endif

bool       hub_live_init(void);
/** Paces itself; call it every scheduler pass. RETRY = the hub failed (back off). */
hub_step_t hub_live_tick(const char *base);
uint32_t   hub_live_backlog(void);

#ifdef __cplusplus
}
#endif
