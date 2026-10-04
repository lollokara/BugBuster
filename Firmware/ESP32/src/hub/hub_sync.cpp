// =============================================================================
// hub_sync.cpp - see hub_sync.h.
// =============================================================================

#include "hub_sync.h"

#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "hat.h"
#include "hub_json.h"
#include "hub_policy.h"
#include "hub_runs.h"

#define MAX_RUNS        64
#define MAX_FILES       64
#define MAX_RANGES      64
#define LIST_PERIOD_MS  300000u
#define BATCH_RECORDS   96u

static const char *TAG = "hub_sync";
static const char *const STATE_NAME[] = { "no_run", "paused", "active", "depleted", "stopped" };

typedef struct {
    uint16_t id;
    uint32_t created;           // 0 = the P4 clock was not set when the run was created
    uint32_t sig;               // directory signature the last completed pass saw
    bool     meta_sent, done;
} run_t;

typedef struct { uint16_t id; uint32_t size; int rank; uint16_t res; } file_t;

typedef enum { PH_LIST, PH_META, PH_PICK, PH_STREAM } phase_t;

// ~5.5 KB of run/file/coverage tables: kept in PSRAM (file-scope, so the attribute takes effect).
typedef struct {
    phase_t  phase;
    uint32_t next_list_ms;
    bool     listed;
    uint16_t active_id;                // loaded run on the P4 (0 = none)
    run_t    runs[MAX_RUNS];
    int      n_runs, cur;
    uint16_t version;                  // record format of the current run
    uint32_t sig;
    file_t   files[MAX_FILES];
    int      n_files, fi;
    uint32_t off;
    hub_rec_t prev;
    bool     has_prev;
    hub_range_t cov[MAX_RANGES];
    int      n_cov;
    hub_meta_t meta;
    hub_wallmap_t wm;                  // run time -> unix time for the current run
    uint32_t hat_backoff;
    uint32_t retry_until;
} sync_state_t;

static EXT_RAM_BSS_ATTR sync_state_t S;

static uint8_t *s_raw;                  // BATCH_RECORDS * 48 B read buffer (PSRAM)

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

bool hub_sync_init(void)
{
    if (s_raw) return true;
    s_raw = (uint8_t *)heap_caps_malloc(BATCH_RECORDS * 48u, MALLOC_CAP_SPIRAM);
    return s_raw != NULL;
}

void hub_sync_reset(void)
{
    for (int k = 0; k < S.n_runs; k++) S.runs[k].done = S.runs[k].meta_sent = false;
    S.next_list_ms = 0;
    S.listed = false;
    S.phase = PH_LIST;
    S.hat_backoff = 0;
    S.retry_until = 0;
}

static void report_progress(void)
{
    uint16_t synced = 0;
    for (int k = 0; k < S.n_runs; k++) synced += S.runs[k].done ? 1 : 0;
    hub_status_set_runs((uint16_t)S.n_runs, synced);
}

// ---- P4 queries ----------------------------------------------------------------------------

// LIST_RUNS, paged: [1][u16 start] -> u16 total, u16 active, u16 ids[]
static bool p4_list(uint16_t *ids, int cap, int *n, uint16_t *active, bool *busy)
{
    if (busy) *busy = false;
    uint8_t rsp[240];
    *n = 0;
    for (;;) {
        const uint8_t a[2] = { (uint8_t)*n, (uint8_t)(*n >> 8) };
        int len = hub_bs(1, a, sizeof a, rsp, sizeof rsp);
        if (len == HAT_ERR_LOCK_BUSY) {
            if (busy) *busy = true;
            return false;
        }
        if (len < 4) return false;
        *active = rd16(rsp + 2);
        int page = (len - 4) / 2;
        for (int k = 0; k < page && *n < cap; k++) ids[(*n)++] = rd16(rsp + 4 + 2 * k);
        if (page == 0 || *n >= rd16(rsp) || *n >= cap) return true;
        vTaskDelay(pdMS_TO_TICKS(2));
    }
}

