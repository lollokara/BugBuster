#pragma once

// =============================================================================
// hub_runs.h - decode the DAQ HAT's stored battery-run files into hub samples
// (spec 2026-10-03 section 6, "Run auto-discovery and backfill"). Mirrors the
// tier handling of iOSApp BattSimService.swift (BattSim.parseMeta/parseRecords/
// parseEvents). Pure C, host-tested by tests/firmware_host/test_hub_runs.py.
//
// File ids (battsim_store.h): 0 meta, 1 ckpt, 2 events, 3 q15, 4 s1, 0x100+N mN.
// =============================================================================

#include "hub_json.h"

#ifdef __cplusplus
extern "C" {
#endif

#define HUB_META_BYTES 68u

typedef struct {
    uint16_t run_id, version;
    uint32_t created_epoch;                     /* 0 = the P4 clock was never set */
    char     name[25];
    uint8_t  chem, cells;
    uint32_t capacity_mah, rint_uohm, ext_ua;
    uint16_t cutoff_mv_cell;
    bool     sd_enable, ext_enable, dither;
    float    start_soc_pct, peukert, sd_pct_month;
} hub_meta_t;

/** d = the first HUB_META_BYTES of meta.bin. False on short data or a bad magic. */
bool hub_meta_parse(const uint8_t *d, size_t n, hub_meta_t *m);
/** Body of POST /api/v1/ingest/runs/{uid}. ended_at <= 0 -> null. All-or-nothing on the writer. */
bool hub_meta_json(hub_jw_t *w, const hub_meta_t *m, const char *state, int64_t ended_at);
/** Final lifecycle state from ev.bin (16 B events: u32 t_s, u16 code, ...): "active" "paused" "stopped"
 *  "depleted", or NULL when the file has none. *t_s = that event's time. */
const char *hub_events_final_state(const uint8_t *ev, size_t n, uint32_t *t_s);
/** q15 interval from ckpt.bin (u32 @336, magic 'CKSB'), default 900, clamped to 1..3600. */
uint16_t hub_q15_res(const uint8_t *ckpt, size_t n);
/** Which tier a run file is: res seconds and rank (0 = s1 finest, 1 = minute, 2 = q15). False for meta/ckpt/events. */
bool hub_tier_of(uint16_t file_id, uint16_t q15_res, uint16_t *res, int *rank);

typedef struct {
    uint32_t t;                                  /* simulated s since run start, interval end */
    uint16_t dt;                                 /* seconds integrated (v2 only, else 0) */
    float    v, vmin, vmax, soc;                 /* V, V, V, percent */
    float    imin, imax;                         /* A */
    float    iavg_v1;                            /* A, version 1 files carry it */
    int64_t  q_dut_nc;                           /* nC, version 2 */
    bool     has_q;
} hub_rec_t;

size_t hub_rec_size(uint16_t version);                    /* 32 (v1), 48 (v2), 0 unknown */
size_t hub_rec_count(uint16_t version, size_t file_bytes);/* whole records only: a torn tail is ignored */
bool   hub_rec_decode(uint16_t version, const uint8_t *p, hub_rec_t *r);
/** prev = the previous record of the same file (or NULL). ts_unix = the interval end in wall time
 *  (hub_wallmap_unix). Average current as the iOS app derives it: contiguous v2 records -> dq/dt;
 *  first record -> q/dt; after a gap -> (imin+imax)/2. */
void   hub_rec_to_sample(const hub_rec_t *r, const hub_rec_t *prev, uint16_t version, uint32_t ts_unix,
                         uint16_t res, hub_sample_t *s);

/* Run time (record t, simulated seconds) only advances while the run is ACTIVE: every START re-bases
 * the wall clock (battsim.c), so created_epoch + t drifts after each pause. START events carry the wall
 * epoch in `b`; the map turns a run time into unix time segment by segment. */
#define HUB_WALL_SEGS 128u
typedef struct { uint32_t t_s, wall; } hub_wall_seg_t;
typedef struct { hub_wall_seg_t seg[HUB_WALL_SEGS]; uint16_t n; uint32_t created; } hub_wallmap_t;

void     hub_wallmap_init(hub_wallmap_t *m, uint32_t created_epoch);
/** Feed whole 16 B events of ev.bin in order (any chunking). START events with a wall epoch add a segment. */
void     hub_wallmap_add_events(hub_wallmap_t *m, const uint8_t *ev, size_t n);
/** Unix time of run time t_s (an interval END: the last segment that started strictly before it applies;
 *  before the first one, created_epoch + t_s). */
uint32_t hub_wallmap_unix(const hub_wallmap_t *m, uint32_t t_s);
/** Inverse: run time of a unix time that falls inside an ACTIVE segment (the S1 cursor for a resumed stream). */
uint32_t hub_wallmap_run_time(const hub_wallmap_t *m, uint32_t unix_s);

/** True iff some range has range.res <= res and range.from < ts <= range.to. */
bool   hub_covered(const hub_range_t *ranges, size_t n, uint32_t ts, uint16_t res);

#ifdef __cplusplus
}
#endif
