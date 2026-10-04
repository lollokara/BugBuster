"""Spec 2026-10-03 section 6, run backfill: decode the P4's run files like the iOS app does.
hub_runs.c is pure C; run it on the host with hand-built file bytes."""
from tests.firmware_host.fwhost import compile_and_run

HUB = "Firmware/ESP32/src/hub"
SRC = [f"{HUB}/{n}" for n in ("hub_runs.c", "hub_json.c", "hub_log.c")]

MAIN = r"""
#include <stdio.h>
#include <string.h>
#include "hub_runs.h"

static void w16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void w32(uint8_t *p, uint32_t v) { w16(p, (uint16_t)v); w16(p + 2, (uint16_t)(v >> 16)); }
static void w64(uint8_t *p, int64_t v) { w32(p, (uint32_t)v); w32(p + 4, (uint32_t)((uint64_t)v >> 32)); }

static void v2(uint8_t *p, uint32_t t, uint16_t dt, uint16_t v, int32_t imin, int32_t imax, int64_t q) {
    memset(p, 0, 48);
    w32(p, t); w16(p + 4, v); w16(p + 6, (uint16_t)(v - 12)); w16(p + 8, (uint16_t)(v + 13)); w16(p + 10, 8725);
    w32(p + 12, (uint32_t)imin); w32(p + 16, (uint32_t)imax); w16(p + 22, dt); w64(p + 24, q);
}

int main(void) {
    uint8_t meta[68]; memset(meta, 0, sizeof meta);
    w32(meta, 0x4E525342u); w16(meta + 4, 2); w16(meta + 6, 7); w32(meta + 8, 1791000000u);
    memcpy(meta + 12, "soak \"A\"", 8);
    uint8_t *p = meta + 36; p[0] = 0; p[1] = 2; p[2] = 1; w32(p + 8, 2000); w16(p + 12, 1000); w16(p + 14, 3000);
    w32(p + 16, 50000); w16(p + 20, 1050); w16(p + 22, 150); w32(p + 24, 1500);
    hub_meta_t m; char buf[600]; hub_jw_t w;
    int pok = hub_meta_parse(meta, sizeof meta, &m);
    printf("meta %d id=%u ep=%u\n", pok, m.run_id, m.created_epoch);
    hub_jw_init(&w, buf, sizeof buf);
    int jok = hub_meta_json(&w, &m, "stopped", 1791003600);
    printf("json %d %s\n", jok, buf);
    hub_jw_init(&w, buf, sizeof buf);
    hub_meta_json(&w, &m, "active", 0);
    printf("null %s\n", strstr(buf, "\"ended_at\":null") ? "yes" : "no");
    meta[0] = 'X';
    int bad = hub_meta_parse(meta, sizeof meta, &m), shrt = hub_meta_parse(meta, 10, &m);
    printf("badmagic %d short %d\n", bad, shrt);

    uint8_t ev[48]; memset(ev, 0, sizeof ev);
    w32(ev, 1); w16(ev + 4, 1); w32(ev + 16, 10); w16(ev + 20, 2); w32(ev + 32, 30); w16(ev + 36, 4);
    uint32_t t = 0; const char *st = hub_events_final_state(ev, sizeof ev, &t);
    printf("ev1 %s %u\n", st, t);
    w16(ev + 36, 5);
    st = hub_events_final_state(ev, sizeof ev, &t); printf("ev2 %s %u\n", st, t);
    w16(ev + 36, 3);
    st = hub_events_final_state(ev, sizeof ev, &t); printf("ev3 %s\n", st);
    const char *e4 = hub_events_final_state(ev, 16, &t), *e5 = hub_events_final_state(ev, 0, &t);
    printf("ev4 %s ev5 %s\n", e4 ? "some" : "none", e5 ? "some" : "none");

    uint8_t ck[352]; memset(ck, 0, sizeof ck);
    printf("q15 short=%u", hub_q15_res(ck, 10)); printf(" nomagic=%u", hub_q15_res(ck, sizeof ck));
    w32(ck, 0x4B435342u); w32(ck + 336, 600); printf(" ok=%u", hub_q15_res(ck, sizeof ck));
    w32(ck + 336, 0); printf(" zero=%u", hub_q15_res(ck, sizeof ck));
    w32(ck + 336, 99999); printf(" big=%u\n", hub_q15_res(ck, sizeof ck));

    uint16_t res = 0; int rank = 0; char out[160]; out[0] = 0;
    int ids[] = { 0, 1, 2, 3, 4, 5, 0xFF, 0x100, 0x105 };
    for (unsigned k = 0; k < sizeof ids / sizeof *ids; k++) {
        int ok = hub_tier_of((uint16_t)ids[k], 600, &res, &rank);
        char one[24];
        if (ok) snprintf(one, sizeof one, " %d:%u/%d", ids[k], res, rank); else snprintf(one, sizeof one, " %d:-", ids[k]);
        strcat(out, one);
    }
    printf("tiers%s\n", out);

    printf("count v2=%zu v1=%zu bad=%zu size=%zu\n", hub_rec_count(2, 100), hub_rec_count(1, 100), hub_rec_count(3, 100), hub_rec_size(2));
    uint8_t a[48], b[48], c[48]; hub_rec_t ra, rb, rc; hub_sample_t s;
    v2(a, 60, 60, 3912, 5000000, 15000000, 600000000LL);
    v2(b, 120, 60, 3900, 5000000, 15000000, 1800000000LL);
    v2(c, 300, 60, 3880, 1000000, 3000000, 2400000000LL);
    hub_rec_decode(2, a, &ra); hub_rec_decode(2, b, &rb); hub_rec_decode(2, c, &rc);
    hub_rec_to_sample(&ra, NULL, 2, 1791000000u + ra.t, 60, &s);
    printf("a ts=%u v=%.3f i=%.4f soc=%.2f ext=%d vmin=%.3f vmax=%.3f\n", s.ts, (double)s.v, (double)s.i, (double)s.soc, s.ext, (double)s.vmin, (double)s.vmax);
    hub_rec_to_sample(&rb, &ra, 2, 1791000000u + rb.t, 60, &s); printf("b i=%.4f\n", (double)s.i);
    hub_rec_to_sample(&rc, &rb, 2, 1791000000u + rc.t, 60, &s); printf("c i=%.4f\n", (double)s.i);
    hub_rec_to_sample(&ra, NULL, 2, 1791000000u + ra.t, 1, &s); printf("s1 ext=%d\n", s.ext);

    uint8_t v1[32]; memset(v1, 0, sizeof v1); w32(v1, 900); w16(v1 + 4, 3700); w16(v1 + 10, 5000); w32(v1 + 12, 20000000);
    hub_rec_t r1; hub_rec_decode(1, v1, &r1); hub_rec_to_sample(&r1, NULL, 1, 1791000000u + r1.t, 900, &s);
    printf("v1 ts=%u i=%.4f\n", s.ts, (double)s.i);

    hub_range_t rg[2] = { { 100, 200, 1 }, { 200, 800, 60 } };
    printf("cov %d%d%d%d%d%d%d\n", hub_covered(rg, 2, 150, 1), hub_covered(rg, 2, 150, 60), hub_covered(rg, 2, 250, 1),
           hub_covered(rg, 2, 250, 60), hub_covered(rg, 2, 100, 1), hub_covered(rg, 2, 200, 1), hub_covered(rg, 2, 801, 60));

    /* wall clock mapping: run time only advances while ACTIVE */
    uint8_t wev[64]; memset(wev, 0, sizeof wev);
    w32(wev, 0);    w16(wev + 4, 2); w32(wev + 12, 1791000000u);     /* START at run time 0 */
    w32(wev + 16, 600);  w16(wev + 20, 3);                             /* PAUSE */
    w32(wev + 32, 600);  w16(wev + 36, 2); w32(wev + 44, 1791003600u); /* START again 3000 s later */
    w32(wev + 48, 1200); w16(wev + 52, 4);                             /* STOP */
    hub_wallmap_t wm; hub_wallmap_init(&wm, 1790999000u);
    hub_wallmap_add_events(&wm, wev, 32); hub_wallmap_add_events(&wm, wev + 32, 32);   /* any chunking */
    printf("wall n=%u t300=%u t600=%u t601=%u t1200=%u\n", wm.n, hub_wallmap_unix(&wm, 300), hub_wallmap_unix(&wm, 600),
           hub_wallmap_unix(&wm, 601), hub_wallmap_unix(&wm, 1200));
    hub_wallmap_init(&wm, 1790999000u);
    printf("wall0 %u\n", hub_wallmap_unix(&wm, 50));
    w32(wev + 12, 0);                                                  /* first START had no wall clock */
    hub_wallmap_add_events(&wm, wev, 64);
    printf("wall1 n=%u t50=%u t700=%u\n", wm.n, hub_wallmap_unix(&wm, 50), hub_wallmap_unix(&wm, 700));
    w32(wev + 12, 1791000000u);
    hub_wallmap_init(&wm, 1790999000u); hub_wallmap_add_events(&wm, wev, 64);
    printf("rt %u %u %u\n", hub_wallmap_run_time(&wm, 1791000300u), hub_wallmap_run_time(&wm, 1791003601u),
           hub_wallmap_run_time(&wm, 1790999500u));
    return 0;
}
"""


