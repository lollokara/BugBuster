"""Host execution of the LA HAT (RP2040) standby participant.

The code under test is the real ``bb_standby_core.c``; it is compiled together with the
HAT_CMD_* macros extracted from ``bb_config.h`` (which itself needs the Pico SDK), plus
fake guarded power/GPIO callbacks that record call order and inject failures.
"""

import re

import pytest

from tests.lib.srcread import read_source
from tests.firmware_host.fwhost import compile_and_run, extract_defines

RP_SRC = "Firmware/RP2040/src"
COMMON = "Firmware/DAQ_HAT/common"
CORE_C = f"{RP_SRC}/bb_standby_core.c"
CONFIG_H = f"{RP_SRC}/bb_config.h"

FREE_CMDS = [
    "HAT_CMD_PING", "HAT_CMD_GET_INFO", "HAT_CMD_GET_PIN_CONFIG", "HAT_CMD_GET_CAPS",
    "HAT_CMD_GET_POWER_STATUS", "HAT_CMD_GET_IO_VOLTAGE", "HAT_CMD_GET_DAP_STATUS",
    "HAT_CMD_LA_GET_STATUS", "HAT_CMD_LA_READ_DATA", "HAT_CMD_LA_STOP",
    "HAT_CMD_LA_LOG_ENABLE", "HAT_CMD_LA_USB_RESET", "HAT_CMD_GET_RAIL_STATUS",
    "HAT_CMD_SET_LED_STATE", "HAT_CMD_CALIBRATE_STATUS", "HAT_CMD_CALIBRATE_EXPORT",
    "HAT_CMD_FW_CHUNK", "HAT_CMD_FW_COMMIT", "HAT_CMD_FW_STATUS",
]
WORK_CMDS = [
    "HAT_CMD_SET_PIN_CONFIG", "HAT_CMD_RESET", "HAT_CMD_SET_IO_VOLTAGE",
    "HAT_CMD_GET_TARGET_INFO", "HAT_CMD_SET_SWD_CLOCK", "HAT_CMD_LA_CONFIG",
    "HAT_CMD_LA_SET_TRIGGER", "HAT_CMD_LA_ARM", "HAT_CMD_LA_FORCE",
    "HAT_CMD_LA_STREAM_START", "HAT_CMD_LA_USB_SEND", "HAT_CMD_LA_SET_ROUTE",
    "HAT_CMD_CALIBRATE_START", "HAT_CMD_CALIBRATE_IMPORT", "HAT_CMD_SET_IO_BANK",
    "HAT_CMD_SET_RAIL_VOLTAGE", "HAT_CMD_FW_BEGIN",
]
# Gated only when they would turn something ON (enable byte / OE byte non-zero).
CONDITIONAL_CMDS = ["HAT_CMD_SET_POWER", "HAT_CMD_SET_RAIL_ENABLE", "HAT_CMD_SET_LEVEL_SHIFT"]


def _all_hat_cmds():
    return re.findall(r"#define\s+(HAT_CMD_\w+)", read_source(CONFIG_H))


def _prelude():
    names = _all_hat_cmds()
    return extract_defines(CONFIG_H, names) + "\n#define BB_SB_HOST_TEST 1\n"


def _run(body: str) -> str:
    src = _prelude() + '\n#include "bb_standby_core.c"\n' + HARNESS + body
    return compile_and_run(src, include_dirs=[RP_SRC, COMMON])


