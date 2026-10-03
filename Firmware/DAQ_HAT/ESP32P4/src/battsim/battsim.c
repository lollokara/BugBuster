// =============================================================================
// battsim.c - battery simulator state machine, model loop and actuation.
//
// One task (10 Hz) owns all run state. Other tasks (S3 link, DDP, settings)
// call in under s_lock, a recursive mutex, so actions run synchronously and
// their result can be reported to the caller.
// =============================================================================

#include "battsim.h"
#include "battsim_model.h"
#include "battsim_integ.h"
#include "battsim_store.h"

#include <math.h>
#include <string.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"

#include "daq_board.h"
#include "config.h"

static const char *TAG = "battsim";

#define TICK_MS            100
#define TICKS_PER_S        BS_MCLK_HZ               // MCLK ticks per second
#define S1_RING            3600u                    // 1 h of 1 s records
#define STALL_US           (2LL * 1000000LL)
#define START_GRACE_US     (3LL * 1000000LL)
#define CUTOFF_TICKS       20                       // 2 s below cutoff
#define GAP_WIN            50                       // 5 s of deficit history
#define GAP_MIN_TICKS      (TICKS_PER_S / 500)      // ignore < 2 ms
#define LEARN_HOLD         3                        // ticks on one code before learning
#define NCODES             255
#define V_ON_MIN           0.5f                     // below this V_DUT the output is off

// One aggregation interval. q_pc / e_nj are measured DUT charge / energy,
// q_all_pc every drain source; voltage sums cover output-on time only.
typedef struct {
    int64_t  ticks, q_pc, e_nj, q_all_pc, v_ticks;
    double   v_uv_ticks;
    float    i_min, i_max, v_min, v_max;
    uint16_t flags;   // BS_RF_*
} agg_t;

static struct {
    daq_board_t      *b;
    SemaphoreHandle_t lock;
    battsim_err_t     err;

    bool              loaded;
    bs_run_meta_t     meta;
    bs_ckpt_t         ck;

    // Integrator bookkeeping.
    int64_t  last_q, last_e, last_ticks;
    uint32_t last_pushes;
    int64_t  last_push_us;
    int64_t  start_us;
    // Lost-sample (gap) fill: wall time vs integrated time.
    int64_t  wall_ref_us, meas_ticks_since_ref;
    int64_t  gap_hist[GAP_WIN];
    int64_t  gap_q_hist[GAP_WIN], gap_e_hist[GAP_WIN], gap_t_hist[GAP_WIN];
    int      gap_idx, gap_n;

    // Live readouts.
    float    i_tick, v_tick, v_target, i_1s, v_1s;
    uint32_t soc_ppm;
    int      below_cutoff;

    // Actuation.
    float    learn[NCODES];
    float    off;
    int8_t   code;
    bool     code_valid;
    int      hold;
    float    dither_acc;

    // History aggregation.
    agg_t    a1, a60, aq;
    int64_t  next_s1, next_m1, next_q;
    bs_hist_rec_v2_t *ring;
    uint32_t ring_head, ring_count;
    int64_t  win_cur_q, win_cur_ticks;
    int64_t  last_ckpt_us;

    uint32_t epoch, epoch_us_s;   // host wall clock at esp_timer seconds
    int64_t  created_us;          // esp_timer at RUN_NEW this boot, 0 = earlier boot
    bool     store_low;           // free space below BS_STORE_LOW_FREE at RUN_NEW
    bool     mirroring;           // battsim writing keys itself
    uint16_t profile_mask;        // cached bs_store_profile_mask()
} S;

static void lock(void)   { xSemaphoreTakeRecursive(S.lock, portMAX_DELAY); }
static void unlock(void) { xSemaphoreGiveRecursive(S.lock); }

// v1 runs (written before history v2) are loaded read-only.
static bool run_writable(void) { return S.loaded && S.meta.version >= 2; }

static uint32_t wall_now(void)
{
    if (!S.epoch) return 0;
    return S.epoch + (uint32_t)(esp_timer_get_time() / 1000000) - S.epoch_us_s;
}

static void agg_reset(agg_t *a)
{
    *a = (agg_t){ .i_min = INFINITY, .i_max = -INFINITY,
                  .v_min = INFINITY, .v_max = -INFINITY };
}

static void agg_add(agg_t *dst, const agg_t *src)
{
    dst->ticks += src->ticks;
    dst->q_pc += src->q_pc;
    dst->e_nj += src->e_nj;
    dst->q_all_pc += src->q_all_pc;
    dst->v_uv_ticks += src->v_uv_ticks;
    dst->v_ticks += src->v_ticks;
    dst->flags |= src->flags;
    if (src->i_min < dst->i_min) dst->i_min = src->i_min;
    if (src->i_max > dst->i_max) dst->i_max = src->i_max;
    if (src->v_min < dst->v_min) dst->v_min = src->v_min;
    if (src->v_max > dst->v_max) dst->v_max = src->v_max;
}

// Split a at `part` ticks into head + tail, proportionally; sums are conserved
// exactly, extremes and flags are copied to both halves.
static void agg_split(const agg_t *a, int64_t part, agg_t *head, agg_t *tail)
{
    *head = *a;
    *tail = *a;
    if (part >= a->ticks) { tail->ticks = tail->q_pc = tail->e_nj = tail->q_all_pc = tail->v_ticks = 0; tail->v_uv_ticks = 0.0; return; }
    if (part < 0) part = 0;
    double f = (double)part / (double)a->ticks;
    head->ticks = part;
    head->q_pc = llround((double)a->q_pc * f);
    head->e_nj = llround((double)a->e_nj * f);
    head->q_all_pc = llround((double)a->q_all_pc * f);
    head->v_ticks = llround((double)a->v_ticks * f);
    head->v_uv_ticks = a->v_uv_ticks * f;
    tail->ticks = a->ticks - head->ticks;
    tail->q_pc = a->q_pc - head->q_pc;
    tail->e_nj = a->e_nj - head->e_nj;
    tail->q_all_pc = a->q_all_pc - head->q_all_pc;
    tail->v_ticks = a->v_ticks - head->v_ticks;
    tail->v_uv_ticks = a->v_uv_ticks - head->v_uv_ticks;
}

static int64_t sim_ticks(void) { return S.ck.ticks; }

static int64_t q_used(void)
{
    return S.ck.q_init_pc + S.ck.q_dut_pc + S.ck.q_ext_pc + S.ck.q_sd_pc +
           S.ck.q_peuk_pc;
}

