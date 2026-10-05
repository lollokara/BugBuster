"""The S3 coordinator, the P4 standby core and the P4 hardware sequences, run together.

REAL files, one process: the S3 policy/coordinator (standby_policy.c, standby_system.c),
the P4 participant (standby_p4_core.c), the P4 inhibitor classifier and ctrl-queue
counters (standby_inhibit.c) and the analog rail sequencing (standby_rails_core.c). Only
the pins, the UART exchange and the ctrl task's queue are modelled; the queue follows the
discipline daq_board.c uses (post/take under one lock with the counters; user messages
need admit_quiet; standby stages do not).

What it proves: the mainboard does not cancel its own sleep because of the P4's standby
stages; a real user job still does; the P4 opens the analog muxes (readback) BEFORE the
mainboard cuts its local analog supply and before any P4 rail moves; a wake closes no
route and restarts nothing; an explicit request does. It does NOT prove pin behaviour on
the bench.
"""

from tests.firmware_host.fwhost import compile_and_run

POWER = "Firmware/ESP32/src/power"
P4_STANDBY = "Firmware/DAQ_HAT/ESP32P4/src/standby"

SOURCES = [
    f"{POWER}/standby_policy.c",
    f"{POWER}/standby_system.c",
    f"{P4_STANDBY}/standby_p4_core.c",
    f"{P4_STANDBY}/standby_inhibit.c",
    f"{P4_STANDBY}/standby_rails_core.c",
]