// RUN_DIR, paged: [2][u16 run][u8 start] -> u16 run, u8 total, u8 start, {u16 id, u32 size}[]
static bool p4_dir(uint16_t run, file_t *files, int cap, int *n, uint32_t *sig, uint16_t q15_res, bool *busy)
{
    if (busy) *busy = false;
    uint8_t rsp[240];
    int seen = 0;
    *n = 0;
    *sig = 0;
    for (;;) {
        const uint8_t a[3] = { (uint8_t)run, (uint8_t)(run >> 8), (uint8_t)seen };
        int len = hub_bs(2, a, sizeof a, rsp, sizeof rsp);
        if (len == HAT_ERR_LOCK_BUSY) {
            if (busy) *busy = true;
            return false;
        }
        if (len < 4) return false;
        int page = (len - 4) / 6;
        for (int k = 0; k < page; k++) {
            uint16_t id = rd16(rsp + 4 + 6 * k);
            uint32_t size = rd32(rsp + 6 + 6 * k);
            *sig = *sig * 31u + id + size;
            file_t f = { id, size, 0, 0 };
            if (*n < cap && hub_tier_of(id, q15_res, &f.res, &f.rank)) files[(*n)++] = f;
        }
        seen += page;
        if (page == 0 || seen >= rsp[2]) return true;
        vTaskDelay(pdMS_TO_TICKS(2));
    }
}

static uint16_t p4_q15_res(uint16_t run)
{
    uint8_t ck[352];
    size_t have = 0;
    while (have < sizeof ck) {
        uint8_t want = (uint8_t)(sizeof ck - have > 232 ? 232 : sizeof ck - have);
        int n = hub_bs_read(run, 1, (uint32_t)have, want, ck + have);
        if (n <= 0) break;
        have += (size_t)n;
        vTaskDelay(pdMS_TO_TICKS(2));
    }
    return hub_q15_res(ck, have);
}

// Final state of a stopped run from ev.bin (14 events per read).
static const char *p4_final_state(uint16_t run, uint32_t *t_s)
{
    uint8_t ev[224];
    const char *state = NULL;
    for (uint32_t off = 0, guard = 0; guard < 40; guard++) {
        int n = hub_bs_read(run, 2, off, sizeof ev, ev);
        if (n < 16) break;
        n -= n % 16;
        uint32_t t = 0;
        const char *s = hub_events_final_state(ev, (size_t)n, &t);
        if (s) { state = s; *t_s = t; }
        off += (uint32_t)n;
        vTaskDelay(pdMS_TO_TICKS(2));
    }
    return state;
}

// ---- hub queries ---------------------------------------------------------------------------

static hub_res_t post(const char *base, const char *path, const char *body, size_t len)
{
    size_t rcap;
    char *resp = hub_net_resp(&rcap);
    int st = 0;
    bool got = hub_http("POST", base, path, hub_device_id(), body, len, resp, rcap, &st);
    hub_res_t r = hub_classify_status(got, st);
    if (r == HUB_RES_OK) hub_status_note_push(true, (uint32_t)(hub_wall_ms() / 1000), NULL);
    else if (r == HUB_RES_RETRY) hub_status_note_push(false, 0, got ? "sync: hub error" : "hub unreachable");
    else ESP_LOGW(TAG, "hub refused %s: HTTP %d", path, st);
    return r;
}