static uint32_t soc_ppm_of(int64_t used)
{
    int64_t cap = bs_capacity_pc(&S.meta.params);
    if (cap <= 0) return 0;
    double f = (double)(cap - used) / (double)cap;
    if (f <= 0.0) return 0;
    if (f >= 1.0) return 1000000u;
    return (uint32_t)(f * 1e6);
}

static uint32_t soc_ppm_now(void) { return soc_ppm_of(q_used()); }

static uint16_t clamp_u16(float x)
{
    if (!(x > 0.0f)) return 0;
    if (x > 65535.0f) return 65535;
    return (uint16_t)lroundf(x);
}

static int32_t clamp_i32(double x, uint16_t *flags)
{
    if (x > 2147483647.0) { *flags |= BS_RF_I_CLAMP; return INT32_MAX; }
    if (x < -2147483648.0) { *flags |= BS_RF_I_CLAMP; return INT32_MIN; }
    return (int32_t)llround(x);
}

// t_end in MCLK ticks; cq / ce / cu are the cumulative DUT charge (pC), DUT
// energy (nJ) and all-source charge (pC) at t_end.
static bs_hist_rec_v2_t make_rec(const agg_t *a, int64_t t_end, int64_t cq,
                                 int64_t ce, int64_t cu)
{
    bs_hist_rec_v2_t r = {0};
    r.t_s = (uint32_t)(t_end / TICKS_PER_S);
    r.flags = a->flags;
    float v_avg = a->v_ticks ? (float)(a->v_uv_ticks / (double)a->v_ticks * 1e-3) : 0.0f;
    r.v_avg_mv = clamp_u16(v_avg);
    r.v_min_mv = isfinite(a->v_min) ? clamp_u16(a->v_min * 1000.0f) : r.v_avg_mv;
    r.v_max_mv = isfinite(a->v_max) ? clamp_u16(a->v_max * 1000.0f) : r.v_avg_mv;
    r.soc_x100 = (uint16_t)(soc_ppm_of(cu) / 100u);
    uint16_t fl = a->flags;
    r.i_min_na = isfinite(a->i_min) ? clamp_i32((double)a->i_min * 1e9, &fl) : 0;
    r.i_max_na = isfinite(a->i_max) ? clamp_i32((double)a->i_max * 1e9, &fl) : 0;
    r.flags = fl;
    int64_t dt = (a->ticks + TICKS_PER_S / 2) / TICKS_PER_S;
    if (dt == 0 && a->ticks > 0) dt = 1;   // sub-second resume/pause fragment
    r.dt_s = (uint16_t)(dt > 65535 ? 65535 : dt);
    r.q_dut_nc = cq / 1000;
    r.q_used_nc = cu / 1000;
    r.e_dut_uj = ce / 1000;
    return r;
}

static void event(uint16_t code, int32_t a, int32_t b)
{
    if (!run_writable()) return;
    bs_event_t ev = { .t_s = (uint32_t)(sim_ticks() / TICKS_PER_S), .code = code,
                      .a = a, .b = b };
    bs_store_event(S.meta.run_id, &ev);
}

static void checkpoint(void)
{
    if (!run_writable()) return;
    if (!bs_store_ckpt_write(S.meta.run_id, &S.ck)) {
        ESP_LOGE(TAG, "checkpoint write failed (run %u)", S.meta.run_id);
    }
    S.last_ckpt_us = esp_timer_get_time();
}

static void dump_s1(void)
{
    if (!run_writable() || !S.ring || S.ring_count == 0) return;
    // Oldest first: the ring is contiguous when not yet wrapped.
    if (S.ring_count < S1_RING) {
        bs_store_s1_write(S.meta.run_id, S.ring, S.ring_count);
        return;
    }
    bs_hist_rec_v2_t *tmp = heap_caps_malloc(sizeof(bs_hist_rec_v2_t) * S1_RING, MALLOC_CAP_SPIRAM);
    if (!tmp) return;
    uint32_t first = S1_RING - S.ring_head;
    memcpy(tmp, &S.ring[S.ring_head], first * sizeof(bs_hist_rec_v2_t));
    memcpy(&tmp[first], S.ring, S.ring_head * sizeof(bs_hist_rec_v2_t));
    bs_store_s1_write(S.meta.run_id, tmp, S1_RING);
    heap_caps_free(tmp);
}

int battsim_s1_since(uint16_t run_id, uint32_t since_t_s, bs_s1_sample_t *out, int max, bool *more)
{
    if (more) *more = false;
    lock();
    int n = -1;
    if (S.loaded && S.meta.run_id == run_id && S.ring) {
        n = bs_s1_since(S.ring, S.ring_head, S.ring_count, S1_RING, since_t_s, out, max, more);
    }
    unlock();
    return n;
}

// ---------------------------------------------------------------------------
// Settings mirror.
// ---------------------------------------------------------------------------
static void set_key(uint16_t key, int32_t v)
{
    S.mirroring = true;
    daq_settings_set_i32(key, v, DAQ_SRC_BATTSIM);
    S.mirroring = false;
}

static void params_from_keys(bs_params_t *p)
{
    int32_t v = 0;
    *p = (bs_params_t){0};
    daq_settings_get_i32(DAQ_K_BS_CHEM, &v);         p->chem = (uint8_t)v;
    daq_settings_get_i32(DAQ_K_BS_CELLS, &v);        p->cells = (uint8_t)v;
    daq_settings_get_i32(DAQ_K_BS_CAPACITY_MAH, &v); p->capacity_mah = (uint32_t)v;
    daq_settings_get_i32(DAQ_K_BS_START_SOC, &v);    p->start_soc_x10 = (uint16_t)v;
    daq_settings_get_i32(DAQ_K_BS_CUTOFF_MV, &v);    p->cutoff_mv_cell = (uint16_t)v;
    daq_settings_get_i32(DAQ_K_BS_RINT_UOHM, &v);    p->rint_uohm_cell = (uint32_t)v;
    daq_settings_get_i32(DAQ_K_BS_PEUKERT, &v);      p->peukert_x1000 = (uint16_t)v;
    daq_settings_get_i32(DAQ_K_BS_SD_ENABLE, &v);    p->sd_enable = (uint8_t)(v != 0);
    daq_settings_get_i32(DAQ_K_BS_SD_PCT, &v);       p->sd_pct_month_x100 = (uint16_t)v;
    daq_settings_get_i32(DAQ_K_BS_EXT_ENABLE, &v);   p->ext_enable = (uint8_t)(v != 0);
    daq_settings_get_i32(DAQ_K_BS_EXT_UA, &v);       p->ext_load_ua = (uint32_t)v;
    daq_settings_get_i32(DAQ_K_BS_DITHER, &v);       p->dither = (uint8_t)(v != 0);
}