HARNESS = r"""
#include <assert.h>
#include <stdio.h>
#include <string.h>

enum { OP_OE = 1, OP_PINS, OP_ROUTE, OP_RAIL0 = 10, OP_RAIL1, OP_RAIL2,
       OP_VERIFY = 20, OP_RESTORE = 30, OP_LED_DARK = 40, OP_LED_NORMAL = 41 };

typedef struct sim {
    bb_sb_t c;
    int depth;                 // current spin-lock nesting
    int lock_violations;       // an op ran while the lock was held
    uint32_t now;
    bb_sb_facts_t facts;
    int ops[128];
    int nops;
    int fail_op;               // op code that reports failure (0 = none)
    void (*on_sample)(struct sim *);
    int rails_on[3];           // the fake hardware: 1 = enabled
} sim_t;

static void s_lock(void *p)   { ((sim_t *)p)->depth++; }
static void s_unlock(void *p) { ((sim_t *)p)->depth--; }
static uint32_t s_now(void *p) { return ((sim_t *)p)->now; }
static void s_sample(void *p, bb_sb_facts_t *f) {
    sim_t *s = (sim_t *)p;
    *f = s->facts;
    if (s->on_sample) s->on_sample(s);
}

static bool rec(sim_t *s, int code) {
    if (s->depth != 0) s->lock_violations++;
    if (s->nops < 128) s->ops[s->nops++] = code;
    return s->fail_op != code;
}
static bool o_oe(void *p)     { return rec((sim_t *)p, OP_OE); }
static bool o_pins(void *p)   { return rec((sim_t *)p, OP_PINS); }
static bool o_route(void *p)  { return rec((sim_t *)p, OP_ROUTE); }
static bool o_rail(void *p, uint8_t r) {
    sim_t *s = (sim_t *)p;
    bool ok = rec(s, OP_RAIL0 + r);
    if (ok) s->rails_on[r] = 0;
    return ok;
}
static bool o_verify(void *p)  { return rec((sim_t *)p, OP_VERIFY); }
static bool o_restore(void *p) { return rec((sim_t *)p, OP_RESTORE); }
static bool o_leds(void *p, bool dark) { return rec((sim_t *)p, dark ? OP_LED_DARK : OP_LED_NORMAL); }

static const bb_sb_env_t ENV_T = { 0, s_lock, s_unlock, s_now, s_sample };
static const bb_sb_ops_t OPS_T = { 0, o_oe, o_pins, o_route, o_rail, o_verify, o_restore, o_leds };

static void sim_init(sim_t *s) {
    memset(s, 0, sizeof(*s));
    bb_sb_init(&s->c);
    s->now = 1000;
    s->rails_on[0] = s->rails_on[1] = s->rails_on[2] = 1;
}

static bb_standby_reply_t call(sim_t *s, uint8_t op, uint8_t stage, uint32_t gen) {
    bb_standby_request_t rq;
    memset(&rq, 0, sizeof(rq));
    rq.schema = BB_STANDBY_SCHEMA; rq.op = op; rq.stage = stage; rq.generation = gen;
    rq.timeout_seconds = 300;
    bb_sb_env_t env = ENV_T; env.ctx = s;
    bb_sb_ops_t ops = OPS_T; ops.ctx = s;
    bb_standby_reply_t rp;
    bb_sb_service(&s->c, &env, &ops, (const uint8_t *)&rq, (uint8_t)sizeof(rq), &rp);
    assert(s->depth == 0);
    return rp;
}
static bb_standby_reply_t slp(sim_t *s, uint8_t st, uint32_t gen) { return call(s, BB_ST_OP_SLEEP, st, gen); }
static bb_standby_reply_t wak(sim_t *s, uint8_t st, uint32_t gen) { return call(s, BB_ST_OP_WAKE, st, gen); }
static bb_standby_reply_t poll(sim_t *s) { return call(s, BB_ST_OP_POLL, 0, 0); }

static void clear_ops(sim_t *s) { s->nops = 0; }
static int count_op(const sim_t *s, int code) {
    int n = 0; for (int i = 0; i < s->nops; ++i) if (s->ops[i] == code) n++; return n;
}
static void expect_ops(const sim_t *s, const int *want, int n) {
    if (s->nops != n) { printf("ops n=%d want %d:", s->nops, n); for (int i = 0; i < s->nops; ++i) printf(" %d", s->ops[i]); printf("\n"); }
    assert(s->nops == n);
    for (int i = 0; i < n; ++i) { if (s->ops[i] != want[i]) printf("op[%d]=%d want %d\n", i, s->ops[i], want[i]); assert(s->ops[i] == want[i]); }
}

static const int SAFE_SEQ[] = { OP_OE, OP_PINS, OP_ROUTE, OP_RAIL1, OP_RAIL2, OP_RAIL0, OP_VERIFY };
#define SAFE_N 7

static void sleep_all(sim_t *s, uint32_t gen) {
    bb_standby_reply_t r;
    for (uint8_t st = 1; st <= 6; ++st) {
        r = slp(s, st, gen);
        assert(r.ready && r.failure == 0 && r.generation == gen);
    }
    assert(r.state == BB_ST_ASLEEP);
}
static void wake_all(sim_t *s, uint32_t gen) {
    bb_standby_reply_t r;
    for (uint8_t st = 7; st <= 11; ++st) {
        r = wak(s, st, gen);
        assert(r.ready && r.failure == 0 && r.generation == gen);
    }
    assert(r.state == BB_ST_ACTIVE);
}
"""


def test_wire_contract_and_all_commands_classified():
    names = _all_hat_cmds()
    known = set(FREE_CMDS) | set(WORK_CMDS) | set(CONDITIONAL_CMDS)
    missing = sorted(set(names) - known)
    assert not missing, (
        f"new HAT command(s) {missing}: classify them in bb_sb_classify() "
        "(FREE, WORK or conditional) and in this test")
    stale = sorted(known - set(names))
    assert not stale, f"test table names no longer in bb_config.h: {stale}"
    core = read_source(CORE_C)
    for name in sorted(known):
        assert name in core, f"{name} is not explicitly handled in bb_sb_classify()"

    checks = []
    for n in FREE_CMDS:
        checks.append(f"assert(bb_sb_classify({n}, NULL, 0) == BB_SB_CMD_FREE);")
    for n in WORK_CMDS:
        checks.append(f"assert(bb_sb_classify({n}, NULL, 0) == BB_SB_CMD_WORK);")
    body = r"""
int main(void) {
    assert(sizeof(bb_standby_request_t) == 16 && sizeof(bb_standby_reply_t) == 16);
    assert(BB_HAT_CMD_STANDBY == 0x7C && BB_HAT_RSP_STANDBY == 0x9C && BB_STANDBY_SCHEMA == 1);
    assert(bb_sb_classify(BB_HAT_CMD_STANDBY, NULL, 0) == BB_SB_CMD_STANDBY);
    """ + "\n    ".join(checks) + r"""
    uint8_t on2[2] = { 0, 1 }, off2[2] = { 0, 0 }, on1[1] = { 1 }, off1[1] = { 0 };
    // an enable/OE request is work, a disable is free (always safe, even asleep)
    assert(bb_sb_classify(HAT_CMD_SET_POWER, on2, 2) == BB_SB_CMD_WORK);
    assert(bb_sb_classify(HAT_CMD_SET_POWER, off2, 2) == BB_SB_CMD_FREE);
    assert(bb_sb_classify(HAT_CMD_SET_RAIL_ENABLE, on2, 2) == BB_SB_CMD_WORK);
    assert(bb_sb_classify(HAT_CMD_SET_RAIL_ENABLE, off2, 2) == BB_SB_CMD_FREE);
    assert(bb_sb_classify(HAT_CMD_SET_LEVEL_SHIFT, on1, 1) == BB_SB_CMD_WORK);
    assert(bb_sb_classify(HAT_CMD_SET_LEVEL_SHIFT, off1, 1) == BB_SB_CMD_FREE);
    // malformed requests never get a free pass
    assert(bb_sb_classify(HAT_CMD_SET_POWER, NULL, 0) == BB_SB_CMD_WORK);
    assert(bb_sb_classify(HAT_CMD_SET_POWER, off2, 1) == BB_SB_CMD_WORK);
    assert(bb_sb_classify(HAT_CMD_SET_LEVEL_SHIFT, NULL, 0) == BB_SB_CMD_WORK);
    assert(bb_sb_classify(0x6F, NULL, 0) == BB_SB_CMD_FREE);   // unknown -> INVALID_CMD upstream
    puts("classify");
    return 0;
}
"""
    assert "classify" in _run(body)