def _run():
    return compile_and_run(MAIN, sources=SRC, include_dirs=[HUB]).splitlines()


def test_meta_json_matches_the_hub_contract():
    out = _run()
    assert out[0] == "meta 1 id=7 ep=1791000000"
    assert out[1] == ('json 1 {"name":"soak \\"A\\"","state":"stopped","ended_at":1791003600,"params":{"chem":"LiPo",'
                      '"cells":2,"capacity_mah":2000,"start_soc_pct":100.0,"cutoff_mv_cell":3000,"rint_uohm_cell":50000,'
                      '"peukert":1.050,"sd_enable":true,"sd_pct_month":1.50,"ext_enable":false,"ext_load_ua":1500,"dither":false}}')
    assert out[2] == "null yes"
    assert out[3] == "badmagic 0 short 0"


def test_final_state_comes_from_the_last_lifecycle_event():
    out = _run()
    assert out[4] == "ev1 stopped 30"
    assert out[5] == "ev2 depleted 30"
    assert out[6] == "ev3 paused"
    assert out[7] == "ev4 none ev5 none"                           # only a 'created' event / empty file: no final state


def test_tiers_and_q15_interval():
    out = _run()
    assert out[8] == "q15 short=900 nomagic=900 ok=600 zero=900 big=3600"
    assert out[9] == "tiers 0:- 1:- 2:- 3:600/2 4:1/0 5:- 255:- 256:60/1 261:60/1"