static void keys_from_params(const bs_params_t *p, const char *name)
{
    set_key(DAQ_K_BS_CHEM, p->chem);
    set_key(DAQ_K_BS_CELLS, p->cells);
    set_key(DAQ_K_BS_CAPACITY_MAH, (int32_t)p->capacity_mah);
    set_key(DAQ_K_BS_START_SOC, p->start_soc_x10);
    set_key(DAQ_K_BS_CUTOFF_MV, p->cutoff_mv_cell);
    set_key(DAQ_K_BS_RINT_UOHM, (int32_t)p->rint_uohm_cell);
    set_key(DAQ_K_BS_PEUKERT, p->peukert_x1000);
    set_key(DAQ_K_BS_SD_ENABLE, p->sd_enable);
    set_key(DAQ_K_BS_SD_PCT, p->sd_pct_month_x100);
    set_key(DAQ_K_BS_EXT_ENABLE, p->ext_enable);
    set_key(DAQ_K_BS_EXT_UA, (int32_t)p->ext_load_ua);
    set_key(DAQ_K_BS_DITHER, p->dither);
    if (name) {
        S.mirroring = true;
        daq_settings_set_str(DAQ_K_BS_NAME, name, DAQ_SRC_BATTSIM);
        S.mirroring = false;
    }
}

static void apply_chem_defaults(void)
{
    bs_params_t p;
    params_from_keys(&p);
    bs_params_t d;
    bs_params_defaults(&d, (bs_chem_t)p.chem, p.cells, p.capacity_mah);
    set_key(DAQ_K_BS_CUTOFF_MV, d.cutoff_mv_cell);
    set_key(DAQ_K_BS_RINT_UOHM, (int32_t)d.rint_uohm_cell);
    set_key(DAQ_K_BS_PEUKERT, d.peukert_x1000);
    set_key(DAQ_K_BS_SD_PCT, d.sd_pct_month_x100);
}

// ---------------------------------------------------------------------------
// Actuation: pick a DS4424 code for V_target from a learned code->V map.
// ---------------------------------------------------------------------------
static float pred_v(int code)
{
    float l = S.learn[code + 127];
    return isnan(l) ? smu_code_to_voltage((int8_t)code) + S.off : l;
}

static void learn_init(float target)
{
    for (int i = 0; i < NCODES; i++) S.learn[i] = NAN;
    S.off = 0.0f;
    // Seed the offset from the factory voltage cal when one exists.
    int8_t c;
    if (S.b->smu.cal && smu_cal_voltage_to_code(S.b->smu.cal, target, &c)) {
        S.off = target - smu_code_to_voltage(c);
    }
    S.code_valid = false;
    S.hold = 0;
    S.dither_acc = 0.0f;
}

static int8_t choose_code(float target)
{
    int best = 0;
    float best_e = INFINITY;
    for (int c = -127; c <= 127; c++) {
        float e = fabsf(pred_v(c) - target);
        if (e < best_e) { best_e = e; best = c; }
    }
    if (!S.meta.params.dither) {
        // Hysteresis: only move when clearly better (no hunting at a boundary).
        if (S.code_valid) {
            float cur_e = fabsf(pred_v(S.code) - target);
            float step = fabsf(pred_v(best) - pred_v(best + (best < 127 ? 1 : -1)));
            if (cur_e <= best_e + 0.15f * step) return S.code;
        }
        return (int8_t)best;
    }
    // First-order sigma-delta between the two codes bracketing the target.
    int lo = best, hi = best;
    for (int c = -127; c <= 127; c++) {
        float v = pred_v(c);
        if (v <= target && (pred_v(lo) > target || v > pred_v(lo))) lo = c;
        if (v > target && (pred_v(hi) <= target || v < pred_v(hi))) hi = c;
    }
    float step = fabsf(pred_v(hi) - pred_v(lo));
    if (S.code_valid) S.dither_acc += target - pred_v(S.code);
    float lim = 4.0f * (step > 0.0f ? step : 0.1f);
    if (S.dither_acc > lim) S.dither_acc = lim;
    if (S.dither_acc < -lim) S.dither_acc = -lim;
    return (int8_t)((S.dither_acc > 0.0f) ? hi : lo);
}

static void actuate(float target, bool force)
{
    int8_t c = choose_code(target);
    // smu_enable() re-ramps through the cal table, which can land on a
    // neighbouring code: re-post whenever the hardware is not where we put it.
    if (!force && S.code_valid && c == S.code && S.b->smu.v_code == c) {
        S.hold++;
        return;
    }
    if (daq_board_defer_bs_code(S.b, c, pred_v(c))) {
        S.code = c;
        S.code_valid = true;
        S.hold = 0;
    }
}

static void learn_update(float v_meas)
{
    if (!S.code_valid || S.hold < LEARN_HOLD || !(v_meas > 0.5f)) return;
    int i = S.code + 127;
    S.learn[i] = isnan(S.learn[i]) ? v_meas : S.learn[i] + 0.2f * (v_meas - S.learn[i]);
    float o = v_meas - smu_code_to_voltage(S.code);
    S.off += 0.05f * (o - S.off);
}

// ---------------------------------------------------------------------------
// History + remaining-time window.
//
// Model ticks are split exactly at whole-second boundaries, so s1 / m1 / q15
// records cover their nominal interval and carry the cumulative totals AT
// that boundary. A pause flushes partial records (dt_s < nominal), so the
// dt_s of a tier sums to the integrated time.
// ---------------------------------------------------------------------------
static int64_t q15_iv_ticks(void)
{
    if (S.ck.q15_interval_s == 0) S.ck.q15_interval_s = 900;
    return (int64_t)S.ck.q15_interval_s * TICKS_PER_S;
}