static void fetch_coverage(const char *base, const char *uid)
{
    char path[96];
    snprintf(path, sizeof path, "/api/v1/runs/%s/coverage", uid);
    size_t rcap;
    char *resp = hub_net_resp(&rcap);
    int st = 0;
    S.n_cov = 0;                                           // an old hub (404) or a bad reply = nothing known: send all
    if (!hub_http("GET", base, path, hub_device_id(), NULL, 0, resp, rcap, &st) || st != 200) return;
    cJSON *root = cJSON_Parse(resp);
    if (!root) return;
    cJSON *it;
    cJSON_ArrayForEach(it, cJSON_GetObjectItem(root, "ranges")) {
        cJSON *f = cJSON_GetObjectItem(it, "from"), *t = cJSON_GetObjectItem(it, "to"), *r = cJSON_GetObjectItem(it, "res");
        if (S.n_cov >= MAX_RANGES || !cJSON_IsNumber(f) || !cJSON_IsNumber(t) || !cJSON_IsNumber(r)) continue;
        cJSON *cs = cJSON_GetObjectItem(it, "clk_src");
        cJSON *cu = cJSON_GetObjectItem(it, "clk_unc_ms");
        hub_clk_src_t csrc = HUB_CLK_EST;
        if (cJSON_IsString(cs)) {
            if (!strcmp(cs->valuestring, "HUB")) csrc = HUB_CLK_HUB;
            else if (!strcmp(cs->valuestring, "SNTP")) csrc = HUB_CLK_SNTP;
            else if (!strcmp(cs->valuestring, "P4_EPOCH")) csrc = HUB_CLK_P4_EPOCH;
            else csrc = HUB_CLK_EST;
        }
        uint32_t cunc = cJSON_IsNumber(cu) ? (uint32_t)cu->valuedouble : 3600000u;
        S.cov[S.n_cov++] = { f->valuedouble, t->valuedouble, (uint16_t)r->valueint, csrc, cunc };
    }
    cJSON_Delete(root);
}

// ---- phases --------------------------------------------------------------------------------

static hub_step_t phase_list(void)
{
    uint16_t ids[MAX_RUNS];
    int n = 0;
    uint16_t active = 0;
    bool busy = false;
    if (!p4_list(ids, MAX_RUNS, &n, &active, &busy)) {            // no DAQ HAT / busy: try again later
        if (busy) {
            S.hat_backoff = hub_hat_backoff_next(S.hat_backoff);
            S.retry_until = hub_uptime_ms() + S.hat_backoff;
            return HUB_STEP_IDLE;
        }
        S.hat_backoff = 0;
        S.next_list_ms = hub_uptime_ms() + 30000u;
        S.phase = PH_LIST;
        return HUB_STEP_IDLE;
    }
    S.hat_backoff = 0;
    run_t keep[MAX_RUNS];
    memcpy(keep, S.runs, sizeof keep);
    int nkeep = S.n_runs;
    S.n_runs = 0;
    bool unset_clock = false;
    for (int k = 0; k < n; k++) {
        hub_meta_t m;
        run_t r = { ids[k], 0, 0, false, false };
        if (hub_p4_meta(ids[k], &m)) r.created = m.created_epoch;
        if (r.created == 0) unset_clock = true;
        for (int j = 0; j < nkeep; j++) {                   // same run (id AND creation time): keep its progress
            if (keep[j].id == r.id && keep[j].created == r.created) { r = keep[j]; break; }
        }
        if (ids[k] == active) r.meta_sent = false;          // the loaded run's state changes: resend metadata
        S.runs[S.n_runs++] = r;
        vTaskDelay(pdMS_TO_TICKS(2));
    }
    S.active_id = active;
    if (unset_clock && hub_clock_valid()) hub_push_epoch_to_p4();   // the P4 dates undated runs on SET_EPOCH
    S.next_list_ms = hub_uptime_ms() + (unset_clock ? 20000u : LIST_PERIOD_MS);
    S.listed = true;
    S.cur = 0;
    S.phase = PH_META;
    report_progress();
    return HUB_STEP_MORE;
}