def test_a_torn_last_record_is_never_decoded():
    out = _run()
    assert out[10] == "count v2=2 v1=3 bad=0 size=48"             # 100 B = 2 whole v2 records + 4 stray bytes


def test_average_current_follows_the_ios_rules():
    out = _run()
    assert out[11] == "a ts=1791000060 v=3.912 i=0.0100 soc=87.25 ext=1 vmin=3.900 vmax=3.925"
    assert out[12] == "b i=0.0200"                                 # contiguous: (1.8 C - 0.6 C) / 60 s
    assert out[13] == "c i=0.0020"                                 # gap before it: (imin + imax) / 2
    assert out[14] == "s1 ext=0"                                   # 1 s rows are sent as the legacy 6-element row
    assert out[15] == "v1 ts=1791000900 i=0.0200"


def test_coverage_rule():
    out = _run()
    assert out[16] == "cov 1101010"      # (150,1) (150,60) (250,1) (250,60) (100,1) (200,1) (801,60)


def test_wall_clock_mapping_survives_pauses():
    out = _run()
    assert out[17] == "wall n=2 t300=1791000300 t600=1791000600 t601=1791003601 t1200=1791004200"
    assert out[18] == "wall0 1790999050"                          # no events: created_epoch + t
    assert out[19] == "wall1 n=1 t50=1790999050 t700=1791003700"  # a START without a wall clock is ignored


def test_unix_time_maps_back_to_run_time():
    out = _run()
    assert out[20] == "rt 300 601 500"        # inside segment 1, inside segment 2, before any START (created_epoch + t)


MAIN_CLK = r"""
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include "hub_runs.h"

static void w16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void w32(uint8_t *p, uint32_t v) { w16(p, (uint16_t)v); w16(p + 2, (uint16_t)(v >> 16)); }

int main(void) {
    hub_wallmap_t wm;
    hub_wallmap_init(&wm, 1790999000u);
    uint32_t unc = 0;
    hub_clk_src_t src = hub_wallmap_src(&wm, 50, &unc);
    printf("init_src=%s unc=%u\n", hub_clk_src_name(src), unc);

    hub_wallmap_init(&wm, 0);
    src = hub_wallmap_src(&wm, 50, &unc);
    printf("zero_src=%s unc=%u\n", hub_clk_src_name(src), unc);

    hub_wallmap_set_created_clock(&wm, HUB_CLK_HUB, 45);
    src = hub_wallmap_src(&wm, 50, &unc);
    printf("custom_src=%s unc=%u\n", hub_clk_src_name(src), unc);

    uint8_t wev[32]; memset(wev, 0, sizeof wev);
    w32(wev, 100); w16(wev + 4, 2); w32(wev + 12, 1791000100u);
    hub_wallmap_add_events(&wm, wev, 16);
    src = hub_wallmap_src(&wm, 200, &unc);
    printf("seg_src=%s unc=%u\n", hub_clk_src_name(src), unc);

    hub_range_t rg[2] = {
        { 100, 200, 1, HUB_CLK_P4_EPOCH, 200 },
        { 200, 800, 60, HUB_CLK_HUB, 50 },
    };
    // At ts=150, range has P4_EPOCH:
    // Better clock (HUB): not covered
    int cov_hub = hub_covered_src(rg, 2, 150, 1, HUB_CLK_HUB);
    // Same clock (P4_EPOCH): covered
    int cov_p4 = hub_covered_src(rg, 2, 150, 1, HUB_CLK_P4_EPOCH);
    // Worse clock (EST): covered
    int cov_est = hub_covered_src(rg, 2, 150, 1, HUB_CLK_EST);
    printf("cov_cmp %d %d %d\n", cov_hub, cov_p4, cov_est);

    return 0;
}
"""