// Close s1, and m1 / q15 when due (or when flushing). Order on the minute:
// q15 append, checkpoint, m1 append - a power cut can then lose the newest
// m1 record but never leave history ahead of the checkpoint it resumes from.
static void close_tiers(int64_t t_end, int64_t cq, int64_t ce, int64_t cu, bool flush)
{
    if (S.a1.ticks > 0) {
        bs_hist_rec_v2_t r = make_rec(&S.a1, t_end, cq, ce, cu);
        if (S.ring) {
            S.ring[S.ring_head] = r;
            S.ring_head = (S.ring_head + 1) % S1_RING;
            if (S.ring_count < S1_RING) S.ring_count++;
        }
        S.i_1s = (float)((double)S.a1.q_pc / 1e12 / ((double)S.a1.ticks / (double)TICKS_PER_S));
        S.v_1s = (float)r.v_avg_mv * 1e-3f;
        agg_add(&S.a60, &S.a1);
    }
    agg_reset(&S.a1);
    if (!flush && t_end < S.next_m1) return;

    bool have_m1 = S.a60.ticks > 0;
    bs_hist_rec_v2_t m1 = {0};
    if (have_m1) {
        m1 = make_rec(&S.a60, t_end, cq, ce, cu);
        agg_add(&S.aq, &S.a60);
    }
    agg_reset(&S.a60);
    if (!flush) {
        S.next_m1 += 60LL * TICKS_PER_S;
        // Close the 1-min remaining-time bucket.
        S.ck.win_q_pc[S.ck.win_head] = S.win_cur_q;
        S.ck.win_head = (S.ck.win_head + 1) % 30u;
        if (S.ck.win_count < 30u) S.ck.win_count++;
        S.win_cur_q = 0;
        S.win_cur_ticks = 0;
    }
    if ((flush || t_end >= S.next_q) && S.aq.ticks > 0) {
        bs_hist_rec_v2_t q = make_rec(&S.aq, t_end, cq, ce, cu);
        if (!bs_store_q15_append(S.meta.run_id, &q, &S.ck)) ESP_LOGW(TAG, "q15 append failed");
        agg_reset(&S.aq);
    }
    if (!flush && t_end >= S.next_q) {
        int64_t iv = q15_iv_ticks();   // possibly doubled by compaction
        S.next_q = (t_end / iv + 1) * iv;
    }
    checkpoint();
    if (have_m1 && !bs_store_m1_append(S.meta.run_id, &m1)) ESP_LOGW(TAG, "m1 append failed");
}

static void history_absorb(const agg_t *part)
{
    agg_add(&S.a1, part);
    S.win_cur_q += part->q_all_pc;
    S.win_cur_ticks += part->ticks;
}

// tick: the interval just banked into S.ck (S.ck already includes it).
static void history_step(const agg_t *tick)
{
    int64_t t0 = sim_ticks() - tick->ticks;
    int64_t cq = S.ck.q_dut_pc - tick->q_pc;
    int64_t ce = S.ck.e_dut_nj - tick->e_nj;
    int64_t cu = q_used() - tick->q_all_pc;
    agg_t rem = *tick;
    while (rem.ticks > 0 && t0 + rem.ticks >= S.next_s1) {
        agg_t head, tail;
        agg_split(&rem, S.next_s1 - t0, &head, &tail);
        history_absorb(&head);
        cq += head.q_pc;
        ce += head.e_nj;
        cu += head.q_all_pc;
        t0 = S.next_s1;
        S.next_s1 += TICKS_PER_S;
        close_tiers(t0, cq, ce, cu, false);
        rem = tail;
    }
    if (rem.ticks > 0) history_absorb(&rem);
}

// Pause / stop: emit the partial s1, m1 and q15 records and checkpoint.
static void history_flush(void)
{
    close_tiers(sim_ticks(), S.ck.q_dut_pc, S.ck.e_dut_nj, q_used(), true);
}

static void reset_boundaries(void)
{
    int64_t t = sim_ticks();
    S.next_s1 = (t / TICKS_PER_S + 1) * TICKS_PER_S;
    S.next_m1 = (t / (60LL * TICKS_PER_S) + 1) * 60LL * TICKS_PER_S;
    int64_t iv = q15_iv_ticks();
    S.next_q = (t / iv + 1) * iv;
    agg_reset(&S.a1); agg_reset(&S.a60); agg_reset(&S.aq);
    S.win_cur_q = 0; S.win_cur_ticks = 0;
}

static bool remaining(uint32_t *out_s, float *i_avg, bool *provisional)
{
    int64_t q = S.win_cur_q, t = S.win_cur_ticks;
    for (uint32_t k = 0; k < S.ck.win_count; k++) {
        q += S.ck.win_q_pc[k];
        t += 60LL * TICKS_PER_S;
    }
    *provisional = S.ck.win_count < 30u;
    if (t <= 0) { *i_avg = 0.0f; return false; }
    double amps = (double)q / 1e12 / ((double)t / (double)TICKS_PER_S);
    *i_avg = (float)amps;
    if (!(amps > 0.0)) return false;
    int64_t rem = bs_capacity_pc(&S.meta.params) - q_used();
    if (rem <= 0) { *out_s = 0; return true; }
    double s = (double)rem / 1e12 / amps;
    *out_s = (s > 4.0e9) ? 0xFFFFFFFEu : (uint32_t)s;
    return true;
}

// ---------------------------------------------------------------------------
// State transitions (called with s_lock held).
// ---------------------------------------------------------------------------
static void integ_rebase(void)
{
    bs_integ_snap_t s;
    bs_integ_snapshot(&s);
    S.last_q = s.q_pc;
    S.last_e = s.e_nj;
    S.last_ticks = s.ticks;
    S.last_pushes = s.pushes;
    S.last_push_us = S.wall_ref_us = esp_timer_get_time();
    S.meas_ticks_since_ref = 0;
    S.gap_idx = S.gap_n = 0;
}

static void output(bool on)
{
    daq_settings_set_i32(DAQ_K_SOURCE_ENABLE, on ? 1 : 0, DAQ_SRC_BATTSIM);
}

static void tick_model(void);

static void enter_stopped(battsim_state_t st, uint16_t ev_code)
{
    bool was_active = S.ck.state == BS_ST_ACTIVE;
    if (was_active) tick_model();   // bank the last interval
    bs_integ_enable(false);
    output(false);
    S.ck.state = (uint8_t)st;
    event(ev_code, 0, (int32_t)wall_now());
    if (was_active) history_flush();   // partial records + checkpoint
    else checkpoint();
    dump_s1();
    ESP_LOGI(TAG, "run %u -> state %d (event %u)", S.meta.run_id, (int)st, ev_code);
}

static void unload_locked(void)
{
    if (!S.loaded) return;
    if (S.ck.state == BS_ST_ACTIVE) enter_stopped(BS_ST_PAUSED, BS_EV_PAUSE);
    else { dump_s1(); checkpoint(); }
    bs_store_active_clear();
    S.loaded = false;
    S.ring_head = S.ring_count = 0;
    memset(&S.ck, 0, sizeof(S.ck));
}