def test_full_cycle_order_idempotence_and_generation_echo():
    body = r"""
int main(void) {
    sim_t s; sim_init(&s);
    bb_standby_reply_t r;

    r = poll(&s);                                              // POLL never adopts a generation
    assert(r.state == BB_ST_ACTIVE && r.ready && r.generation == 0 && r.inhibitors == 0);

    // stage 1: freeze only, no hardware action; stage 2: the whole power-down in one round trip
    r = slp(&s, 1, 100);
    assert(r.ready && r.state == BB_ST_PREPARING && r.generation == 100 && s.nops == 0);
    assert(!bb_sb_monitor_allowed(&s.c));                       // power ADC monitor stopped
    r = slp(&s, 2, 100);
    assert(r.ready && r.state == BB_ST_ASLEEP && r.failure == 0);
    expect_ops(&s, SAFE_SEQ, SAFE_N);
    assert(s.c.barrier && s.c.hw_paused && !s.c.leds_dark);
    assert(!s.rails_on[0] && !s.rails_on[1] && !s.rails_on[2]);
    clear_ops(&s);

    for (uint8_t st = 3; st <= 5; ++st) { r = slp(&s, st, 100); assert(r.ready); }
    assert(s.nops == 0 && !s.c.leds_dark);                      // LEDs stay on until stage 6
    r = slp(&s, 6, 100);
    assert(r.ready && s.c.leds_dark);
    { int w[] = { OP_LED_DARK }; expect_ops(&s, w, 1); }
    clear_ops(&s);

    // every stage repeated is an idempotent ready and runs nothing again
    for (uint8_t st = 1; st <= 6; ++st) { r = slp(&s, st, 100); assert(r.ready && r.generation == 100); }
    assert(s.nops == 0);

    r = poll(&s);                                              // asleep and stable
    assert(r.state == BB_ST_ASLEEP && r.ready && r.generation == 100);

    // wake: safe re-assert, S3-only analog stages, readiness, indicators LAST
    r = wak(&s, 7, 101);
    assert(r.ready && r.state == BB_ST_WAKING && r.generation == 101);
    expect_ops(&s, SAFE_SEQ, SAFE_N);
    clear_ops(&s);
    r = wak(&s, 8, 101); assert(r.ready && s.nops == 0);
    r = wak(&s, 9, 101); assert(r.ready && s.nops == 0);
    r = wak(&s, 11, 101);                                      // indicators before readiness: refused
    assert(!r.ready && r.failure == BB_SB_FAIL_ORDER && s.c.leds_dark);
    r = wak(&s, 10, 101);
    assert(r.ready && s.nops == 2 && s.ops[0] == OP_RESTORE && s.ops[1] == OP_VERIFY);
    assert(s.c.state == BB_ST_WAKING && s.c.barrier);          // still blocked until stage 11
    clear_ops(&s);
    r = wak(&s, 11, 101);
    assert(r.ready && r.state == BB_ST_ACTIVE && !s.c.barrier && !s.c.leds_dark);
    { int w[] = { OP_LED_NORMAL }; expect_ops(&s, w, 1); }
    assert(s.rails_on[0] == 0 && s.rails_on[1] == 0 && s.rails_on[2] == 0);   // wake enabled nothing
    assert(bb_sb_monitor_allowed(&s.c));
    clear_ops(&s);
    for (uint8_t st = 7; st <= 11; ++st) { r = wak(&s, st, 101); assert(r.ready); }
    assert(s.nops == 0 && s.lock_violations == 0);
    puts("cycle");
    return 0;
}
"""
    assert "cycle" in _run(body)


def test_stale_wrap_arg_and_progress_epoch():
    body = r"""
int main(void) {
    sim_t s; sim_init(&s);
    bb_standby_reply_t r;
    sleep_all(&s, 100); wake_all(&s, 101);

    uint32_t act = s.c.activity;
    clear_ops(&s);
    r = slp(&s, 1, 100);                                       // older than 101
    assert(!r.ready && r.failure == BB_SB_FAIL_STALE && r.generation == 101 && s.c.state == BB_ST_ACTIVE);
    r = wak(&s, 7, 0xFFFFFFF0u);                               // modular: older
    assert(r.failure == BB_SB_FAIL_STALE);
    assert(s.nops == 0 && s.c.activity == act);

    sim_init(&s);
    s.c.generation = 0xFFFFFFFEu; s.c.gen_valid = true;
    r = slp(&s, 1, 2);                                         // wraps forward
    assert(r.ready && r.generation == 2);

    sim_init(&s);
    r = slp(&s, 9, 5);  assert(r.failure == BB_SB_FAIL_ARG);   // wake stage on a sleep op
    r = wak(&s, 3, 5);  assert(r.failure == BB_SB_FAIL_ARG);
    r = call(&s, 7, 1, 5); assert(r.failure == BB_SB_FAIL_ARG);          // unknown op
    { bb_standby_request_t q; memset(&q, 0, sizeof q);
      q.schema = 9; q.op = BB_ST_OP_SLEEP; q.stage = 1; q.generation = 5;
      bb_standby_reply_t rp; bb_sb_env_t env = ENV_T; env.ctx = &s; bb_sb_ops_t ops = OPS_T; ops.ctx = &s;
      bb_sb_service(&s.c, &env, &ops, (const uint8_t *)&q, 16, &rp);
      assert(rp.failure == BB_SB_FAIL_ARG && rp.schema == BB_STANDBY_SCHEMA);
      bb_sb_service(&s.c, &env, &ops, (const uint8_t *)&q, 15, &rp);     // wrong length
      assert(rp.failure == BB_SB_FAIL_ARG); }
    assert(s.c.state == BB_ST_ACTIVE && !s.c.gen_valid);

    // BOOT progress rebases the S3 epoch even when the stored counter is higher
    sim_init(&s);
    sleep_all(&s, 5000);
    r = call(&s, BB_ST_OP_PROGRESS, BB_ST_BOOT_STAGE, 3);
    assert(r.ready && r.generation == 3 && r.failure == 0 && s.c.state == BB_ST_ASLEEP);
    r = wak(&s, 7, 4);
    assert(r.ready && r.state == BB_ST_WAKING && r.generation == 4);

    // any other PROGRESS is "not applied": no display here, nothing adopted, no side effect
    sim_init(&s);
    r = call(&s, BB_ST_OP_PROGRESS, 4, 77);
    assert(!r.ready && r.failure == 0 && r.generation == 0 && !s.c.gen_valid && s.nops == 0);
    assert(s.c.state == BB_ST_ACTIVE && s.c.activity == 0);
    puts("stale");
    return 0;
}
"""
    assert "stale" in _run(body)


