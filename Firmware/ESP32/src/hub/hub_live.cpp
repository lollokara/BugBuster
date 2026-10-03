// =============================================================================
// hub_live.cpp - see hub_live.h.
// =============================================================================

#include "hub_live.h"

#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "esp_attr.h"
#include "esp_heap_caps.h"

#include "hub_json.h"
#include "hub_policy.h"
#include "hub_runs.h"

#define BACKLOG_ROWS 7200u          /* >= 2 h at 1 Hz (spec) */
#define FLUSH_ROWS   600u
#define POLL_MS      2000u
#define FLUSH_MS     10000u
#define MAX_S1_CALLS 4

typedef struct {
    bool     active;
    uint16_t run_id;
    uint32_t created;
    char     uid[48];
    uint32_t cursor;                 /* run time (s) of the newest sample already queued */
    uint32_t next_poll, next_flush, retry_until, backoff;
    int8_t   state;
    hub_wallmap_t wm;
} live_t;

static EXT_RAM_BSS_ATTR live_t L;
static hub_sample_t *s_q;            /* circular backlog, PSRAM */
static uint32_t s_head, s_count;

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

bool hub_live_init(void)
{
    if (s_q) return true;
    s_q = (hub_sample_t *)heap_caps_malloc(BACKLOG_ROWS * sizeof(hub_sample_t), MALLOC_CAP_SPIRAM);
    return s_q != NULL;
}

uint32_t hub_live_backlog(void) { return s_count; }

static void queue(const hub_sample_t *s)
{
    if (s_count == BACKLOG_ROWS) { s_head = (s_head + 1) % BACKLOG_ROWS; s_count--; }   /* oldest falls out; backfill covers it */
    s_q[(s_head + s_count) % BACKLOG_ROWS] = *s;
    s_count++;
}

static void pop(uint32_t n)
{
    if (n > s_count) n = s_count;
    s_head = (s_head + n) % BACKLOG_ROWS;
    s_count -= n;
}

// A run just became active: learn its identity, its wall-clock map and where the hub's 1 s data ends.
static bool begin(const char *base, uint16_t run_id)
{
    hub_meta_t m;
    if (!hub_p4_meta(run_id, &m) || m.created_epoch == 0 || !hub_clock_valid()) return false;
    if (L.active && L.run_id != run_id) { pop(s_count); }    /* the old run's rows are backfilled from its files */
    L.run_id = run_id;
    L.created = m.created_epoch;
    hub_run_uid(L.uid, sizeof L.uid, run_id, m.created_epoch);
    hub_p4_wallmap(run_id, m.created_epoch, &L.wm);
    L.cursor = 0;
    char path[96];
    snprintf(path, sizeof path, "/api/v1/runs/%s/coverage", L.uid);
    size_t rcap;
    char *resp = hub_net_resp(&rcap);
    int st = 0;
    if (hub_http("GET", base, path, hub_device_id(), NULL, 0, resp, rcap, &st) && st == 200) {
        cJSON *root = cJSON_Parse(resp);
        double last = 0;
        cJSON *it;
        cJSON_ArrayForEach(it, cJSON_GetObjectItem(root, "ranges")) {
            cJSON *to = cJSON_GetObjectItem(it, "to"), *res = cJSON_GetObjectItem(it, "res");
            if (cJSON_IsNumber(to) && cJSON_IsNumber(res) && res->valueint == 1 && to->valuedouble > last) last = to->valuedouble;
        }
        cJSON_Delete(root);
        if (last > 0) L.cursor = hub_wallmap_run_time(&L.wm, (uint32_t)last);   /* resume where the hub stopped */
    }
    L.active = true;
    return true;
}