static bool load_locked(uint16_t id, bool at_boot)
{
    bs_run_meta_t m;
    bs_ckpt_t ck;
    if (!bs_store_meta_read(id, &m) || !bs_store_ckpt_read(id, &ck)) {
        S.err = BS_E_NOT_FOUND;
        return false;
    }
    unload_locked();
    S.meta = m;
    S.ck = ck;
    S.loaded = true;
    S.created_us = 0;
    S.ring_head = S.ring_count = 0;
    if (S.ck.state == BS_ST_ACTIVE) {
        // Power was lost mid-run. The gap is ignored; resume needs the user.
        S.ck.state = BS_ST_PAUSED;
        event(at_boot ? BS_EV_REBOOT : BS_EV_PAUSE, 0, (int32_t)wall_now());
        checkpoint();
    }
    if (!run_writable()) {
        ESP_LOGW(TAG, "run %u is format v%u: loaded read-only", id, S.meta.version);
    }
    reset_boundaries();
    S.soc_ppm = soc_ppm_now();
    S.v_target = bs_pack_voltage(&S.meta.params, S.soc_ppm, 0.0f);
    bs_store_active_set(id);
    keys_from_params(&S.meta.params, S.meta.name);
    set_key(DAQ_K_BS_RUN_SELECT, id);
    ESP_LOGI(TAG, "loaded run %u (state %u, SOC %.2f%%)", id, S.ck.state,
             S.soc_ppm / 1e4);
    return true;
}

static bool new_run_locked(void)
{
    bs_params_t p;
    params_from_keys(&p);
    if (bs_params_validate(&p, (uint32_t)(SMU_VDUT_MIN * 1000.0f + 40.0f),
                           (uint32_t)(SMU_VDUT_MAX * 1000.0f - 40.0f)) != BS_OK) {
        S.err = BS_E_INVALID;
        return false;
    }
    unload_locked();
    uint32_t fs_total = 0, fs_used = 0;
    bs_store_usage(&fs_total, &fs_used);
    S.store_low = fs_total - fs_used < BS_STORE_LOW_FREE;
    bs_run_meta_t m = {0};
    m.params = p;
    char name[DAQ_TLV_MAX_VAL + 1] = {0};
    daq_settings_get_str(DAQ_K_BS_NAME, name, sizeof(name));
    if (!name[0]) {
        const bs_chem_info_t *ci = bs_chem_info((bs_chem_t)p.chem);
        snprintf(name, sizeof(name), "%s %uS %lumAh", ci ? ci->name : "?",
                 (unsigned)p.cells, (unsigned long)p.capacity_mah);
    }
    strncpy(m.name, name, BS_NAME_LEN - 1);
    m.created_epoch = wall_now();
    if (!bs_store_run_create(&m)) { S.err = BS_E_IO; return false; }

    bs_ckpt_t ck = {0};
    int64_t cap = bs_capacity_pc(&p);
    ck.q_init_pc = cap - cap / 1000 * p.start_soc_x10;
    ck.state = BS_ST_PAUSED;
    ck.q15_interval_s = 900;
    S.meta = m;
    S.ck = ck;
    S.loaded = true;
    S.created_us = esp_timer_get_time();
    event(BS_EV_CREATED, p.start_soc_x10, (int32_t)p.capacity_mah);
    checkpoint();
    bs_store_active_set(m.run_id);
    reset_boundaries();
    S.ring_head = S.ring_count = 0;
    S.soc_ppm = soc_ppm_now();
    S.v_target = bs_pack_voltage(&p, S.soc_ppm, 0.0f);
    set_key(DAQ_K_BS_RUN_SELECT, m.run_id);
    ESP_LOGI(TAG, "new run %u: %s", m.run_id, m.name);
    return true;
}

static bool start_locked(void)
{
    if (!S.loaded) { S.err = BS_E_NO_RUN; return false; }
    if (S.ck.state == BS_ST_ACTIVE) return true;
    if (S.ck.state != BS_ST_PAUSED) { S.err = BS_E_STATE; return false; }
    if (!run_writable()) { S.err = BS_E_STATE; return false; }   // v1 run: read-only
    if (!S.b->fast_running) { S.err = BS_E_NO_ACQ; return false; }
    if (!daq_board_pd_ok(S.b, 9000, 3000)) { S.err = BS_E_NO_PD; return false; }

    S.soc_ppm = soc_ppm_now();
    S.v_target = bs_pack_voltage(&S.meta.params, S.soc_ppm, 0.0f);
    learn_init(S.v_target);
    actuate(S.v_target, true);            // ramp to the start code while off
    output(true);
    integ_rebase();
    bs_integ_enable(true);
    S.start_us = esp_timer_get_time();
    S.below_cutoff = 0;
    S.ck.state = BS_ST_ACTIVE;
    reset_boundaries();
    S.a1.flags |= BS_RF_RESUME;   // ORs up into the first m1 and q15 records
    event(BS_EV_START, (int32_t)S.soc_ppm, (int32_t)wall_now());
    checkpoint();
    return true;
}