def test_clock_source_hierarchy_in_c():
    out = compile_and_run(MAIN_CLK, sources=SRC, include_dirs=[HUB]).splitlines()
    assert out[0] == "init_src=P4_EPOCH unc=200"
    assert out[1] == "zero_src=EST unc=3600000"
    assert out[2] == "custom_src=HUB unc=45"
    assert out[3] == "seg_src=P4_EPOCH unc=205"  # 200 + 100 * 50 / 1000 = 205
    assert out[4] == "cov_cmp 0 1 1"


MAIN_REANCHOR = r"""
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include "hub_runs.h"

int main(void) {
    // 1. Unsegmented run (n = 0) with created = 1000, EST
    hub_wallmap_t wm;
    hub_wallmap_init(&wm, 1000);
    wm.created_src = HUB_CLK_EST;
    wm.created_unc_ms = 3600000;

    hub_reanchor_req_t reqs[4];
    int n = hub_wallmap_compute_reanchor(&wm, 600, 1791000600u, HUB_CLK_HUB, 50, reqs, 4);
    printf("unseg n=%d from=%.0f to=%.0f delta=%.0f src=%s unc=%u\n",
           n, reqs[0].from_ts, reqs[0].to_ts, reqs[0].delta_s,
           hub_clk_src_name(reqs[0].clk_src), reqs[0].clk_unc_ms);

    char buf[256];
    hub_jw_t w;
    hub_jw_init(&w, buf, sizeof buf);
    hub_reanchor_json(&w, &reqs[0]);
    printf("json %s\n", buf);

    // Apply reanchor
    hub_wallmap_apply_reanchor(&wm, &reqs[0]);
    printf("applied created=%u src=%s unc=%u\n", wm.created, hub_clk_src_name(wm.created_src), wm.created_unc_ms);

    // After applying, recomputing should yield 0 requests (not worse)
    n = hub_wallmap_compute_reanchor(&wm, 600, 1791000600u, HUB_CLK_HUB, 50, reqs, 4);
    printf("noop n=%d\n", n);

    // 2. Multi-segment run:
    // seg[0]: t_s = 0, wall = 1000000, P4_EPOCH, unc = 200
    // seg[1]: t_s = 300, wall = 1000300, P4_EPOCH, unc = 215
    hub_wallmap_init(&wm, 1000000);
    wm.n = 2;
    wm.seg[0].t_s = 0; wm.seg[0].wall = 1000000; wm.seg[0].src = HUB_CLK_P4_EPOCH; wm.seg[0].unc_ms = 200;
    wm.seg[1].t_s = 300; wm.seg[1].wall = 1000300; wm.seg[1].src = HUB_CLK_P4_EPOCH; wm.seg[1].unc_ms = 215;

    // At run_time_s = 500, ref_wall = 1791000500:
    // dur in seg 1 is 500 - 300 = 200.
    // true_wall_start of seg 1 = 1791000500 - 200 = 1791000300.
    // delta_s = 1791000300 - 1000300 = 1790000000.
    n = hub_wallmap_compute_reanchor(&wm, 500, 1791000500u, HUB_CLK_HUB, 40, reqs, 4);
    printf("multiseg n=%d delta0=%.0f delta1=%.0f\n", n, reqs[0].delta_s, reqs[1].delta_s);

    hub_wallmap_apply_reanchor(&wm, &reqs[0]);
    hub_wallmap_apply_reanchor(&wm, &reqs[1]);
    printf("applied_multi seg0=%u seg1=%u\n", wm.seg[0].wall, wm.seg[1].wall);

    return 0;
}
"""


def test_reanchor_computation_and_json():
    out = compile_and_run(MAIN_REANCHOR, sources=SRC, include_dirs=[HUB]).splitlines()
    assert out[0] == "unseg n=1 from=1000 to=1600 delta=1790999000 src=HUB unc=50"
    assert out[1] == 'json {"from_ts":1000,"to_ts":1600,"delta_s":1790999000,"clk_src":"HUB","clk_unc_ms":50}'
    assert out[2] == "applied created=1791000000 src=HUB unc=50"
    assert out[3] == "noop n=0"
    assert out[4] == "multiseg n=2 delta0=1790000000 delta1=1790000000"
    assert out[5] == "applied_multi seg0=1791000000 seg1=1791000300"