def test_inhibitors_map_real_owner_bits_and_refuse_quiesce():
    body = r"""
static void set_la_armed(bb_sb_facts_t *f)     { f->la_armed = true; }
static void set_la_capturing(bb_sb_facts_t *f) { f->la_capturing = true; }
static void set_la_streaming(bb_sb_facts_t *f) { f->la_streaming = true; }
static void set_la_usb(bb_sb_facts_t *f)       { f->la_usb_session = true; }
static void set_la_pending(bb_sb_facts_t *f)   { f->la_usb_pending = true; }
static void set_fw(bb_sb_facts_t *f)           { f->fw_update = true; }
static void set_cal(bb_sb_facts_t *f)          { f->calibration = true; }
static void set_uart(bb_sb_facts_t *f)         { f->uart_bridge = true; }
static void set_bus(bb_sb_facts_t *f)          { f->bus = true; }
static void set_cmd(bb_sb_facts_t *f)          { f->cmd_pending = true; }

int main(void) {
    struct { void (*set)(bb_sb_facts_t *); uint32_t bit; } t[] = {
        { set_la_armed, BB_ST_INH_TRIGGER }, { set_la_capturing, BB_ST_INH_STREAM },
        { set_la_streaming, BB_ST_INH_STREAM }, { set_la_usb, BB_ST_INH_STREAM },
        { set_la_pending, BB_ST_INH_STREAM }, { set_fw, BB_ST_INH_OTA },
        { set_cal, BB_ST_INH_CALIBRATION }, { set_uart, BB_ST_INH_UART },
        { set_bus, BB_ST_INH_BUS }, { set_cmd, BB_ST_INH_WORK },
    };
    for (unsigned i = 0; i < sizeof(t) / sizeof(t[0]); ++i) {
        sim_t s; sim_init(&s);
        t[i].set(&s.facts);
        bb_standby_reply_t r = poll(&s);
        assert(r.inhibitors == t[i].bit);                        // POLL reports the real owner bit
        r = slp(&s, 1, 10);
        assert(!r.ready && r.failure == BB_SB_FAIL_BUSY && r.state == BB_ST_ACTIVE);
        assert((r.inhibitors & t[i].bit) == t[i].bit && s.nops == 0 && !s.c.barrier);
        memset(&s.facts, 0, sizeof s.facts);                     // the owner finished: sleep may proceed
        r = slp(&s, 1, 10);
        assert(r.ready && r.state == BB_ST_PREPARING);
    }

    // an owner that appears after quiesce refuses the power-down at stage 2, hardware untouched
    sim_t s; sim_init(&s);
    bb_standby_reply_t r = slp(&s, 1, 20);
    assert(r.ready);
    s.facts.la_streaming = true;
    r = slp(&s, 2, 20);
    assert(!r.ready && r.failure == BB_SB_FAIL_BUSY && !s.c.barrier && s.nops == 0 && s.c.state == BB_ST_PREPARING);
    assert(r.inhibitors & BB_ST_INH_STREAM);

    // DONE capture data is RAM: it is not an owner and does not block (no fact exists for it)
    sim_init(&s);
    r = slp(&s, 1, 30); assert(r.ready);
    puts("inhibitors");
    return 0;
}
"""
    assert "inhibitors" in _run(body)