static hub_step_t phase_meta(const char *base)
{
    for (int k = 0; k < S.n_runs; k++) {
        run_t *r = &S.runs[k];
        if (r->meta_sent || r->created == 0) continue;
        hub_meta_t m;
        if (!hub_p4_meta(r->id, &m)) { r->meta_sent = true; continue; }
        const char *state = "stopped";
        int64_t ended = 0;
        if (r->id == S.active_id) {
            uint8_t st[96];
            int n = hub_bs(0, NULL, 0, st, sizeof st);
            if (n == HAT_ERR_LOCK_BUSY) {
                S.hat_backoff = hub_hat_backoff_next(S.hat_backoff);
                S.retry_until = hub_uptime_ms() + S.hat_backoff;
                return HUB_STEP_IDLE;
            }
            if (n >= 2 && st[1] < 5) state = STATE_NAME[st[1]];
        } else {
            uint32_t t = 0;
            const char *fs = p4_final_state(r->id, &t);
            if (fs && strcmp(fs, "active") != 0) state = fs;           // paused / stopped / depleted
            if (fs && (strcmp(fs, "stopped") == 0 || strcmp(fs, "depleted") == 0)) ended = (int64_t)r->created + t;
        }
        char uid[48], path[96];
        hub_run_uid(uid, sizeof uid, r->id, r->created);
        snprintf(path, sizeof path, "/api/v1/ingest/runs/%s", uid);
        char *body = hub_net_body();
        hub_jw_t w;
        hub_jw_init(&w, body, 1024);
        if (!hub_meta_json(&w, &m, state, ended)) { r->meta_sent = true; continue; }
        hub_res_t res = post(base, path, body, w.len);
        if (res == HUB_RES_RETRY) return HUB_STEP_RETRY;
        r->meta_sent = true;
        S.hat_backoff = 0;
        return HUB_STEP_MORE;
    }
    S.hat_backoff = 0;
    S.phase = PH_PICK;
    return HUB_STEP_MORE;
}

static hub_step_t phase_pick(const char *base)
{
    for (; S.cur < S.n_runs; S.cur++) {
        run_t *r = &S.runs[S.cur];
        if (r->created == 0) continue;
        hub_meta_t m;
        if (!hub_p4_meta(r->id, &m)) continue;
        uint16_t q15 = p4_q15_res(r->id);
        int n = 0;
        uint32_t sig = 0;
        bool busy = false;
        if (!p4_dir(r->id, S.files, MAX_FILES, &n, &sig, q15, &busy)) {
            if (busy) {
                S.hat_backoff = hub_hat_backoff_next(S.hat_backoff);
                S.retry_until = hub_uptime_ms() + S.hat_backoff;
                return HUB_STEP_IDLE;
            }
            return HUB_STEP_IDLE;
        }
        S.hat_backoff = 0;
        if (r->done && r->sig == sig) continue;             // nothing new on the P4 since the last full pass
        r->done = false;
        S.sig = sig;
        S.meta = m;
        S.version = m.version;
        // finest tier first, so the coarser tiers are diffed against what the finer ones just added
        for (int a = 1; a < n; a++)
            for (int b = a; b > 0 && (S.files[b].rank < S.files[b - 1].rank ||
                                      (S.files[b].rank == S.files[b - 1].rank && S.files[b].id < S.files[b - 1].id)); b--) {
                file_t t = S.files[b]; S.files[b] = S.files[b - 1]; S.files[b - 1] = t;
            }
        S.n_files = n;
        S.fi = 0;
        S.off = 0;
        S.has_prev = false;
        hub_p4_wallmap(r->id, r->created, &S.wm);
        char uid[48];
        hub_run_uid(uid, sizeof uid, r->id, r->created);
        fetch_coverage(base, uid);
        S.phase = PH_STREAM;
        return HUB_STEP_MORE;
    }
    S.hat_backoff = 0;
    S.phase = PH_LIST;
    report_progress();
    return HUB_STEP_IDLE;
}