// ---------------------------------------------------------------------------
// The 10 Hz model step (ACTIVE only).
// ---------------------------------------------------------------------------
static void tick_model(void)
{
    const bs_params_t *p = &S.meta.params;
    int64_t now = esp_timer_get_time();
    bs_integ_snap_t s;
    bs_integ_snapshot(&s);

    int64_t dq = s.q_pc - S.last_q;
    int64_t de = s.e_nj - S.last_e;
    int64_t dt = s.ticks - S.last_ticks;
    S.last_q = s.q_pc;
    S.last_e = s.e_nj;
    S.last_ticks = s.ticks;
    if (s.pushes != S.last_pushes) { S.last_pushes = s.pushes; S.last_push_us = now; }

    // Lost samples (DRDY edges missed during flash writes etc.): integrated
    // time falls behind wall time permanently, unlike ring backlog which
    // drains. Fill only the persistent part - the minimum deficit over 5 s.
    S.meas_ticks_since_ref += dt;
    int64_t wall_ticks = (now - S.wall_ref_us) * 2048 / 125;   // us -> MCLK ticks
    int64_t deficit = wall_ticks - S.meas_ticks_since_ref;
    S.gap_hist[S.gap_idx] = deficit;
    S.gap_q_hist[S.gap_idx] = dq;
    S.gap_e_hist[S.gap_idx] = de;
    S.gap_t_hist[S.gap_idx] = dt;
    S.gap_idx = (S.gap_idx + 1) % GAP_WIN;
    if (S.gap_n < GAP_WIN) S.gap_n++;
    int64_t fill = 0, fill_q = 0, fill_e = 0;
    if (S.gap_n == GAP_WIN) {
        int64_t mn = INT64_MAX, wq = 0, we = 0, wt = 0;
        for (int k = 0; k < GAP_WIN; k++) {
            if (S.gap_hist[k] < mn) mn = S.gap_hist[k];
            wq += S.gap_q_hist[k];
            we += S.gap_e_hist[k];
            wt += S.gap_t_hist[k];
        }
        if (mn > GAP_MIN_TICKS && wt > 0) {
            fill = mn;
            fill_q = (int64_t)llround((double)wq * (double)fill / (double)wt);
            S.ck.e_frac += (double)we * (double)fill / (double)wt;
            fill_e = (int64_t)floor(S.ck.e_frac);
            S.ck.e_frac -= (double)fill_e;
            S.meas_ticks_since_ref += fill;
            for (int k = 0; k < GAP_WIN; k++) S.gap_hist[k] -= fill;
        }
    }

    int64_t dt_tot = dt + fill;
    int64_t dq_dut = dq + fill_q;
    int64_t de_dut = de + fill_e;

    int64_t dq_ext = 0;
    if (p->ext_enable && p->ext_load_ua) {
        S.ck.ext_rem += (int64_t)p->ext_load_ua * 1000LL * dt_tot;
        dq_ext = S.ck.ext_rem >> 14;
        S.ck.ext_rem &= (BS_NA_TICKS_PER_PC - 1);
    }

    double dt_s = (double)dt_tot / (double)TICKS_PER_S;
    int64_t dq_peuk = 0;
    if (dt_tot > 0) {
        float i_load = (float)((double)(dq_dut + dq_ext) / 1e12 / dt_s);
        float f = bs_peukert_factor(p, i_load);
        if (f > 1.0f) {
            S.ck.peuk_frac += (double)(dq_dut + dq_ext) * (double)(f - 1.0f);
            dq_peuk = (int64_t)floor(S.ck.peuk_frac);
            S.ck.peuk_frac -= (double)dq_peuk;
        }
    }

    int64_t rem = bs_capacity_pc(p) - q_used();
    S.ck.sd_frac += bs_self_discharge_pc(p, rem, dt_s);
    int64_t dq_sd = (int64_t)floor(S.ck.sd_frac);
    S.ck.sd_frac -= (double)dq_sd;

    S.ck.q_dut_pc += dq_dut;
    S.ck.e_dut_nj += de_dut;
    S.ck.q_ext_pc += dq_ext;
    S.ck.q_peuk_pc += dq_peuk;
    S.ck.q_sd_pc += dq_sd;
    S.ck.ticks += dt_tot;

    // Live readouts.
    if (dt > 0) S.i_tick = (float)((double)dq / 1e12 / ((double)dt / (double)TICKS_PER_S));
    if (s.int_ticks > 0) S.v_tick = (float)((double)s.v_uv_ticks / (double)s.int_ticks * 1e-6);
    S.soc_ppm = soc_ppm_now();
    S.v_target = bs_pack_voltage(p, S.soc_ppm, S.i_tick);

    agg_t a;
    agg_reset(&a);
    a.ticks = dt_tot;
    a.q_pc = dq_dut;
    a.e_nj = de_dut;
    a.q_all_pc = dq_dut + dq_ext + dq_sd + dq_peuk;
    a.v_uv_ticks = (double)s.v_uv_ticks;
    a.v_ticks = s.int_ticks;
    a.i_min = s.i_min; a.i_max = s.i_max;
    a.v_min = s.v_min; a.v_max = s.v_max;
    if (fill) a.flags |= BS_RF_GAP;
    if (s.off_ticks > 0) a.flags |= BS_RF_OUTPUT_OFF;
    if (S.v_target < (float)p->cells * (float)p->cutoff_mv_cell * 1e-3f || S.soc_ppm == 0) {
        a.flags |= BS_RF_CUTOFF;
    }
    if (dt_tot > 0) history_step(&a);
}

static void tick_active(void)
{
    int64_t now = esp_timer_get_time();
    tick_model();
    const bs_params_t *p = &S.meta.params;

    if (now - S.last_push_us > STALL_US) {
        ESP_LOGW(TAG, "acquisition stalled - pausing run %u", S.meta.run_id);
        enter_stopped(BS_ST_PAUSED, BS_EV_STALL);
        return;
    }
    if (!S.b->smu.enabled && now - S.start_us > START_GRACE_US) {
        bool pd = daq_board_pd_ok(S.b, 9000, 3000);
        enter_stopped(BS_ST_PAUSED, pd ? BS_EV_OUTPUT_OFF : BS_EV_PD_LOST);
        return;
    }
    float v_cut = (float)p->cells * (float)p->cutoff_mv_cell * 1e-3f;
    if (S.soc_ppm == 0) S.below_cutoff = CUTOFF_TICKS;
    else if (S.v_target < v_cut) S.below_cutoff++;
    else S.below_cutoff = 0;
    if (S.below_cutoff >= CUTOFF_TICKS) {
        enter_stopped(BS_ST_DEPLETED, BS_EV_DEPLETED);
        return;
    }

    learn_update(S.v_tick);
    actuate(S.v_target, false);
    // Checkpoints ride the 1-min history boundary (close_tiers).
}

static void battsim_task(void *arg)
{
    (void)arg;
    TickType_t wake = xTaskGetTickCount();
    for (;;) {
        vTaskDelayUntil(&wake, pdMS_TO_TICKS(TICK_MS));
        lock();
        if (S.loaded && S.ck.state == BS_ST_ACTIVE) tick_active();
        unlock();
    }
}

// ---------------------------------------------------------------------------
// Public API.
// ---------------------------------------------------------------------------
void battsim_init(daq_board_t *b)
{
    S.b = b;
    S.lock = xSemaphoreCreateRecursiveMutex();
    S.ring = heap_caps_calloc(S1_RING, sizeof(bs_hist_rec_v2_t), MALLOC_CAP_SPIRAM);
    for (int i = 0; i < NCODES; i++) S.learn[i] = NAN;

    if (bs_store_init()) {
        S.profile_mask = bs_store_profile_mask();
        uint16_t id;
        if (bs_store_active_get(&id)) {
            lock();
            if (!load_locked(id, true)) bs_store_active_clear();
            unlock();
        }
    }
    // Core 1 is busy-polled by the prio-20 ADAQ capture task; a task pinned
    // there never runs. Core 0 next to daq_ui (daq_fast yields every 1024 loops).
    xTaskCreatePinnedToCore(battsim_task, "battsim", 6144, NULL, 5, NULL, 0);
}