def test_barrier_race_and_cancel_never_touch_hardware():
    body = r"""
static void inject_admit(sim_t *s) { assert(bb_sb_admit(&s->c)); bb_sb_leave(&s->c); s->on_sample = 0; }
// PREPARING already refuses entries, so the stage-2 recheck is white-box: pretend one slipped in.
static void inject_bump(sim_t *s) { s->c.entered_seq++; s->on_sample = 0; }

int main(void) {
    sim_t s; sim_init(&s);
    bb_standby_reply_t r;

    // work that enters between entry_seq and the decision is caught at stage 1 ...
    s.on_sample = inject_admit;
    r = slp(&s, 1, 10);
    assert(!r.ready && r.failure == BB_SB_FAIL_BUSY && s.c.state == BB_ST_ACTIVE);
    r = slp(&s, 1, 10); assert(r.ready && s.c.state == BB_ST_PREPARING);

    // ... and at stage 2, where the barrier is set atomically with the recheck
    s.on_sample = inject_bump;
    r = slp(&s, 2, 10);
    assert(!r.ready && r.failure == BB_SB_FAIL_BUSY && !s.c.barrier && s.nops == 0);

    // in-flight work refuses sleep
    sim_init(&s);
    assert(bb_sb_admit(&s.c));
    r = slp(&s, 1, 10);
    assert(r.failure == BB_SB_FAIL_BUSY && (r.inhibitors & BB_ST_INH_WORK));
    bb_sb_leave(&s.c); bb_sb_leave(&s.c);                       // underflow is harmless
    assert(s.c.inflight == 0);
    r = slp(&s, 1, 10); assert(r.ready);

    // PREPARING already refuses new work; the refusal is activity + a wake request
    uint32_t a = s.c.activity;
    assert(!bb_sb_admit(&s.c));
    assert(s.c.activity == a + 1 && s.c.wake_pending);
    assert(!bb_sb_dap_enter(&s.c, s.now) && s.c.activity == a + 2);
    r = slp(&s, 2, 10);                                         // wake_pending keeps the barrier down
    assert(!r.ready && r.failure == BB_SB_FAIL_BUSY && !s.c.barrier && s.nops == 0);
    assert(r.inhibitors & BB_ST_INH_WORK);

    // cancel: the S3 wakes. Nothing was powered down, so nothing is re-asserted or lit
    r = wak(&s, 7, 11);
    assert(r.ready && r.state == BB_ST_WAKING && s.nops == 0);
    r = wak(&s, 8, 11); assert(r.ready);
    r = wak(&s, 9, 11); assert(r.ready);
    r = wak(&s, 10, 11); assert(r.ready && s.nops == 0);
    assert(!bb_sb_admit(&s.c));                                 // still WAKING: blocked
    r = wak(&s, 11, 11);
    assert(r.ready && r.state == BB_ST_ACTIVE && s.nops == 0 && !s.c.wake_pending);
    assert(bb_sb_admit(&s.c)); bb_sb_leave(&s.c);

    // the same wake request repeated on an ACTIVE participant is harmless
    for (uint8_t st = 7; st <= 11; ++st) { r = wak(&s, st, 11); assert(r.ready && r.state == BB_ST_ACTIVE); }
    puts("barrier");
    return 0;
}
"""
    assert "barrier" in _run(body)


def test_power_down_failure_is_fault_safe_and_attempts_every_step():
    body = r"""
int main(void) {
    int codes[] = { OP_OE, OP_PINS, OP_ROUTE, OP_RAIL1, OP_RAIL2, OP_RAIL0, OP_VERIFY };
    for (unsigned i = 0; i < sizeof(codes) / sizeof(codes[0]); ++i) {
        sim_t s; sim_init(&s);
        bb_standby_reply_t r = slp(&s, 1, 40);
        assert(r.ready);
        s.fail_op = codes[i];
        r = slp(&s, 2, 40);
        // never ASLEEP/ready after a failed power-down, and every remaining step still ran
        assert(!r.ready && r.state == BB_ST_FAULT_SAFE && r.failure == BB_SB_FAIL_HW);
        expect_ops(&s, SAFE_SEQ, SAFE_N);
        assert(s.c.barrier && s.c.hw_paused && s.c.step_running == 0);
        assert(!bb_sb_admit(&s.c));                              // nothing runs on a half-down HAT
        r = poll(&s);
        assert(!r.ready && r.state == BB_ST_FAULT_SAFE && r.failure == BB_SB_FAIL_HW);
        r = slp(&s, 2, 40);                                      // sleep is not retried from a fault
        assert(!r.ready && r.state == BB_ST_FAULT_SAFE && r.failure == BB_SB_FAIL_HW);
        r = slp(&s, 6, 40); assert(!r.ready && r.state == BB_ST_FAULT_SAFE);
        clear_ops(&s);

        // recovery is the wake path, which re-asserts the safe state first
        s.fail_op = 0;
        r = wak(&s, 7, 41);
        assert(r.ready && r.state == BB_ST_WAKING);
        expect_ops(&s, SAFE_SEQ, SAFE_N);
        wak(&s, 8, 41); wak(&s, 9, 41);
        r = wak(&s, 10, 41); assert(r.ready);
        r = wak(&s, 11, 41); assert(r.ready && r.state == BB_ST_ACTIVE && !s.c.barrier);
    }

    // a wake that cannot re-assert safe, or cannot restore readiness, stays FAULT_SAFE; retry works
    sim_t s; sim_init(&s);
    sleep_all(&s, 50);
    s.fail_op = OP_RAIL1;
    bb_standby_reply_t r = wak(&s, 7, 51);
    assert(!r.ready && r.state == BB_ST_FAULT_SAFE && r.failure == BB_SB_FAIL_HW);
    s.fail_op = 0;
    r = wak(&s, 7, 51); assert(r.ready && r.state == BB_ST_WAKING);
    wak(&s, 8, 51); wak(&s, 9, 51);
    s.fail_op = OP_RESTORE;
    r = wak(&s, 10, 51);
    assert(!r.ready && r.state == BB_ST_FAULT_SAFE && s.c.hw_paused);
    r = wak(&s, 11, 51); assert(!r.ready && r.failure == BB_SB_FAIL_ORDER);
    s.fail_op = 0;
    r = wak(&s, 10, 51); assert(r.ready && !s.c.hw_paused);
    r = wak(&s, 11, 51); assert(r.ready && r.state == BB_ST_ACTIVE);

    // an indicator failure leaves the instrument usable and retryable
    sim_init(&s);
    sleep_all(&s, 60);
    wak(&s, 7, 61); wak(&s, 8, 61); wak(&s, 9, 61); wak(&s, 10, 61);
    s.fail_op = OP_LED_NORMAL;
    r = wak(&s, 11, 61);
    assert(!r.ready && r.state == BB_ST_ACTIVE && r.failure == BB_SB_FAIL_HW && !s.c.barrier);
    assert(bb_sb_admit(&s.c)); bb_sb_leave(&s.c);
    s.fail_op = 0;
    r = wak(&s, 11, 61); assert(r.ready && !s.c.leds_dark);

    // an indicator failure while going dark does not pretend the stage worked
    sim_init(&s);
    slp(&s, 1, 70); slp(&s, 2, 70); slp(&s, 3, 70); slp(&s, 4, 70); slp(&s, 5, 70);
    s.fail_op = OP_LED_DARK;
    r = slp(&s, 6, 70);
    assert(!r.ready && r.state == BB_ST_ASLEEP && r.failure == BB_SB_FAIL_HW && !s.c.leds_dark);
    s.fail_op = 0;
    r = slp(&s, 6, 70); assert(r.ready && s.c.leds_dark);
    puts("fault");
    return 0;
}
"""
    assert "fault" in _run(body)