static hub_step_t phase_stream(const char *base)
{
    run_t *r = &S.runs[S.cur];
    size_t rs = hub_rec_size(S.version);
    if (!rs) { r->done = true; r->sig = S.sig; S.cur++; S.phase = PH_PICK; return HUB_STEP_MORE; }   // unknown format
    if (S.fi >= S.n_files) {                                // all tiers sent
        r->done = true;
        r->sig = S.sig;
        S.cur++;
        S.phase = PH_PICK;
        report_progress();
        return HUB_STEP_MORE;
    }
    const file_t *f = &S.files[S.fi];
    size_t whole = hub_rec_count(S.version, f->size > S.off ? f->size - S.off : 0);   // a torn tail is never decoded
    if (whole == 0) {                                       // tier done: refresh coverage for the next one
        S.fi++;
        S.off = 0;
        S.has_prev = false;
        char uid[48];
        hub_run_uid(uid, sizeof uid, r->id, r->created);
        fetch_coverage(base, uid);
        return HUB_STEP_MORE;
    }
    if (whole > BATCH_RECORDS) whole = BATCH_RECORDS;
    size_t want = whole * rs, have = 0;
    const size_t chunk = (236 / rs) * rs;
    bool lock_busy = false;
    while (have < want) {
        uint8_t len = (uint8_t)(want - have < chunk ? want - have : chunk);
        int n = hub_bs_read(r->id, f->id, S.off + (uint32_t)have, len, s_raw + have);
        if (n == HAT_ERR_LOCK_BUSY) {
            lock_busy = true;
            break;
        }
        if (n <= 0) break;
        have += (size_t)n;
        vTaskDelay(pdMS_TO_TICKS(2));
    }
    size_t recs = have / rs;
    if (recs == 0) {
        if (lock_busy) {
            S.hat_backoff = hub_hat_backoff_next(S.hat_backoff);
            S.retry_until = hub_uptime_ms() + S.hat_backoff;
            return HUB_STEP_IDLE;
        }
        S.fi++; S.off = 0; S.has_prev = false; return HUB_STEP_MORE;   // file shrank or read failed: skip it
    }
    S.hat_backoff = 0;

    char *body = hub_net_body();
    hub_jw_t w;
    hub_jw_init(&w, body, HUB_BODY_CAP);
    hub_jw_raw(&w, "[");
    hub_rec_t cur, prev = S.prev;
    bool has_prev = S.has_prev;
    uint32_t rows = 0;
    hub_clk_src_t batch_src = HUB_CLK_EST;
    uint32_t batch_unc = 3600000u;
    for (size_t k = 0; k < recs; k++) {
        hub_rec_decode(S.version, s_raw + k * rs, &cur);
        hub_sample_t s;
        hub_rec_to_sample(&cur, has_prev ? &prev : NULL, S.version, hub_wallmap_unix(&S.wm, cur.t), f->res, &s);
        prev = cur;
        has_prev = true;
        s.clk_src = hub_wallmap_src(&S.wm, cur.t, &s.clk_unc_ms);
        batch_src = s.clk_src;
        batch_unc = s.clk_unc_ms;
        if (hub_covered(S.cov, (size_t)S.n_cov, s.ts, f->res) && hub_covered_src(S.cov, (size_t)S.n_cov, s.ts, f->res, s.clk_src)) continue;
        size_t mark = w.len;
        if (rows) hub_jw_raw(&w, ",");
        if (!hub_json_sample(&w, &s)) { w.len = mark; w.p[mark] = '\0'; w.over = false; break; }
        rows++;
    }
    hub_jw_raw(&w, "]");
    if (rows) {
        char uid[48], path[160];
        hub_run_uid(uid, sizeof uid, r->id, r->created);
        snprintf(path, sizeof path, "/api/v1/ingest/runs/%s/samples?res=%u" "&clk_src=%s&clk_unc_ms=%u",
                 uid, (unsigned)f->res, hub_clk_src_name(batch_src), (unsigned)batch_unc);
        if (post(base, path, body, w.len) == HUB_RES_RETRY) return HUB_STEP_RETRY;   // same batch next time
    }
    S.off += (uint32_t)(recs * rs);
    S.prev = prev;
    S.has_prev = has_prev;
    return HUB_STEP_MORE;
}

hub_step_t hub_sync_step(const char *base)
{
    if (!s_raw) return HUB_STEP_IDLE;
    if (S.retry_until && hub_uptime_ms() < S.retry_until) return HUB_STEP_IDLE;
    S.retry_until = 0;
    if (S.phase == PH_LIST) {
        if (S.listed && hub_uptime_ms() < S.next_list_ms) return HUB_STEP_IDLE;
        return phase_list();
    }
    if (S.phase == PH_META) return phase_meta(base);
    if (S.phase == PH_PICK) return phase_pick(base);
    return phase_stream(base);
}