battsim_state_t battsim_state(void) { return S.loaded ? (battsim_state_t)S.ck.state : BS_ST_NONE; }
battsim_err_t battsim_last_error(void) { return S.err; }
bool battsim_owns_supply(void) { return S.loaded; }
bool battsim_active(void) { return S.loaded && S.ck.state == BS_ST_ACTIVE; }
bool battsim_integrating(void) { return bs_integ_enabled(); }

void battsim_fast_push(float amps_mean, float volts_mean, float watts_mean,
                       uint32_t raw_periods)
{
    bool on = S.b && S.b->smu.enabled && volts_mean > V_ON_MIN;
    bs_integ_push(amps_mean, volts_mean, watts_mean, raw_periods, on);
}

void battsim_set_epoch(uint32_t unix_s)
{
    S.epoch = unix_s;
    S.epoch_us_s = (uint32_t)(esp_timer_get_time() / 1000000);
    if (!S.lock || !unix_s) return;
    lock();
    // Runs created before any host connected (C6 menu) get a creation date
    // on the first SET_EPOCH: exact if created this boot, else now - elapsed.
    if (run_writable() && S.meta.created_epoch == 0) {
        uint32_t back = S.created_us
            ? (uint32_t)((esp_timer_get_time() - S.created_us) / 1000000)
            : (uint32_t)(sim_ticks() / TICKS_PER_S);
        S.meta.created_epoch = unix_s > back ? unix_s - back : unix_s;
        bs_store_meta_write(&S.meta);
    }
    unlock();
}

void battsim_get_status(battsim_status_t *o)
{
    // Readers include the UI task: never block behind a flash write, serve
    // the last snapshot instead.
    static battsim_status_t s_cache = { .version = 2 };
    if (!S.lock || xSemaphoreTakeRecursive(S.lock, pdMS_TO_TICKS(20)) != pdTRUE) {
        *o = s_cache;
        return;
    }
    memset(o, 0, sizeof(*o));
    o->version = 2;
    o->state = (uint8_t)battsim_state();
    o->last_error = (uint8_t)S.err;
    if (S.store_low) o->flags |= BS_FLAG_STORE_LOW;
    if (bs_store_available()) {
        o->flags |= BS_FLAG_STORE_OK;
        o->profile_mask = S.profile_mask;
        uint32_t total = 0, used = 0;
        bs_store_usage(&total, &used);
        o->fs_total = total;
        o->fs_used = used;
    }
    if (S.b && S.b->smu.enabled) o->flags |= BS_FLAG_OUTPUT_ON;
    if (S.loaded) {
        const bs_params_t *p = &S.meta.params;
        o->run_id = S.meta.run_id;
        o->chem = p->chem;
        o->cells = p->cells;
        o->capacity_mah = p->capacity_mah;
        if (p->sd_enable) o->flags |= BS_FLAG_SD;
        if (p->ext_enable) o->flags |= BS_FLAG_EXT;
        if (p->dither) o->flags |= BS_FLAG_DITHER;
        o->soc_x100 = (uint16_t)(soc_ppm_now() / 100u);
        o->elapsed_s = (uint32_t)(sim_ticks() / TICKS_PER_S);
        bool prov = true;
        uint32_t rem_s = 0;
        float i_avg = 0.0f;
        if (remaining(&rem_s, &i_avg, &prov)) {
            o->remaining_s = rem_s;
            o->flags |= BS_FLAG_REMAIN_OK;
        }
        o->i_avg = i_avg;
        if (prov) o->flags |= BS_FLAG_PROVISIONAL;
        o->v_target = S.v_target;
        o->v_meas = (S.ck.state == BS_ST_ACTIVE) ? S.v_1s : 0.0f;
        o->i_meas = (S.ck.state == BS_ST_ACTIVE) ? S.i_1s : 0.0f;
        o->q_used_nc = q_used() / 1000;
        o->q_dut_nc = S.ck.q_dut_pc / 1000;
        o->q_ext_nc = S.ck.q_ext_pc / 1000;
        o->q_sd_nc = S.ck.q_sd_pc / 1000;
        o->q_peuk_nc = S.ck.q_peuk_pc / 1000;
        o->e_dut_uj = S.ck.e_dut_nj / 1000;
    }
    s_cache = *o;
    unlock();
}

static bool is_identity_key(uint16_t key)
{
    return key == DAQ_K_BS_CHEM || key == DAQ_K_BS_CELLS ||
           key == DAQ_K_BS_CAPACITY_MAH || key == DAQ_K_BS_START_SOC;
}

static bool is_live_key(uint16_t key)
{
    return key == DAQ_K_BS_CUTOFF_MV || key == DAQ_K_BS_RINT_UOHM ||
           key == DAQ_K_BS_PEUKERT || key == DAQ_K_BS_SD_ENABLE ||
           key == DAQ_K_BS_SD_PCT || key == DAQ_K_BS_EXT_ENABLE ||
           key == DAQ_K_BS_EXT_UA || key == DAQ_K_BS_DITHER;
}

static bool is_acq_key(uint16_t key)
{
    return key == DAQ_K_SAMPLE_RATE_IDX || key == DAQ_K_FILTER ||
           key == DAQ_K_DECIMATION || key == DAQ_K_REJECT_5060 ||
           key == DAQ_K_SR_MODE;
}

bool battsim_guard(uint16_t key, uint8_t action_id, int32_t ival, daq_src_t src)
{
    if (src == DAQ_SRC_BATTSIM || src == DAQ_SRC_BOOT || !S.lock) return true;
    if (action_id) {
        if (action_id == DAQ_ACT_FACTORY_RESET && S.loaded) {
            S.err = BS_E_BUSY;
            return false;
        }
        return true;
    }
    // Only CHANGES are vetoed: the C6 re-sends its whole settings set on every
    // edit, and an unchanged value must not be refused or acted on.
    int32_t cur = 0;
    if (key != DAQ_K_BS_NAME && daq_settings_get_i32(key, &cur) && cur == ival) return true;

    if (key == DAQ_K_SOURCE_ENABLE && S.loaded) {
        // The output belongs to the run: ON resumes it, OFF pauses it.
        battsim_action(ival ? DAQ_ACT_BS_RUN_START : DAQ_ACT_BS_RUN_PAUSE);
        return false;
    }
    if (key == DAQ_K_DUT_VOLTAGE_MV && S.loaded) { S.err = BS_E_BUSY; return false; }
    if (is_acq_key(key) && battsim_active())      { S.err = BS_E_BUSY; return false; }
    if (is_identity_key(key) && S.loaded)         { S.err = BS_E_BUSY; return false; }
    if (is_live_key(key) && S.loaded && !run_writable()) { S.err = BS_E_STATE; return false; }
    return true;
}