def test_stale_completions_and_late_replies_do_not_advance():
    body = r"""
int main(void) {
    sim_t s; sim_init(&s);
    bb_standby_request_t q; memset(&q, 0, sizeof q);
    q.schema = BB_STANDBY_SCHEMA; q.op = BB_ST_OP_SLEEP; q.stage = 1; q.generation = 9;
    bb_standby_reply_t rp; bb_sb_fx_t fx;
    bb_sb_facts_t f; memset(&f, 0, sizeof f);
    bb_sb_handle(&s.c, &q, 10, bb_sb_entry_seq(&s.c), &f, &rp, &fx);
    assert(rp.ready);
    q.stage = 2;
    bb_sb_handle(&s.c, &q, 11, bb_sb_entry_seq(&s.c), &f, &rp, &fx);
    assert(!rp.ready && fx.run_stage == 2 && s.c.barrier && s.c.step_running == 2);

    // a second request for the stage that is still executing: not ready, never run twice
    bb_sb_handle(&s.c, &q, 12, bb_sb_entry_seq(&s.c), &f, &rp, &fx);
    assert(!rp.ready && fx.run_stage == 0 && rp.failure == 0);
    // another stage while one executes: refused as out of order
    q.stage = 3;
    bb_sb_handle(&s.c, &q, 12, bb_sb_entry_seq(&s.c), &f, &rp, &fx);
    assert(!rp.ready && rp.failure == BB_SB_FAIL_ORDER);

    // completions for a stage that is not running are ignored
    bb_sb_step_done(&s.c, 7, true);
    bb_sb_step_done(&s.c, 6, true);
    assert(s.c.state == BB_ST_PREPARING && s.c.step_running == 2 && !s.c.leds_dark);
    bb_sb_step_done(&s.c, 2, true);
    assert(s.c.state == BB_ST_ASLEEP && s.c.step_running == 0);
    bb_sb_step_done(&s.c, 2, false);                              // duplicate (and contradictory): ignored
    assert(s.c.state == BB_ST_ASLEEP && s.c.failure == 0);

    // a mutating request with an older generation changes nothing, whatever it says
    q.op = BB_ST_OP_WAKE; q.stage = 7; q.generation = 8;
    uint32_t act = s.c.activity;
    bb_sb_handle(&s.c, &q, 13, bb_sb_entry_seq(&s.c), &f, &rp, &fx);
    assert(!rp.ready && rp.failure == BB_SB_FAIL_STALE && rp.generation == 9);
    assert(s.c.state == BB_ST_ASLEEP && fx.run_stage == 0 && s.c.activity == act);
    puts("late");
    return 0;
}
"""
    assert "late" in _run(body)


def test_repeated_cycles_hold_invariants():
    body = r"""
int main(void) {
    sim_t s; sim_init(&s);
    uint32_t gen = 0xFFFFFFF0u;                                   // crosses the 32-bit wrap
    uint32_t last_act = 0;
    for (int i = 0; i < 64; ++i) {
        clear_ops(&s);
        gen++;
        sleep_all(&s, gen);
        assert(count_op(&s, OP_RAIL0) == 1 && count_op(&s, OP_RAIL1) == 1 && count_op(&s, OP_RAIL2) == 1);
        assert(count_op(&s, OP_LED_DARK) == 1 && s.c.hw_paused && s.c.barrier && s.c.leds_dark);
        assert(!bb_sb_admit(&s.c));
        gen++;
        wake_all(&s, gen);
        assert(!s.c.barrier && !s.c.hw_paused && !s.c.leds_dark && !s.c.wake_pending);
        assert(s.c.state == BB_ST_ACTIVE && s.c.inflight == 0);
        assert(bb_sb_admit(&s.c)); bb_sb_leave(&s.c);
        assert(s.c.activity >= last_act); last_act = s.c.activity;
        assert(s.lock_violations == 0 && s.depth == 0);
        if (i % 7 == 3) {                                         // interleave cancelled prepares
            gen++;
            bb_standby_reply_t r = slp(&s, 1, gen); assert(r.ready);
            gen++;
            for (uint8_t st = 7; st <= 11; ++st) { r = wak(&s, st, gen); assert(r.ready); }
            assert(s.c.state == BB_ST_ACTIVE);
        }
    }
    puts("repeat");
    return 0;
}
"""
    assert "repeat" in _run(body)