MAIN = r"""
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "standby_system.h"
#include "standby_p4_core.h"
#include "standby_inhibit.h"
#include "standby_rails_core.h"

/* ---------------- event log ---------------- */
static char trace[16384];
static void ev(const char *s) { strcat(trace, s); strcat(trace, " "); }
static int pos(const char *s) { const char *p = strstr(trace, s); return p ? (int)(p - trace) : -1; }
static int pos_after(const char *s, int from) { const char *p = strstr(trace + from, s); return p ? (int)(p - trace) : -1; }
#define HAS(s) (pos(s) >= 0)
#define POS(s) pos_checked(s)
static int pos_checked(const char *s) {
    int p = pos(s);
    if (p < 0) { fprintf(stderr, "missing event %s\n  trace: %s\n", s, trace); assert(0); }
    return p;
}

/* ---------------- P4 model: state, ctrl queue, hardware ---------------- */
typedef struct { int standby; int stage; uint32_t gen; int tag; } Msg;
static Msg q[8]; static int qn;
static Msg cur; static int cur_valid, cur_left;      /* the message the ctrl task is executing */
static sb_ctrl_track_t track;
static sb_p4_t p4;
static sb_rails_t rails;
static int user_executed, user_dropped;
static int legacy;                                    /* pre-fix classification: any queued/busy message is WORK */
static int stage_ticks = 12;                          /* a hardware stage spans 120 ms: several 40 ms re-sends see it in flight */
static int fail_verify, pad_high;
static int pg_ms; static bool v3_en;
static uint32_t now_ms;

static void post(int standby, int stage, uint32_t gen, int tag) {
    assert(qn < 8);
    Msg m = { standby, stage, gen, tag };
    q[qn++] = m;
    sb_ctrl_posted(&track, standby != 0);
}

static bool op_mux_off(void *c) { (void)c; ev("P4:MUX_OPEN"); pad_high = 0; return true; }
static bool op_park(void *c) { (void)c; ev("P4:BYPASS"); return true; }
static bool op_verify(void *c) { (void)c; ev("P4:VERIFY"); return !fail_verify && !pad_high; }
static bool op_mux_on(void *c) { (void)c; ev("P4:MUX_CLOSE"); pad_high = 1; return true; }
static bool op_gate(void *c, sb_gate_t m) { (void)c; ev(m == SB_GATE_CLOSED ? "P4:GATE_CLOSED" : "P4:GATE_OTHER"); return true; }
static bool op_rst(void *c, bool a) { (void)c; ev(a ? "P4:RST_LOW" : "P4:RST_HIGH"); return true; }
static bool op_bus(void *c) { (void)c; ev("P4:BUS"); return true; }
static bool op_rail(void *c, sb_rail_t r, bool on) {
    (void)c;
    static const char *n[] = { "P4:24V=", "P4:26V=", "P4:3V3=" };
    char b[32]; snprintf(b, sizeof b, "%s%d", n[r], on); ev(b);
    if (r == SB_RAIL_3V3) { v3_en = on; pg_ms = 0; }
    return true;
}
static int op_pg(void *c) { (void)c; return v3_en ? (pg_ms >= 2) : (pg_ms < 3); }
static bool op_pulse(void *c) { (void)c; ev("P4:PULSE"); return true; }
static void op_delay(void *c, uint32_t us) { (void)c; pg_ms += us / 1000; }
static bool op_cancelled(void *c) { (void)c; return false; }
static const sb_rails_ops_t rails_ops = {
    NULL, op_mux_off, op_park, op_verify, op_mux_on, op_gate, op_rst, op_bus, op_bus,
    op_rail, op_pg, op_pulse, op_delay, op_cancelled,
};

/* The P4's local inhibitors exactly as daq_board_standby_inhibitors() builds them. */
static uint32_t p4_local_inhibitors(void) {
    if (legacy) return (qn > 0 || cur_valid) ? BB_ST_INH_WORK : 0u;
    sb_inh_inputs_t in; memset(&in, 0, sizeof in);
    in.ctrl_queue_total = (uint32_t)qn;
    return sb_inh_compute(&in, &track);
}

static void run_stage_hw(const Msg *m) {
    switch (m->stage) {
    case 2: ev("P4:PAUSE"); sb_p4_step_complete(&p4, 2, m->gen, true, true); break;
    case 3: { bool ok = sb_rails_mux_off(&rails, &rails_ops); sb_p4_step_complete(&p4, 3, m->gen, ok, false); break; }
    case 5: { bool ok = sb_rails_off(&rails, &rails_ops); sb_p4_step_complete(&p4, 5, m->gen, ok, false); break; }
    case 8: { bool ok = sb_rails_on(&rails, &rails_ops); sb_p4_step_complete(&p4, 8, m->gen, ok, false); break; }
    case 9: rails.cut = false; ev("P4:RESTORE"); sb_p4_step_complete(&p4, 9, m->gen, true, false); break;
    case 10: ev("P4:SMU_OFF_VERIFIED"); sb_p4_step_complete(&p4, 10, m->gen, true, false); break;
    default: break;
    }
}

/* One tick of the ctrl task (daq_ctrl_task). */
static void ctrl_tick(void) {
    if (!cur_valid) {
        if (qn == 0) return;
        cur = q[0]; memmove(q, q + 1, sizeof(Msg) * (size_t)(qn - 1)); qn--;
        sb_ctrl_taken(&track, cur.standby != 0);
        cur_valid = 1; cur_left = stage_ticks;
        if (!cur.standby) {
            if (!sb_p4_admit_quiet(&p4)) {                 /* queued before a barrier: dropped, never run */
                user_dropped++; sb_ctrl_done(&track); cur_valid = 0; return;
            }
        }
        return;
    }
    if (--cur_left > 0) return;
    if (cur.standby) {
        if (sb_p4_step_begin(&p4, (uint8_t)cur.stage, cur.gen)) run_stage_hw(&cur);
    } else {
        user_executed++; ev("P4:USER_JOB"); sb_p4_leave(&p4);
    }
    sb_ctrl_done(&track); cur_valid = 0;
}

/* ---------------- C6 model ---------------- */
static int c6_stage, c6_left;
static bb_standby_reply_t c6_reply(uint32_t gen, uint8_t state) {
    bb_standby_reply_t r; memset(&r, 0, sizeof r);
    r.schema = BB_STANDBY_SCHEMA; r.state = state; r.ready = 1; r.generation = gen;
    return r;
}

/* ---------------- S3 environment ---------------- */
static uint32_t sleep_reply_inh;                       /* OR of every inhibitor the P4 reported on a SLEEP request */
static int polls;

static void e_lock(void *c) { (void)c; }
static void e_unlock(void *c) { (void)c; }
static uint32_t e_now(void *c) { (void)c; return now_ms; }
static uint32_t e_inh(void *c) { (void)c; return 0; }
static StandbyHatLink e_link(void *c) { (void)c; return STANDBY_HAT_LINKED; }
static StandbyXchg e_xchg(void *c, const bb_standby_request_t *rq, bb_standby_reply_t *rp) {
    (void)c;
    sb_p4_fx_t fx;
    const uint32_t seq = sb_p4_entry_seq(&p4);
    sb_p4_handle(&p4, rq, now_ms, seq, p4_local_inhibitors(), rp, &fx);
    if (rq->op == BB_ST_OP_SLEEP) sleep_reply_inh |= rp->inhibitors;
    if (rq->op == BB_ST_OP_POLL) polls++;
    if (fx.run_stage) post(1, fx.run_stage, fx.run_generation, 0);
    if (fx.forward_c6) { c6_stage = fx.forward_stage; c6_left = 2; }
    if (c6_stage && --c6_left <= 0) {
        uint8_t st = c6_stage == 6 ? BB_ST_ASLEEP : c6_stage == 7 ? BB_ST_WAKING : BB_ST_ACTIVE;
        bb_standby_reply_t cr = c6_reply(p4.generation, st);
        sb_p4_c6_reply(&p4, &cr, now_ms);
        c6_stage = 0;
    }
    sb_p4_tick(&p4, now_ms);
    return STANDBY_XCHG_OK;
}
static StandbyStepResult e_local(void *c, StandbyStep step, uint32_t gen) {
    (void)c; (void)gen;
    char b[24]; snprintf(b, sizeof b, "S3:LOCAL%d", (int)step); ev(b);
    return STANDBY_STEP_DONE;
}
static bool e_persist(void *c, uint32_t s) { (void)c; (void)s; return true; }
static bool e_wlock(void *c) { (void)c; return true; }
static void e_wunlock(void *c) { (void)c; }
static bool e_safe(void *c) { (void)c; return true; }

static StandbySystem sys;
static void world_init0(void) {
    trace[0] = 0; qn = 0; cur_valid = 0; user_executed = user_dropped = 0; fail_verify = 0; pad_high = 1;
    sleep_reply_inh = 0; polls = 0; c6_stage = 0; v3_en = true; pg_ms = 100; now_ms = 1000;
    sb_ctrl_track_init(&track);
    sb_p4_init(&p4);
    sb_p4_set_c6(&p4, true, true);
    sb_rails_init(&rails, true);
    StandbyOps ops = { .ctx = NULL, .lock = e_lock, .unlock = e_unlock, .now_ms = e_now, .inhibitors = e_inh,
                       .hat_link = e_link, .hat_exchange = e_xchg, .local_step = e_local,
                       .persist_timeout = e_persist, .write_lock = e_wlock, .write_unlock = e_wunlock,
                       .safe_outputs_off = e_safe };
    standby_system_init(&sys, &ops, 300);
}
static void tick(void) { now_ms += 10; standby_system_tick(&sys); ctrl_tick(); }
static int drive_until(StandbyState want, uint32_t max_ms) {
    for (uint32_t t = 0; t < max_ms; t += 10) { if (sys.policy.state == want) return 1; tick(); }
    return sys.policy.state == want;
}
static void drive(uint32_t ms) { for (uint32_t t = 0; t < ms; t += 10) tick(); }
/* A fresh world with the S3 having completed its first HAT poll (an unproven HAT is itself an inhibitor). */
static void world_init(void) { world_init0(); drive(1000); }

/* the first explicit request after a wake, as standby_p4.c provision_routes_if_held() does it */
static int explicit_request(void) {
    if (!sb_p4_admit(&p4)) return -1;
    int connected = 0;
    if (rails.mux_open) {
        rails.cut = false;                                  /* the glue only gets here after stage 9 */
        connected = sb_rails_mux_connect(&rails, &rails_ops);
        if (connected) sb_p4_routes_connected(&p4);
    }
    sb_p4_leave(&p4);
    return connected;
}

int main(void) {
    /* ---- 1. a complete sleep/wake with the PRODUCTION classification ---------------- */
    world_init();
    assert(standby_system_sleep(&sys) == STANDBY_RC_OK);
    assert(drive_until(STANDBY_ASLEEP, 30000));
    assert(sys.cancelled_prepares == 0);
    assert(sleep_reply_inh == 0);                           /* not one SLEEP reply carried an inhibitor: no self-cancel */
    assert(p4.state == BB_ST_ASLEEP && p4.analog_cut && p4.routes_held && p4.barrier);
    /* order: P4 muxes open and read back -> S3 local analog off -> P4 rails off */
    assert(POS("P4:MUX_OPEN") < POS("P4:BYPASS") && POS("P4:BYPASS") < POS("P4:VERIFY"));
    assert(POS("P4:VERIFY") < POS("S3:LOCAL5"));            /* S3 VANALOG cut comes after the P4 mux is proven open */
    assert(POS("S3:LOCAL5") < POS("P4:24V=0"));             /* S3 local step runs BEFORE the HAT part of stage 5 */
    /* stage 5 re-reads the pads (guard) after the S3 cut and before the first P4 pin moves */
    assert(pos_after("P4:VERIFY", POS("S3:LOCAL5")) > POS("S3:LOCAL5"));
    assert(pos_after("P4:VERIFY", POS("S3:LOCAL5")) < POS("P4:GATE_CLOSED"));
    assert(POS("P4:GATE_CLOSED") < POS("P4:24V=0") && POS("P4:24V=0") < POS("P4:26V=0") &&
           POS("P4:26V=0") < POS("P4:3V3=0"));
    assert(pos_after("P4:MUX_OPEN", POS("P4:MUX_OPEN") + 1) < 0);   /* stage 5 does not redo the disconnect */
    puts("sleep-order");

    /* ---- wake: rails back, converters back, NOTHING reconnected, nothing restarted ---- */
    int wake_mark = (int)strlen(trace);
    standby_system_wake(&sys);
    assert(drive_until(STANDBY_ACTIVE, 30000));
    assert(p4.state == BB_ST_ACTIVE && !sb_p4_blocked(&p4));
    assert(pos_after("P4:3V3=1", wake_mark) >= 0 && pos_after("P4:RESTORE", wake_mark) >= 0);
    assert(pos_after("P4:MUX_CLOSE", wake_mark) < 0);       /* no route closed by the wake */
    assert(p4.routes_held && rails.mux_open && p4.acq_resume_pending);
    drive(5000);                                            /* presence, status polls and telemetry: still nothing */
    assert(pos_after("P4:MUX_CLOSE", wake_mark) < 0 && polls > 4);
    assert(p4.routes_held && p4.acq_resume_pending);
    assert(explicit_request() == 1);                        /* the first explicit request connects the path */
    assert(pos_after("P4:MUX_CLOSE", wake_mark) > pos_after("P4:RESTORE", wake_mark));
    assert(!p4.routes_held && sb_p4_take_acq_resume(&p4));
    assert(explicit_request() == 0 && !HAS("P4:MUX_CLOSE P4:MUX_CLOSE"));   /* repeats move nothing */
    puts("wake-holds-routes");

    /* ---- 2. negative control: the pre-fix classification cancels the sleep ----------- */
    world_init();
    legacy = 1;
    assert(standby_system_sleep(&sys) == STANDBY_RC_OK);
    int reached = drive_until(STANDBY_ASLEEP, 15000);
    assert(!reached);                                       /* the standby stage itself counted as work */
    assert(sys.policy.state == STANDBY_ACTIVE && !HAS("P4:MUX_OPEN") && !HAS("P4:24V=0"));
    legacy = 0;
    puts("legacy-self-cancels");

    /* ---- 3. a REAL user job arriving while preparing cancels it, hardware untouched --- */
    world_init();
    assert(standby_system_sleep(&sys) == STANDBY_RC_OK);
    for (int i = 0; i < 2000 && !(sys.policy.state == STANDBY_PREPARING && p4.state == BB_ST_PREPARING); ++i) tick();
    assert(p4.state == BB_ST_PREPARING && !p4.barrier);
    post(0, 0, 0, 7);                                       /* e.g. a SET_SOURCE that was admitted just before */
    assert(sb_p4_admit(&p4) == 0);                          /* ... and new work is refused from now on */
    drive(2000);
    assert(sys.policy.state != STANDBY_ASLEEP);
    assert(!HAS("P4:PAUSE") && !HAS("P4:MUX_OPEN") && !HAS("P4:24V=0") && !HAS("S3:LOCAL5"));
    drive(2000);
    assert(user_executed + user_dropped == 1);              /* it was handled by the ctrl task, never against paused ADCs */
    puts("user-job-cancels");

    /* ---- 4. user work posted after the barrier is dropped, not executed --------------- */
    world_init();
    assert(standby_system_sleep(&sys) == STANDBY_RC_OK);
    assert(drive_until(STANDBY_ASLEEP, 30000));
    post(0, 0, 0, 9);                                       /* an unadmitted producer (battery-sim style) */
    drive(500);
    assert(user_executed == 0 && user_dropped == 1 && !HAS("P4:USER_JOB"));
    assert(track.standby_queued == 0 && !track.user_busy && !track.standby_busy);
    puts("late-user-job-dropped");

    /* ---- 5. a mux readback failure stops everything before any rail or S3 analog cut --- */
    world_init();
    fail_verify = 1;
    assert(standby_system_sleep(&sys) == STANDBY_RC_OK);
    drive(30000);
    assert(sys.policy.state == STANDBY_FAULT_SAFE || sys.policy.state != STANDBY_ASLEEP);
    assert(HAS("P4:MUX_OPEN") && !HAS("P4:24V=0") && !HAS("P4:3V3=0") && !HAS("P4:GATE_CLOSED"));
    assert(!HAS("S3:LOCAL5"));                              /* the mainboard never cut its analog supply */
    assert(!rails.cut && !p4.analog_cut);
    puts("mux-failure-stops");

    /* ---- 6. twenty cycles, routes closed only by an explicit request ------------------ */
    world_init();
    for (int i = 0; i < 20; ++i) {
        assert(standby_system_sleep(&sys) == STANDBY_RC_OK);
        assert(drive_until(STANDBY_ASLEEP, 30000));
        standby_system_wake(&sys);
        assert(drive_until(STANDBY_ACTIVE, 30000));
        assert(p4.routes_held && !sb_p4_blocked(&p4));
        assert(explicit_request() == 1);
        assert(sb_p4_take_acq_resume(&p4));
        drive(100);
    }
    assert(sys.cancelled_prepares == 0 && !p4.routes_held);
    puts("twenty-cycles");
    return 0;
}
"""


def test_standby_s3_p4_integrated_sequence():
    out = compile_and_run(
        MAIN,
        sources=SOURCES,
        include_dirs=[POWER, P4_STANDBY],
    )
    for marker in ("sleep-order", "wake-holds-routes", "legacy-self-cancels", "user-job-cancels",
                   "late-user-job-dropped", "mux-failure-stops", "twenty-cycles"):
        assert marker in out, marker