void battsim_on_setting(uint16_t key, int32_t ival, bool boot)
{
    if (boot || S.mirroring || !S.lock) return;
    lock();
    if (S.loaded) {
        // Live-editable parameters: update the run copy and log the change.
        bs_params_t *p = &S.meta.params;
        bool changed = true;
        switch (key) {
        case DAQ_K_BS_CUTOFF_MV:  p->cutoff_mv_cell = (uint16_t)ival; break;
        case DAQ_K_BS_RINT_UOHM:  p->rint_uohm_cell = (uint32_t)ival; break;
        case DAQ_K_BS_PEUKERT:    p->peukert_x1000 = (uint16_t)ival; break;
        case DAQ_K_BS_SD_ENABLE:  p->sd_enable = (uint8_t)(ival != 0); break;
        case DAQ_K_BS_SD_PCT:     p->sd_pct_month_x100 = (uint16_t)ival; break;
        case DAQ_K_BS_EXT_ENABLE: p->ext_enable = (uint8_t)(ival != 0); break;
        case DAQ_K_BS_EXT_UA:     p->ext_load_ua = (uint32_t)ival; break;
        case DAQ_K_BS_DITHER:     p->dither = (uint8_t)(ival != 0); S.dither_acc = 0; break;
        default: changed = false; break;
        }
        if (changed && run_writable()) {
            if (S.ck.state == BS_ST_ACTIVE) tick_model();   // old params up to now
            bs_store_meta_write(&S.meta);
            event(BS_EV_PARAM, key, ival);
        }
    } else if (key == DAQ_K_BS_CHEM) {
        apply_chem_defaults();
    } else if (key == DAQ_K_BS_CAPACITY_MAH) {
        int32_t chem = 0;
        daq_settings_get_i32(DAQ_K_BS_CHEM, &chem);
        set_key(DAQ_K_BS_RINT_UOHM, (int32_t)bs_default_rint_uohm((bs_chem_t)chem, (uint32_t)ival));
    }
    unlock();
}

bool battsim_load_run(uint16_t run_id)
{
    if (!S.lock) return false;
    lock();
    bool ok;
    if (!bs_store_available()) { S.err = BS_E_NO_STORE; ok = false; }
    else if (battsim_active()) { S.err = BS_E_BUSY; ok = false; }
    else ok = load_locked(run_id, false);
    unlock();
    return ok;
}

bool battsim_delete_run(uint16_t run_id)
{
    if (!S.lock) return false;
    lock();
    bool ok = !(S.loaded && S.meta.run_id == run_id) && bs_store_delete_run(run_id);
    if (!ok) S.err = (S.loaded && S.meta.run_id == run_id) ? BS_E_BUSY : BS_E_NOT_FOUND;
    if (ok && S.store_low) {
        uint32_t t = 0, u = 0;
        bs_store_usage(&t, &u);
        S.store_low = t - u < BS_STORE_LOW_FREE;
    }
    unlock();
    return ok;
}

bool battsim_action(uint8_t action_id)
{
    if (!S.lock) return false;
    lock();
    S.err = BS_E_NONE;
    bool ok = false;
    int32_t slot = 0, sel = 0;
    daq_settings_get_i32(DAQ_K_BS_PROFILE_SLOT, &slot);
    daq_settings_get_i32(DAQ_K_BS_RUN_SELECT, &sel);

    if (!bs_store_available() && action_id != DAQ_ACT_BS_DEFAULTS) {
        S.err = BS_E_NO_STORE;
        unlock();
        return false;
    }
    switch (action_id) {
    case DAQ_ACT_BS_PROFILE_SAVE: {
        bs_params_t p;
        params_from_keys(&p);
        char name[DAQ_TLV_MAX_VAL + 1] = {0};
        daq_settings_get_str(DAQ_K_BS_NAME, name, sizeof(name));
        ok = bs_store_profile_save((uint8_t)slot, name, &p);
        if (ok) S.profile_mask |= (uint16_t)(1u << slot);
        else S.err = BS_E_IO;
        break;
    }
    case DAQ_ACT_BS_PROFILE_LOAD: {
        if (S.loaded) { S.err = BS_E_BUSY; break; }
        bs_params_t p;
        char name[BS_NAME_LEN];
        ok = bs_store_profile_load((uint8_t)slot, name, &p);
        if (ok) keys_from_params(&p, name);
        else S.err = BS_E_NOT_FOUND;
        break;
    }
    case DAQ_ACT_BS_PROFILE_DELETE:
        ok = bs_store_profile_delete((uint8_t)slot);
        if (ok) S.profile_mask &= (uint16_t)~(1u << slot);
        else S.err = BS_E_NOT_FOUND;
        break;
    case DAQ_ACT_BS_RUN_NEW:
        if (battsim_active()) { S.err = BS_E_BUSY; break; }
        ok = new_run_locked();
        break;
    case DAQ_ACT_BS_RUN_START:
        ok = start_locked();
        break;
    case DAQ_ACT_BS_RUN_PAUSE:
        if (!S.loaded) { S.err = BS_E_NO_RUN; break; }
        if (S.ck.state == BS_ST_ACTIVE) enter_stopped(BS_ST_PAUSED, BS_EV_PAUSE);
        ok = true;
        break;
    case DAQ_ACT_BS_RUN_STOP:
        if (!S.loaded) { S.err = BS_E_NO_RUN; break; }
        if (S.ck.state == BS_ST_ACTIVE || S.ck.state == BS_ST_PAUSED) {
            enter_stopped(BS_ST_STOPPED, BS_EV_STOP);
        }
        ok = true;
        break;
    case DAQ_ACT_BS_RUN_UNLOAD:
        unload_locked();
        ok = true;
        break;
    case DAQ_ACT_BS_RUN_LOAD:
        if (battsim_active()) { S.err = BS_E_BUSY; break; }
        ok = load_locked((uint16_t)sel, false);
        break;
    case DAQ_ACT_BS_RUN_DELETE:
        ok = battsim_delete_run((uint16_t)sel);
        break;
    case DAQ_ACT_BS_DEFAULTS:
        if (S.loaded) { S.err = BS_E_BUSY; break; }
        apply_chem_defaults();
        ok = true;
        break;
    default:
        unlock();
        return false;
    }
    unlock();
    return ok;
}
