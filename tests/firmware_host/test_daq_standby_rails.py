"""Executes the DAQ analog-rail sequencing (standby_rails_core.c) on the host.

The real C file is compiled; only the pins, delays and buses it drives are fakes
that record the order of every call. This proves the sequence and its failure
handling - NOT that the rails behave on the bench.
"""

from tests.firmware_host.fwhost import compile_and_run

P4 = "Firmware/DAQ_HAT/ESP32P4/src/standby"

MAIN = r"""
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "standby_rails_core.h"

static char trace[1024];
static int pg_level, pg_rise_after, pg_ms, pg_fall_after;   // fake TPS74601
static bool v3_en;
static bool fail_rail[3], fail_mux, fail_bypass, fail_gate, fail_reset, fail_bus, fail_restore, fail_pulse;
static bool fail_verify, fail_connect, mux_pad_high;
static int cancel_after_calls, cancel_calls;
static int pg_mode;   // 0 normal, 1 read error, 2 never rises, 3 never falls
static int waited_us;

static void rec(const char *s) { strcat(trace, s); strcat(trace, " "); }
static bool mux_disconnect(void *c) { (void)c; rec("mux_off"); if (!fail_mux) mux_pad_high = false; return !fail_mux; }
static bool bypass_park(void *c) { (void)c; rec("bypass"); return !fail_bypass; }
static bool mux_verify(void *c) { (void)c; rec("verify"); return !fail_verify && !mux_pad_high; }
static bool mux_connect(void *c) { (void)c; rec("mux_on"); if (fail_connect) return false; mux_pad_high = true; return true; }
static bool cancelled(void *c) { (void)c; return cancel_after_calls >= 0 && cancel_calls++ >= cancel_after_calls; }
static bool adc_gate(void *c, sb_gate_t m) { (void)c; rec(m == SB_GATE_CLOSED ? "gate_closed" : "gate_other"); return !fail_gate; }
static bool adaq_reset(void *c, bool a) { (void)c; rec(a ? "rst_low" : "rst_high"); return !fail_reset; }
static bool bus_release(void *c) { (void)c; rec("bus_hiz"); return !fail_bus; }
static bool bus_restore(void *c) { (void)c; rec("bus_on"); return !fail_restore; }
static bool rail_set(void *c, sb_rail_t r, bool on) {
    (void)c;
    static const char *n[] = { "24V", "26V", "3V3" };
    char b[32]; snprintf(b, sizeof b, "%s=%d", n[r], on); rec(b);
    if (fail_rail[r]) return false;
    if (r == SB_RAIL_3V3) { v3_en = on; pg_ms = 0; }
    return true;
}
static int pg_read(void *c) {
    (void)c;
    if (pg_mode == 1) return -1;
    if (v3_en) return pg_mode == 2 ? 0 : (pg_ms >= pg_rise_after);
    if (pg_mode == 3) return 1;
    return pg_ms >= pg_fall_after ? 0 : 1;
}
static bool adaq_pulse(void *c) { (void)c; rec("pulse"); return !fail_pulse; }
static void delay_us(void *c, uint32_t us) { (void)c; pg_ms += us / 1000; waited_us += us; }

static const sb_rails_ops_t ops = {
    NULL, mux_disconnect, bypass_park, mux_verify, mux_connect, adc_gate, adaq_reset, bus_release,
    bus_restore, rail_set, pg_read, adaq_pulse, delay_us, cancelled,
};

static void reset_env(void) {
    trace[0] = 0; pg_rise_after = 3; pg_fall_after = 4; pg_mode = 0; v3_en = true; pg_ms = 0; waited_us = 0;
    fail_mux = fail_bypass = fail_gate = fail_reset = fail_bus = fail_restore = fail_pulse = false;
    fail_verify = fail_connect = mux_pad_high = false; cancel_after_calls = -1; cancel_calls = 0;
    memset(fail_rail, 0, sizeof fail_rail);
}
// stage 3 then stage 5, the way the worker runs them
static bool cut_rails(sb_rails_t *r) { return sb_rails_mux_off(r, &ops) && sb_rails_off(r, &ops); }
// the analog rails come back with the muxes still disconnected: stage 8
static bool power_back(sb_rails_t *r) { return sb_rails_on(r, &ops); }
#define HAS(s) (strstr(trace, s) != NULL)
static int pos(const char *s) { const char *p = strstr(trace, s); return p ? (int)(p - trace) : -1; }

int main(void) {
    sb_rails_t r;

    // ---- stage 3 then 5: the muxes are disconnected and PROVEN before any rail moves ----
    reset_env(); sb_rails_init(&r, true); mux_pad_high = true;
    assert(sb_rails_mux_off(&r, &ops));
    assert(r.mux_open && r.mux_verified && !r.cut);
    assert(pos("mux_off") < pos("bypass") && pos("bypass") < pos("verify"));
    assert(!HAS("24V") && !HAS("26V") && !HAS("3V3") && !HAS("gate") && !HAS("rst"));   // no rail, no converter
    printf("mux off ok\n");

    // ---- off: strict order, PG proves the 3V3 rail fell -----------------------
    trace[0] = 0;
    assert(sb_rails_off(&r, &ops));
    assert(pos("verify") >= 0 && pos("verify") < pos("gate_closed"));      // the guard re-reads the pads
    assert(!HAS("mux_off") && !HAS("bypass"));                              // ... and does not redo stage 3
    assert(pos("verify") < pos("24V=0"));
    assert(pos("gate_closed") < pos("rst_low"));
    assert(pos("gate_closed") < pos("rst_low"));
    assert(pos("rst_low") < pos("bus_hiz"));
    assert(pos("bus_hiz") < pos("24V=0"));
    assert(pos("24V=0") < pos("26V=0"));
    assert(pos("26V=0") < pos("3V3=0"));
    assert(!r.powered && r.cut && !r.dirty && r.fail_mask == 0);
    assert(!HAS("=1"));
    printf("off order ok\n");

    // ---- off: PG read error is a failure, not a quiet "low" -----------------
    reset_env(); sb_rails_init(&r, true); pg_mode = 1;
    assert(sb_rails_mux_off(&r, &ops) && !sb_rails_off(&r, &ops));
    assert(r.fail_mask & SB_RF_PG_READ);
    assert(HAS("3V3=0") && r.dirty && !r.powered);
    printf("off pg read error ok\n");

    // ---- off: PG still high after EN low -> stuck ----------------------------
    reset_env(); sb_rails_init(&r, true); pg_mode = 3;
    assert(sb_rails_mux_off(&r, &ops) && !sb_rails_off(&r, &ops));
    assert(r.fail_mask & SB_RF_PG_STUCK);
    assert(waited_us >= (int)(SB_PG_OFF_MS * 1000u) - 1000);
    printf("off pg stuck ok\n");

    // ---- off: a failing rail does not stop the later ones --------------------
    reset_env(); sb_rails_init(&r, true); fail_rail[SB_RAIL_24V] = true;
    assert(sb_rails_mux_off(&r, &ops) && !sb_rails_off(&r, &ops));
    assert(r.fail_mask & SB_RF_RAIL_24V);
    assert(HAS("26V=0") && HAS("3V3=0") && HAS("rst_low") && HAS("bus_hiz"));
    printf("off best effort ok\n");

    // ---- GUARD: no rail, converter or pin moves unless the muxes are proven open ----
    reset_env(); sb_rails_init(&r, true);                          // stage 3 never ran
    assert(!sb_rails_off(&r, &ops));
    assert((r.fail_mask & SB_RF_MUX_GUARD) && !r.cut && !r.dirty && r.powered);
    assert(!HAS("24V") && !HAS("26V") && !HAS("3V3") && !HAS("gate") && !HAS("rst") && !HAS("bus"));
    reset_env(); sb_rails_init(&r, true); fail_mux = true;         // the disconnect itself failed
    assert(!sb_rails_mux_off(&r, &ops) && (r.fail_mask & SB_RF_MUX) && r.mux_open && !r.mux_verified);
    trace[0] = 0;
    assert(!sb_rails_off(&r, &ops) && (r.fail_mask & SB_RF_MUX_GUARD));
    assert(!HAS("24V") && !HAS("3V3") && !HAS("gate") && !r.cut);
    reset_env(); sb_rails_init(&r, true); fail_verify = true;      // readback disagrees with what was driven
    assert(!sb_rails_mux_off(&r, &ops) && (r.fail_mask & SB_RF_MUX_VERIFY) && !r.mux_verified);
    assert(!sb_rails_off(&r, &ops) && !HAS("24V") && !r.cut);
    reset_env(); sb_rails_init(&r, true); fail_bypass = true;
    assert(!sb_rails_mux_off(&r, &ops) && (r.fail_mask & SB_RF_BYPASS) && r.mux_open);
    assert(!sb_rails_off(&r, &ops) && !r.cut);
    // proven at stage 3 but a pad went high again before stage 5: re-read, refused
    reset_env(); sb_rails_init(&r, true);
    assert(sb_rails_mux_off(&r, &ops));
    mux_pad_high = true; trace[0] = 0;
    assert(!sb_rails_off(&r, &ops) && (r.fail_mask & SB_RF_MUX_GUARD) && !r.mux_verified);
    assert(!HAS("24V") && !HAS("3V3") && !HAS("gate") && !r.cut);
    mux_pad_high = false;
    assert(!sb_rails_off(&r, &ops));                               // stays refused until stage 3 is redone
    assert(sb_rails_mux_off(&r, &ops) && sb_rails_off(&r, &ops) && r.cut);
    printf("mux guard ok\n");

    // ---- on: 3V3 -> PG -> 26V -> 24V -> bus -> reset release ------------------
    reset_env(); sb_rails_init(&r, true);
    assert(cut_rails(&r)); trace[0] = 0; v3_en = false; pg_ms = 0;
    assert(sb_rails_on(&r, &ops));
    assert(pos("rst_low") < pos("3V3=1"));
    assert(pos("3V3=1") < pos("26V=1"));
    assert(pos("26V=1") < pos("24V=1"));
    assert(pos("24V=1") < pos("bus_on"));
    assert(pos("bus_on") < pos("pulse"));
    assert(!HAS("=0"));                           // nothing is switched off on the way up
    assert(!HAS("mux") && !HAS("verify") && !HAS("bypass"));   // a wake never touches the muxes
    assert(r.powered && !r.dirty && r.cut && r.mux_open);
    printf("on order ok\n");

    // ---- on: repeated stage is an idempotent no-op ---------------------------
    trace[0] = 0;
    assert(sb_rails_on(&r, &ops));
    assert(trace[0] == 0);
    printf("on idempotent ok\n");

    // ---- on: PG never rises -> deadline, everything back off ------------------
    reset_env(); sb_rails_init(&r, true); cut_rails(&r); trace[0] = 0; v3_en = false;
    pg_mode = 2; waited_us = 0;
    assert(!sb_rails_on(&r, &ops));
    assert(r.fail_mask & SB_RF_PG_TIMEOUT);
    assert(waited_us < 200000);                    // bounded
    assert(!HAS("26V=1") && !HAS("24V=1"));
    assert(HAS("3V3=0") && !r.powered && r.dirty);
    printf("on pg timeout ok\n");

    // ---- recovery after the partial failure resets everything first ----------
    pg_mode = 0; trace[0] = 0; v3_en = false;
    assert(sb_rails_on(&r, &ops));
    assert(pos("3V3=0") >= 0 && pos("3V3=0") < pos("3V3=1"));
    assert(pos("24V=0") < pos("3V3=1"));
    assert(r.powered && !r.dirty);
    printf("on recovery ok\n");

    // ---- on: 26V fails after 3V3 is up -> everything off again ----------------
    reset_env(); sb_rails_init(&r, true); cut_rails(&r); trace[0] = 0; v3_en = false;
    fail_rail[SB_RAIL_26V] = true;
    assert(!sb_rails_on(&r, &ops));
    assert(r.fail_mask & SB_RF_RAIL_26V);
    assert(pos("3V3=1") < pos("3V3=0") && !HAS("24V=1") && !HAS("pulse"));
    printf("on rail failure ok\n");

    // ---- on: read error / restore error / pulse error ------------------------
    reset_env(); sb_rails_init(&r, true); cut_rails(&r); v3_en = false; pg_mode = 1;
    assert(!sb_rails_on(&r, &ops) && (r.fail_mask & SB_RF_PG_READ) && !r.powered);
    reset_env(); sb_rails_init(&r, true); cut_rails(&r); v3_en = false; fail_restore = true;
    assert(!sb_rails_on(&r, &ops) && (r.fail_mask & SB_RF_BUS) && !r.powered && HAS("3V3=0"));
    reset_env(); sb_rails_init(&r, true); cut_rails(&r); v3_en = false; fail_pulse = true;
    assert(!sb_rails_on(&r, &ops) && (r.fail_mask & SB_RF_PULSE) && !r.powered);
    printf("on errors ok\n");

    // ---- a superseded power-on stops at the next step and leaves everything OFF ----
    for (int at = 0; at < 4; ++at) {
        reset_env(); sb_rails_init(&r, true); cut_rails(&r); trace[0] = 0; v3_en = false; pg_ms = 0;
        cancel_after_calls = at; cancel_calls = 0;
        assert(!sb_rails_on(&r, &ops));
        assert((r.fail_mask & SB_RF_CANCELLED) && !r.powered && r.dirty);
        assert(!HAS("bus_on") && !HAS("pulse") && !HAS("mux_on"));        // nothing after the cancel point
        assert(HAS("3V3=0") && HAS("24V=0") && HAS("26V=0"));               // ... and every rail is down
        cancel_after_calls = -1; trace[0] = 0; v3_en = false; pg_ms = 0;
        assert(sb_rails_on(&r, &ops) && r.powered && !r.dirty);            // the next generation starts clean
    }
    printf("on cancelled ok\n");

    // ---- routes: connected only on an explicit request, never by a wake ----------
    reset_env(); sb_rails_init(&r, true);
    assert(sb_rails_mux_connect(&r, &ops) && trace[0] == 0);              // never disconnected: nothing to do
    assert(cut_rails(&r)); v3_en = false; pg_ms = 0;
    trace[0] = 0;
    assert(!sb_rails_mux_connect(&r, &ops) && (r.fail_mask & SB_RF_NOT_POWERED) && !HAS("mux_on") && r.mux_open);
    assert(sb_rails_on(&r, &ops));
    assert(!sb_rails_mux_connect(&r, &ops) && !HAS("mux_on") && r.mux_open);   // converters not restored yet (cut)
    r.cut = false;                                                         // the glue clears it after stage 9
    trace[0] = 0;
    assert(sb_rails_mux_connect(&r, &ops) && HAS("mux_on") && !r.mux_open && !r.mux_verified);
    assert(sb_rails_mux_connect(&r, &ops));                                // repeat is a no-op
    // a connect that fails leaves the muxes disconnected and held
    assert(cut_rails(&r)); v3_en = false; pg_ms = 0; assert(sb_rails_on(&r, &ops)); r.cut = false;
    fail_connect = true; trace[0] = 0;
    assert(!sb_rails_mux_connect(&r, &ops) && (r.fail_mask & SB_RF_MUX) && r.mux_open);
    assert(HAS("mux_off") && HAS("bypass") && r.mux_verified);
    printf("routes ok\n");

    // ---- 20 sleep/wake cycles ends where it started ------------------------------
    reset_env(); sb_rails_init(&r, true);
    for (int i = 0; i < 20; ++i) {
        trace[0] = 0;
        assert(cut_rails(&r));
        v3_en = false; pg_ms = 0;
        assert(sb_rails_on(&r, &ops));
        assert(r.powered && !r.dirty && r.mux_open);
        r.cut = false;
        assert(sb_rails_mux_connect(&r, &ops) && !r.mux_open);
    }
    printf("20 cycles ok\n");

    // ---- a never-cut board: power-on is a no-op ---------------------------------
    reset_env(); sb_rails_init(&r, true);
    assert(sb_rails_on(&r, &ops) && trace[0] == 0 && !r.cut);
    printf("never cut ok\n");
    return 0;
}
"""


def test_rail_sequence_on_host():
    out = compile_and_run(MAIN, sources=[f"{P4}/standby_rails_core.c"], include_dirs=[P4])
    for line in ("mux off ok", "off order ok", "off pg read error ok", "off pg stuck ok", "off best effort ok",
                 "mux guard ok", "on order ok", "on idempotent ok", "on pg timeout ok", "on recovery ok",
                 "on rail failure ok", "on errors ok", "on cancelled ok", "routes ok", "20 cycles ok",
                 "never cut ok"):
        assert line in out