def test_new_work_while_asleep_is_refused_counted_and_signalled():
    body = r"""
int main(void) {
    sim_t s; sim_init(&s);
    bb_sb_fx_t fx;
    sleep_all(&s, 20);
    uint32_t a0 = s.c.activity;
    bb_standby_reply_t r = poll(&s);
    assert(r.state == BB_ST_ASLEEP && r.inhibitors == 0 && r.activity == a0);   // idle and honest

    assert(!bb_sb_admit(&s.c));                                   // a command that needs hardware
    assert(s.c.activity == a0 + 1 && s.c.wake_pending && s.c.inflight == 0);
    assert(!bb_sb_dap_enter(&s.c, s.now));                        // a DAP frame
    assert(!bb_sb_dap_enter(&s.c, s.now));
    assert(s.c.activity == a0 + 3);
    r = poll(&s);                                                 // POLL reports it in ANY state
    assert(r.state == BB_ST_ASLEEP && (r.inhibitors & BB_ST_INH_WORK) && r.activity == a0 + 3);
    assert(s.c.dap_frames == 0 && s.c.dap_inflight == 0 && !s.c.dap_open);

    // the HAT IRQ is pulsed once, then again only after the repeat period
    bb_sb_tick(&s.c, 5000, &s.facts, &fx); assert(fx.irq);
    bb_sb_tick(&s.c, 5001, &s.facts, &fx); assert(!fx.irq);
    bb_sb_tick(&s.c, 5000 + BB_SB_IRQ_REPEAT_MS - 1, &s.facts, &fx); assert(!fx.irq);
    bb_sb_tick(&s.c, 5000 + BB_SB_IRQ_REPEAT_MS, &s.facts, &fx); assert(fx.irq);

    // note_activity while blocked also requests a wake; while ACTIVE it only counts
    sim_init(&s);
    bb_sb_note_activity(&s.c); assert(s.c.activity == 1 && !s.c.wake_pending);
    sleep_all(&s, 3);
    bb_sb_note_activity(&s.c); assert(s.c.wake_pending);
    wake_all(&s, 4);
    assert(!s.c.wake_pending);
    bb_sb_tick(&s.c, 9000, &s.facts, &fx); assert(!fx.irq);       // nothing left to signal
    puts("asleep-work");
    return 0;
}
"""
    assert "asleep-work" in _run(body)


def test_dap_logical_session_lease():
    body = r"""
int main(void) {
    sim_t s; sim_init(&s);
    bb_sb_fx_t fx;
    uint32_t t = 100000;
    uint32_t a;

    // frames that never open a port (enumeration, DAP_Info probes) inhibit nothing
    assert(bb_sb_dap_enter(&s.c, t));
    assert((bb_sb_inhibitors(&s.c, &s.facts, t) & BB_ST_INH_SWD) != 0);   // while the frame runs
    bb_sb_dap_leave(&s.c, t, false);
    assert(!s.c.dap_open && bb_sb_inhibitors(&s.c, &s.facts, t) == 0 && s.c.dap_connects == 0);

    // DAP_Connect opens a logical session: it inhibits, counts, and is activity
    a = s.c.activity;
    assert(bb_sb_dap_enter(&s.c, t)); bb_sb_dap_leave(&s.c, t, true);
    assert(s.c.dap_open && s.c.dap_connects == 1 && s.c.activity == a + 1);
    assert(bb_sb_inhibitors(&s.c, &s.facts, t + 1) & BB_ST_INH_SWD);
    s.now = t + 1;
    bb_standby_reply_t r = call(&s, BB_ST_OP_SLEEP, 1, 5);
    assert(!r.ready && r.failure == BB_SB_FAIL_BUSY && (r.inhibitors & BB_ST_INH_SWD));
    assert(s.nops == 0);

    // any frame refreshes the lease; the bound is measured from the LAST frame
    uint32_t t2 = t + BB_SB_DAP_TTL_MS - 1;
    assert(bb_sb_dap_enter(&s.c, t2)); bb_sb_dap_leave(&s.c, t2, true);
    assert(s.c.dap_connects == 1 && s.c.dap_frames == 3);
    assert(bb_sb_inhibitors(&s.c, &s.facts, t2 + BB_SB_DAP_TTL_MS - 1) & BB_ST_INH_SWD);
    assert(!(bb_sb_inhibitors(&s.c, &s.facts, t2 + BB_SB_DAP_TTL_MS) & BB_ST_INH_SWD));  // lapsed

    // the periodic tick retires the lapsed lease and counts it as a released host
    a = s.c.activity;
    bb_sb_tick(&s.c, t2 + BB_SB_DAP_TTL_MS - 1, &s.facts, &fx); assert(s.c.dap_open);
    bb_sb_tick(&s.c, t2 + BB_SB_DAP_TTL_MS, &s.facts, &fx);
    assert(!s.c.dap_open && s.c.dap_expired == 1 && s.c.activity == a + 1);

    // DAP_Disconnect releases immediately and restarts the idle interval
    assert(bb_sb_dap_enter(&s.c, t2)); bb_sb_dap_leave(&s.c, t2, true);
    a = s.c.activity;
    assert(bb_sb_dap_enter(&s.c, t2 + 5)); bb_sb_dap_leave(&s.c, t2 + 5, false);
    assert(!s.c.dap_open && s.c.dap_disconnects == 1 && s.c.activity == a + 1);
    assert(!(bb_sb_inhibitors(&s.c, &s.facts, t2 + 6) & BB_ST_INH_SWD));

    // the USB host vanishing ends the session without a DAP_Disconnect
    assert(bb_sb_dap_enter(&s.c, t2)); bb_sb_dap_leave(&s.c, t2, true);
    a = s.c.activity;
    bb_sb_dap_host_gone(&s.c);
    assert(!s.c.dap_open && s.c.activity == a + 1);
    bb_sb_dap_host_gone(&s.c); assert(s.c.activity == a + 1);      // idempotent

    // an in-flight frame is never expired from under itself
    sim_init(&s);
    assert(bb_sb_dap_enter(&s.c, 10)); bb_sb_dap_leave(&s.c, 10, true);
    assert(bb_sb_dap_enter(&s.c, 10));
    bb_sb_tick(&s.c, 10 + 10 * BB_SB_DAP_TTL_MS, &s.facts, &fx);
    assert(s.c.dap_open && s.c.dap_expired == 0);
    bb_sb_dap_leave(&s.c, 10 + 10 * BB_SB_DAP_TTL_MS, true);

    // time wrap: a session opened just before the counter wraps still expires on time
    sim_init(&s);
    uint32_t w = 0xFFFFFFFFu - 100u;
    assert(bb_sb_dap_enter(&s.c, w)); bb_sb_dap_leave(&s.c, w, true);
    assert(bb_sb_inhibitors(&s.c, &s.facts, w + 1000u) & BB_ST_INH_SWD);                 // across the wrap
    assert(!(bb_sb_inhibitors(&s.c, &s.facts, w + BB_SB_DAP_TTL_MS) & BB_ST_INH_SWD));

    // a session can't be opened or kept by frames the barrier refused
    sim_init(&s);
    sleep_all(&s, 3);
    assert(!bb_sb_dap_enter(&s.c, 500)); assert(!s.c.dap_open && s.c.dap_connects == 0);
    puts("dap");
    return 0;
}
"""
    assert "dap" in _run(body)