static void drain_s1(void)
{
    uint8_t rsp[240];
    for (int call = 0; call < MAX_S1_CALLS; call++) {
        const uint8_t a[7] = { (uint8_t)L.run_id, (uint8_t)(L.run_id >> 8), (uint8_t)L.cursor, (uint8_t)(L.cursor >> 8),
                               (uint8_t)(L.cursor >> 16), (uint8_t)(L.cursor >> 24), 14 };
        int len = hub_bs(6 /* BS_HOP_S1_SINCE */, a, sizeof a, rsp, sizeof rsp);
        if (len < 4) return;
        int n = rsp[2];
        if (4 + n * 16 > len) return;
        for (int k = 0; k < n; k++) {
            const uint8_t *p = rsp + 4 + 16 * k;           /* u32 t_s, u16 v_mv, u16 soc_x100, i32 i_ua, u16 flags, u16 dt */
            uint32_t t = rd32(p);
            hub_sample_t s = {};
            s.ts = hub_wallmap_unix(&L.wm, t);
            s.v = rd16(p + 4) / 1000.0f;
            s.soc = rd16(p + 6) / 100.0f;
            s.i = (float)((int32_t)rd32(p + 8) * 1e-6);
            s.state = L.state;
            queue(&s);
            L.cursor = t;
        }
        if (!rsp[3]) return;                                /* no more newer records */
    }
}

static hub_step_t flush(const char *base)
{
    char *body = hub_net_body();
    hub_jw_t w;
    hub_jw_init(&w, body, HUB_BODY_CAP);
    hub_jw_raw(&w, "[");
    uint32_t n = 0;
    for (; n < s_count && n < FLUSH_ROWS; n++) {
        size_t mark = w.len;
        if (n) hub_jw_raw(&w, ",");
        if (!hub_json_sample(&w, &s_q[(s_head + n) % BACKLOG_ROWS])) { w.len = mark; w.p[mark] = '\0'; w.over = false; break; }
    }
    hub_jw_raw(&w, "]");
    char path[96];
    snprintf(path, sizeof path, "/api/v1/ingest/runs/%s/samples", L.uid);
    size_t rcap;
    char *resp = hub_net_resp(&rcap);
    int st = 0;
    bool got = hub_http("POST", base, path, hub_device_id(), body, w.len, resp, rcap, &st);
    hub_res_t r = hub_classify_status(got, st);
    uint32_t now = hub_uptime_ms();
    if (r == HUB_RES_RETRY) {
        L.backoff = hub_backoff_next(L.backoff);
        L.retry_until = now + L.backoff;
        hub_status_note_push(false, 0, got ? "live: hub error" : "hub unreachable");
        return HUB_STEP_RETRY;
    }
    L.backoff = 0;
    pop(n);                                                 /* accepted, or a 4xx that can never succeed */
    if (r == HUB_RES_OK) hub_status_note_push(true, (uint32_t)(hub_wall_ms() / 1000), NULL);
    L.next_flush = now + FLUSH_MS;
    return s_count >= FLUSH_ROWS ? HUB_STEP_MORE : HUB_STEP_IDLE;
}

hub_step_t hub_live_tick(const char *base)
{
    if (!s_q) return HUB_STEP_IDLE;
    uint32_t now = hub_uptime_ms();
    if (now < L.retry_until) return HUB_STEP_IDLE;
    if (now >= L.next_poll) {
        L.next_poll = now + POLL_MS;
        uint8_t st[96];
        int n = hub_bs(0 /* BS_HOP_STATUS */, NULL, 0, st, sizeof st);
        if (n < 8) { L.active = false; }
        else if (st[1] == 2) {                              /* BS_ST_ACTIVE */
            uint16_t id = rd16(st + 4);
            L.state = 2;
            if ((!L.active || L.run_id != id) && !begin(base, id)) return HUB_STEP_IDLE;
            drain_s1();
        } else if (L.active) {
            L.active = false;                               /* paused/stopped: flush what is queued, then rest */
            L.next_flush = 0;
        }
    }
    if (s_count && L.uid[0] && (s_count >= FLUSH_ROWS || now >= L.next_flush)) return flush(base);
    return HUB_STEP_IDLE;
}