def test_activity_edges_timeouts_and_monitor_gate():
    body = r"""
int main(void) {
    sim_t s; sim_init(&s);
    bb_sb_fx_t fx;

    // owner start/finish are activity; the command queue depth and steady state are not
    bb_sb_tick(&s.c, 10, &s.facts, &fx);                          // first sight baselines
    assert(s.c.activity == 0);
    bb_sb_tick(&s.c, 11, &s.facts, &fx); assert(s.c.activity == 0);
    s.facts.cmd_pending = true;
    for (int i = 0; i < 20; ++i) { bb_sb_tick(&s.c, 12 + i, &s.facts, &fx); }
    assert(s.c.activity == 0);                                    // S3 polling must not keep it awake
    s.facts.cmd_pending = false;
    s.facts.la_streaming = true;  bb_sb_tick(&s.c, 40, &s.facts, &fx); assert(s.c.activity == 1);
    bb_sb_tick(&s.c, 41, &s.facts, &fx);                          assert(s.c.activity == 1);
    s.facts.la_streaming = false; bb_sb_tick(&s.c, 42, &s.facts, &fx); assert(s.c.activity == 2);
    s.facts.fw_update = true;     bb_sb_tick(&s.c, 43, &s.facts, &fx); assert(s.c.activity == 3);
    s.facts.fw_update = false;    bb_sb_tick(&s.c, 44, &s.facts, &fx); assert(s.c.activity == 4);

    // PREPARING with a silent S3 returns to ACTIVE; the deadline is exact and S3 contact resets it
    sim_init(&s);
    bb_standby_reply_t r;
    s.now = 1000; r = slp(&s, 1, 8); assert(r.ready && s.c.state == BB_ST_PREPARING);
    bb_sb_tick(&s.c, 1000 + BB_SB_PREPARE_TIMEOUT_MS - 1, &s.facts, &fx);
    assert(s.c.state == BB_ST_PREPARING);
    s.now = 1000 + BB_SB_PREPARE_TIMEOUT_MS - 1; r = poll(&s);    // the S3 is alive: restart the clock
    bb_sb_tick(&s.c, 1000 + BB_SB_PREPARE_TIMEOUT_MS, &s.facts, &fx);
    assert(s.c.state == BB_ST_PREPARING);
    uint32_t a = s.c.activity;
    bb_sb_tick(&s.c, 1000 + 2 * BB_SB_PREPARE_TIMEOUT_MS - 1, &s.facts, &fx);
    assert(s.c.state == BB_ST_ACTIVE && !s.c.barrier && !s.c.wake_pending && s.c.activity == a + 1);
    assert(s.nops == 0 && bb_sb_admit(&s.c));                     // usable again, nothing was powered down
    bb_sb_leave(&s.c);

    // an ASLEEP/WAKING/FAULT_SAFE participant is never auto-cancelled: its outputs are safe and stay so
    sim_init(&s);
    sleep_all(&s, 5);
    bb_sb_tick(&s.c, 10 * BB_SB_PREPARE_TIMEOUT_MS, &s.facts, &fx);
    assert(s.c.state == BB_ST_ASLEEP && s.c.barrier);

    // monitor gate
    sim_init(&s);
    assert(bb_sb_monitor_allowed(&s.c));
    slp(&s, 1, 2); assert(!bb_sb_monitor_allowed(&s.c));
    slp(&s, 2, 2); assert(!bb_sb_monitor_allowed(&s.c));
    wak(&s, 7, 3); assert(!bb_sb_monitor_allowed(&s.c));
    wak(&s, 8, 3); wak(&s, 9, 3); wak(&s, 10, 3); assert(!bb_sb_monitor_allowed(&s.c));
    wak(&s, 11, 3); assert(bb_sb_monitor_allowed(&s.c));
    sim_init(&s);
    slp(&s, 1, 2); s.fail_op = OP_RAIL2; slp(&s, 2, 2);
    assert(s.c.state == BB_ST_FAULT_SAFE && !bb_sb_monitor_allowed(&s.c));
    puts("edges");
    return 0;
}
"""
    assert "edges" in _run(body)


@pytest.mark.parametrize("name", ["bb_standby_core.c"])
def test_core_has_no_sdk_dependency(name):
    src = read_source(f"{RP_SRC}/{name}") + read_source(f"{RP_SRC}/bb_standby_core.h")
    for banned in ("pico/", "hardware/", "tusb", "FreeRTOS", "bb_la_log", "write_clear"):
        assert banned not in src, f"{banned} must stay out of the pure core"
