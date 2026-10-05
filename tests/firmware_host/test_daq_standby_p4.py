"""Host execution of the DAQ P4 standby participant (wire/ack/race/input gate/analog stages)."""

from tests.firmware_host.fwhost import compile_and_run

P4_STANDBY = "Firmware/DAQ_HAT/ESP32P4/src/standby"
COMMON = "Firmware/DAQ_HAT/common"

MAIN = r"""
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "standby_p4_core.h"

static bb_standby_request_t rq(uint8_t op, uint8_t stage, uint32_t gen) {
    bb_standby_request_t r;
    memset(&r, 0, sizeof(r));
    r.schema = BB_STANDBY_SCHEMA; r.op = op; r.stage = stage; r.generation = gen;
    r.timeout_seconds = 300;
    return r;
}

static bb_standby_reply_t call(sb_p4_t *c, bb_standby_request_t r, uint32_t now,
                               uint32_t inh, sb_p4_fx_t *fx_out) {
    bb_standby_reply_t rp;
    sb_p4_fx_t fx;
    sb_p4_handle(c, &r, now, sb_p4_entry_seq(c), inh, &rp, &fx);
    if (fx_out) *fx_out = fx;
    return rp;
}

static bb_standby_reply_t sleep_stage(sb_p4_t *c, uint8_t st, uint32_t gen, uint32_t now,
                                      sb_p4_fx_t *fx) {
    return call(c, rq(BB_ST_OP_SLEEP, st, gen), now, 0, fx);
}
static bb_standby_reply_t wake_stage(sb_p4_t *c, uint8_t st, uint32_t gen, uint32_t now,
                                     sb_p4_fx_t *fx) {
    return call(c, rq(BB_ST_OP_WAKE, st, gen), now, 0, fx);
}

static bb_standby_reply_t c6_reply(uint32_t gen, uint8_t state, uint8_t ready,
                                   uint32_t activity, uint32_t inh) {
    bb_standby_reply_t r;
    memset(&r, 0, sizeof(r));
    r.schema = BB_STANDBY_SCHEMA; r.state = state; r.ready = ready; r.generation = gen;
    r.activity = activity; r.inhibitors = inh;
    return r;
}

// Run a worker stage the way the ctrl task does. On success the S3's re-sent request is
// answered (ready); on failure the P4's state is reported (a re-sent request would retry).
static bb_standby_reply_t run_worker(sb_p4_t *c, bool sleep_op, uint8_t st, uint32_t gen,
                                     uint32_t now, bool ok) {
    sb_p4_fx_t fx;
    uint8_t op = sleep_op ? BB_ST_OP_SLEEP : BB_ST_OP_WAKE;
    bb_standby_reply_t r = call(c, rq(op, st, gen), now, 0, &fx);
    if (!(!r.ready && fx.run_stage == st && fx.run_generation == gen)) {
        fprintf(stderr, "run_worker stage %u gen %u: ready=%u failure=%u state=%u run_stage=%u\n", st, gen,
                r.ready, r.failure, r.state, fx.run_stage);
    }
    assert(!r.ready && fx.run_stage == st && fx.run_generation == gen);
    assert(sb_p4_step_begin(c, st, gen));
    sb_p4_step_complete(c, st, gen, ok, true);
    if (!ok) return call(c, rq(BB_ST_OP_POLL, 0, 0), now + 1, 0, &fx);
    return call(c, rq(op, st, gen), now + 1, 0, &fx);
}

// Drive a complete sleep to ASLEEP; C6 present and answering.
static void go_asleep(sb_p4_t *c, uint32_t gen, uint32_t t) {
    sb_p4_fx_t fx;
    bb_standby_reply_t r;
    sb_p4_set_c6(c, true, true);
    r = sleep_stage(c, 1, gen, t, &fx);
    assert(r.ready && r.state == BB_ST_PREPARING && r.generation == gen);
    r = run_worker(c, true, 2, gen, t, true);
    assert(r.ready && r.state == BB_ST_ASLEEP && c->barrier);
    assert(!c->routes_held && !c->analog_cut);
    r = run_worker(c, true, 3, gen, t, true);                   // muxes disconnected, every rail still on
    assert(r.ready && c->routes_held && !c->analog_cut);
    r = sleep_stage(c, 4, gen, t, &fx); assert(r.ready && fx.run_stage == 0);
    assert(!c->analog_cut);
    r = run_worker(c, true, 5, gen, t, true);                   // the DAQ analog rails
    assert(r.ready && c->analog_cut);
    r = sleep_stage(c, 6, gen, t, &fx);
    assert(!r.ready && fx.forward_c6 && fx.forward_stage == 6);
    bb_standby_reply_t cr = c6_reply(gen, BB_ST_ASLEEP, 1, 0, 0);
    sb_p4_c6_reply(c, &cr, t + 5);
    r = sleep_stage(c, 6, gen, t + 6, &fx);
    assert(r.ready && r.state == BB_ST_ASLEEP);
}

static void go_awake(sb_p4_t *c, uint32_t gen, uint32_t t) {
    sb_p4_fx_t fx;
    bb_standby_reply_t r = wake_stage(c, 7, gen, t, &fx);
    assert(!r.ready && fx.forward_c6 && fx.forward_stage == 7 && r.state == BB_ST_WAKING);
    bb_standby_reply_t cr = c6_reply(gen, BB_ST_WAKING, 1, 0, 0);
    sb_p4_c6_reply(c, &cr, t + 2);
    assert(wake_stage(c, 7, gen, t + 3, &fx).ready);
    if (c->analog_cut) {
        r = wake_stage(c, 9, gen, t + 3, &fx);                  // converters before the rails: refused
        assert(!r.ready && r.failure == SB_FAIL_ORDER && fx.run_stage == 0);
        r = run_worker(c, false, 8, gen, t + 3, true); assert(r.ready);
        r = wake_stage(c, 10, gen, t + 3, &fx);                 // hat wake before the converters: refused
        assert(!r.ready && r.failure == SB_FAIL_ORDER && fx.run_stage == 0);
        r = run_worker(c, false, 9, gen, t + 3, true); assert(r.ready && !c->analog_cut);
    } else {
        r = wake_stage(c, 8, gen, t + 3, &fx); assert(r.ready && fx.run_stage == 0);
        r = wake_stage(c, 9, gen, t + 3, &fx); assert(r.ready && fx.run_stage == 0);
    }
    r = wake_stage(c, 10, gen, t + 3, &fx);
    assert(!r.ready && fx.run_stage == 10 && sb_p4_blocked(c));
    sb_p4_step_complete(c, 10, gen, true, false);
    assert(wake_stage(c, 10, gen, t + 4, &fx).ready);
    r = wake_stage(c, 11, gen, t + 4, &fx);
    assert(!r.ready && fx.forward_c6 && fx.forward_stage == 11);
    cr = c6_reply(gen, BB_ST_ACTIVE, 1, 0, 0);
    sb_p4_c6_reply(c, &cr, t + 6);
    r = wake_stage(c, 11, gen, t + 7, &fx);
    assert(r.ready && r.state == BB_ST_ACTIVE && !sb_p4_blocked(c));
}

static uint8_t lease(sb_p4_t *c, uint8_t op, uint32_t id, uint32_t ttl, uint32_t now) {
    return sb_p4_lease(c, op, id, ttl, now, NULL);
}

int main(void) {
    sb_p4_t c;
    sb_p4_fx_t fx;
    bb_standby_reply_t r;

    assert(sizeof(bb_standby_request_t) == 16 && sizeof(bb_standby_reply_t) == 16);
    assert(BB_HAT_CMD_STANDBY == 0x7C && BB_HAT_RSP_STANDBY == 0x9C);

    // ---- full cycle, generation echo, idempotent duplicates ----------------
    sb_p4_init(&c);
    sb_p4_set_c6(&c, true, true);
    r = call(&c, rq(BB_ST_OP_POLL, 0, 77), 10, 0, &fx);
    assert(r.state == BB_ST_ACTIVE && r.ready && r.generation == 0);   // POLL never adopts a gen
    go_asleep(&c, 100, 1000);
    assert(c.hw_paused && c.fast_was_running && c.barrier && c.analog_cut);
    r = sleep_stage(&c, 6, 100, 1100, &fx);                            // duplicate of a done stage
    assert(r.ready && !fx.forward_c6 && r.generation == 100);
    r = sleep_stage(&c, 5, 100, 1100, &fx);                            // ... and of the rail cut
    assert(r.ready && fx.run_stage == 0);
    go_awake(&c, 101, 2000);
    assert(!c.hw_paused && !c.barrier && !c.wake_pending && !c.c6_dark && !c.analog_cut);
    puts("full-cycle/idempotent/echo");

    // ---- stale and out-of-order generations --------------------------------
    r = call(&c, rq(BB_ST_OP_SLEEP, 1, 100), 3000, 0, &fx);            // older than 101
    assert(!r.ready && r.failure == SB_FAIL_STALE && r.generation == 101 && c.state == BB_ST_ACTIVE);
    r = call(&c, rq(BB_ST_OP_WAKE, 7, 0xFFFFFFF0u), 3000, 0, &fx);     // modular: older
    assert(r.failure == SB_FAIL_STALE);
    sb_p4_init(&c);
    c.generation = 0xFFFFFFFEu; c.gen_valid = true;
    r = call(&c, rq(BB_ST_OP_SLEEP, 1, 2), 5, 0, &fx);                 // wraps forward
    assert(r.ready && r.generation == 2);
    sb_p4_init(&c);
    r = call(&c, rq(BB_ST_OP_SLEEP, 9, 5), 0, 0, &fx);                 // wrong stage for op
    assert(r.failure == SB_FAIL_ARG);
    { bb_standby_request_t bad = rq(BB_ST_OP_SLEEP, 1, 5); bad.schema = 9;
      r = call(&c, bad, 0, 0, &fx); assert(r.failure == SB_FAIL_ARG); }
    { bb_standby_request_t bad = rq(7, 1, 5);
      r = call(&c, bad, 0, 0, &fx); assert(r.failure == SB_FAIL_ARG); }
    puts("stale/wrap/arg");

    // ---- S3 boot rebases the generation even when the counter is higher ----
    sb_p4_init(&c);
    go_asleep(&c, 5000, 0);
    sb_p4_set_c6(&c, false, false);
    bb_standby_request_t boot = rq(BB_ST_OP_PROGRESS, BB_ST_BOOT_STAGE, 3);
    boot.completed = BB_ST_BOOT_IO | BB_ST_BOOT_P4;                    // the S3 tries to claim the Analyzer
    boot.failed = BB_ST_BOOT_WIFI;
    r = call(&c, boot, 10, 0, &fx);
    assert(r.ready && r.generation == 3 && fx.forward_c6 && fx.forward_stage == 0);
    assert(c.boot_completed == BB_ST_BOOT_IO && c.boot_failed == BB_ST_BOOT_WIFI);
    r = wake_stage(&c, 7, 4, 20, &fx);                                  // new epoch is orderable
    assert(r.ready && r.state == BB_ST_WAKING && r.generation == 4);
    bb_standby_request_t mirror;
    sb_p4_build_c6_request(&c, 0, NULL, &mirror);
    assert(mirror.op == BB_ST_OP_POLL && mirror.stage == BB_ST_BOOT_STAGE);
    assert(mirror.completed == BB_ST_BOOT_IO && mirror.timeout_seconds == 300);   // P4 bit still pending
    sb_p4_set_boot_p4(&c, true);                                        // the P4's own bring-up succeeded
    sb_p4_build_c6_request(&c, 0, NULL, &mirror);
    assert(mirror.completed == (BB_ST_BOOT_IO | BB_ST_BOOT_P4) && !(mirror.failed & BB_ST_BOOT_P4));
    sb_p4_set_boot_p4(&c, false);                                       // ... or did not
    sb_p4_build_c6_request(&c, 0, NULL, &mirror);
    assert(mirror.completed == BB_ST_BOOT_IO && (mirror.failed & BB_ST_BOOT_P4));
    puts("boot-rebase/mirror");

    // ---- Analyzer milestone is the P4's own, with or without an S3 report ---
    sb_p4_init(&c);
    sb_p4_build_c6_request(&c, 0, NULL, &mirror);
    assert(mirror.stage == 0 && mirror.completed == 0 && mirror.failed == 0);   // pending: say nothing
    sb_p4_set_boot_p4(&c, true);
    sb_p4_build_c6_request(&c, 0, NULL, &mirror);
    assert(mirror.stage == SB_STAGE_BOOT_P4_ONLY && mirror.completed == BB_ST_BOOT_P4 && !mirror.failed);
    sb_p4_set_boot_p4(&c, false);
    sb_p4_build_c6_request(&c, 0, NULL, &mirror);
    assert(mirror.stage == SB_STAGE_BOOT_P4_ONLY && mirror.completed == 0 && mirror.failed == BB_ST_BOOT_P4);
    puts("boot-p4-truth");

    // ---- BUSY: inhibitor at quiesce, in-flight work, entry race at barrier --
    sb_p4_init(&c);
    r = call(&c, rq(BB_ST_OP_SLEEP, 1, 10), 0, BB_ST_INH_STREAM, &fx);
    assert(!r.ready && r.failure == SB_FAIL_BUSY && c.state == BB_ST_ACTIVE
           && (r.inhibitors & BB_ST_INH_STREAM));
    r = call(&c, rq(BB_ST_OP_SLEEP, 1, 10), 1, 0, &fx);                 // inhibitor gone: retry works
    assert(r.ready && c.state == BB_ST_PREPARING);

    assert(!sb_p4_admit(&c));                                           // PREPARING rejects work
    assert(c.wake_pending);
    r = call(&c, rq(BB_ST_OP_SLEEP, 2, 10), 2, 0, &fx);                 // rejected work refuses the barrier
    assert(!r.ready && r.failure == SB_FAIL_BUSY && !c.barrier && fx.run_stage == 0);
    assert(r.inhibitors & BB_ST_INH_WORK);

    sb_p4_init(&c);
    assert(sb_p4_admit(&c));                                            // in flight at quiesce
    r = call(&c, rq(BB_ST_OP_SLEEP, 1, 10), 0, 0, &fx);
    assert(r.failure == SB_FAIL_BUSY && (r.inhibitors & BB_ST_INH_WORK));
    sb_p4_leave(&c);
    r = call(&c, rq(BB_ST_OP_SLEEP, 1, 10), 1, 0, &fx);
    assert(r.ready);

    // entry between snapshot and barrier: sample the seq, let work in, then stage 2
    sb_p4_init(&c);
    call(&c, rq(BB_ST_OP_SLEEP, 1, 10), 0, 0, &fx);
    c.state = BB_ST_PREPARING;
    uint32_t seq = sb_p4_entry_seq(&c);
    c.entered_seq++;                                                    // work slipped in after the snapshot
    { bb_standby_request_t q = rq(BB_ST_OP_SLEEP, 2, 10);
      sb_p4_handle(&c, &q, 5, seq, 0, &r, &fx); }
    assert(r.failure == SB_FAIL_BUSY && !c.barrier && fx.run_stage == 0);
    puts("busy/race");

    // ---- admission: blocked states reject and request wake -----------------
    sb_p4_init(&c);
    assert(sb_p4_admit(&c) && c.inflight == 1);
    sb_p4_leave(&c);
    sb_p4_leave(&c);                                                    // underflow is harmless
    assert(c.inflight == 0);
    go_asleep(&c, 20, 0);
    uint32_t a1 = c.activity;
    assert(!sb_p4_admit(&c) && c.activity == a1 + 1 && c.wake_pending);
    r = call(&c, rq(BB_ST_OP_POLL, 0, 0), 9, 0, &fx);
    assert(r.state == BB_ST_ASLEEP && (r.inhibitors & BB_ST_INH_WORK) && r.activity == a1 + 1);
    go_awake(&c, 21, 100);
    assert(sb_p4_admit(&c));
    sb_p4_leave(&c);
    // admitted work never bumps activity: background readers cannot hold the idle timer up
    { uint32_t act = c.activity;
      for (int i = 0; i < 50; ++i) { assert(sb_p4_admit(&c)); sb_p4_leave(&c); }
      for (int i = 0; i < 50; ++i) { assert(sb_p4_admit_quiet(&c)); sb_p4_leave(&c); }
      assert(c.activity == act && c.inflight == 0); }
    // queued work that was already admitted: refused silently, never a wake request
    sb_p4_init(&c);
    assert(sb_p4_admit_quiet(&c) && c.inflight == 1);
    sb_p4_leave(&c);
    go_asleep(&c, 22, 200);
    uint32_t aq = c.activity;
    assert(!sb_p4_admit_quiet(&c) && c.activity == aq && !c.wake_pending);
    puts("admission");

    // ---- worker failures end in FAULT_SAFE; a later wake recovers ----------
    sb_p4_init(&c);
    sb_p4_set_c6(&c, true, true);
    call(&c, rq(BB_ST_OP_SLEEP, 1, 30), 0, 0, &fx);
    call(&c, rq(BB_ST_OP_SLEEP, 2, 30), 0, 0, &fx);
    sb_p4_step_complete(&c, 2, 30, false, true);
    r = call(&c, rq(BB_ST_OP_SLEEP, 2, 30), 1, 0, &fx);
    assert(!r.ready && r.state == BB_ST_FAULT_SAFE && r.failure == SB_FAIL_HW && c.barrier);
    assert(c.steps_failed == (1u << 2));
    assert(!sb_p4_admit(&c));
    r = call(&c, rq(BB_ST_OP_POLL, 0, 0), 2, 0, &fx);
    assert(!r.ready && r.state == BB_ST_FAULT_SAFE);
    r = wake_stage(&c, 7, 31, 10, &fx);
    assert(r.state == BB_ST_WAKING);
    r = wake_stage(&c, 8, 31, 11, &fx); assert(r.ready);                // the rails were never cut
    r = wake_stage(&c, 9, 31, 11, &fx); assert(r.ready);
    r = wake_stage(&c, 10, 31, 11, &fx);
    assert(fx.run_stage == 10);                                         // hw_paused was set conservatively
    sb_p4_step_complete(&c, 10, 31, false, false);
    assert(c.state == BB_ST_FAULT_SAFE && c.failure == SB_FAIL_HW && c.hw_paused);
    r = wake_stage(&c, 10, 31, 12, &fx);                                // a re-sent stage retries the resume
    assert(!r.ready && fx.run_stage == 10 && r.state == BB_ST_FAULT_SAFE);
    sb_p4_step_complete(&c, 10, 31, true, false);
    r = wake_stage(&c, 10, 31, 13, &fx);
    assert(r.ready && r.state == BB_ST_WAKING && !c.hw_paused);
    // a worker that never reports back is bounded
    sb_p4_init(&c);
    call(&c, rq(BB_ST_OP_SLEEP, 1, 40), 0, 0, &fx);
    call(&c, rq(BB_ST_OP_SLEEP, 2, 40), 0, 0, &fx);
    assert(!sb_p4_tick(&c, SB_WORKER_TIMEOUT_MS - 1) && c.step_running == 2);
    sb_p4_tick(&c, SB_WORKER_TIMEOUT_MS);
    assert(c.step_running == 0 && c.state == BB_ST_FAULT_SAFE && c.failure == SB_FAIL_HW);
    puts("fault-safe");

    // ---- analog stages: failure, partial recovery, ordering -----------------
    // The rail cut fails: nothing sleeps further, the cut is remembered, a wake redoes it all.
    sb_p4_init(&c);
    sb_p4_set_c6_expected(&c, false);                                   // a board variant without a display
    sb_p4_set_c6(&c, false, false);
    call(&c, rq(BB_ST_OP_SLEEP, 1, 80), 0, 0, &fx);
    run_worker(&c, true, 2, 80, 0, true);
    run_worker(&c, true, 3, 80, 0, true); sleep_stage(&c, 4, 80, 0, &fx);
    r = run_worker(&c, true, 5, 80, 0, false);
    assert(!r.ready && r.state == BB_ST_FAULT_SAFE && r.failure == SB_FAIL_HW && c.analog_cut);
    assert(c.steps_failed == (1u << 5) && c.barrier);
    r = sleep_stage(&c, 6, 80, 5, &fx);                                 // nothing may go on sleeping
    assert(!r.ready && r.failure == SB_FAIL_HW && fx.forward_c6 == false);
    r = wake_stage(&c, 7, 81, 10, &fx);
    assert(r.ready && r.state == BB_ST_WAKING && c.steps_failed == 0);
    // power-on fails half way (PG, a rail): the stage is retried and may be retried again
    r = run_worker(&c, false, 8, 81, 11, false);
    assert(!r.ready && r.state == BB_ST_FAULT_SAFE && c.steps_failed == (1u << 8) && c.barrier);
    r = wake_stage(&c, 9, 81, 12, &fx);                                 // converters before rails: refused
    assert(!r.ready && r.failure == SB_FAIL_ORDER && fx.run_stage == 0);
    r = run_worker(&c, false, 8, 81, 13, true);                         // the retry runs the worker again
    assert(r.ready && c.steps_failed == 0);
    r = run_worker(&c, false, 9, 81, 14, false);                        // configure fails
    assert(!r.ready && r.state == BB_ST_FAULT_SAFE && c.analog_cut);
    r = wake_stage(&c, 10, 81, 15, &fx);                                // the converters are not back
    assert(!r.ready && r.failure == SB_FAIL_ORDER && fx.run_stage == 0 && c.barrier);
    r = run_worker(&c, false, 9, 81, 16, true);
    assert(r.ready && !c.analog_cut);
    r = run_worker(&c, false, 10, 81, 17, true); assert(r.ready && c.state == BB_ST_WAKING);
    r = wake_stage(&c, 11, 81, 18, &fx);
    assert(r.ready && r.state == BB_ST_ACTIVE && !sb_p4_blocked(&c));
    puts("analog-recovery");

    // ---- a late worker can never advance a newer transaction -----------------
    sb_p4_init(&c);
    sb_p4_set_c6_expected(&c, false);
    sb_p4_set_c6(&c, false, false);
    call(&c, rq(BB_ST_OP_SLEEP, 1, 90), 0, 0, &fx);
    r = call(&c, rq(BB_ST_OP_SLEEP, 2, 90), 1, 0, &fx);
    assert(fx.run_stage == 2 && fx.run_generation == 90);
    assert(!sb_p4_step_begin(&c, 2, 89) && !sb_p4_step_begin(&c, 5, 90) && sb_p4_step_begin(&c, 2, 90));
    sb_p4_step_complete(&c, 2, 89, true, true);                         // wrong generation
    assert(c.step_running == 2 && !c.hw_paused && c.state == BB_ST_PREPARING);
    sb_p4_step_complete(&c, 5, 90, true, true);                         // wrong stage
    assert(c.step_running == 2 && c.state == BB_ST_PREPARING);
    // S3 reboots while the worker is queued: new epoch, the old worker is abandoned
    { bb_standby_request_t b2 = rq(BB_ST_OP_PROGRESS, BB_ST_BOOT_STAGE, 7);
      call(&c, b2, 2, 0, &fx); }
    assert(c.step_running == 0 && c.generation == 7 && c.hw_paused);    // it may have run: wake must undo it
    sb_p4_step_complete(&c, 2, 90, true, true);                         // the late completion
    assert(c.state == BB_ST_PREPARING && c.steps_done == 0);
    assert(!sb_p4_step_begin(&c, 2, 90));
    r = wake_stage(&c, 7, 8, 3, &fx);                                   // the next transaction starts cleanly
    assert(r.ready && r.generation == 8);
    // a newer SLEEP/WAKE generation also supersedes a stage in flight
    sb_p4_init(&c);
    sb_p4_set_c6_expected(&c, false);
    sb_p4_set_c6(&c, false, false);
    call(&c, rq(BB_ST_OP_SLEEP, 1, 95), 0, 0, &fx);
    call(&c, rq(BB_ST_OP_SLEEP, 2, 95), 1, 0, &fx);
    r = wake_stage(&c, 7, 96, 2, &fx);
    assert(r.ready && c.step_running == 0 && c.hw_paused && c.generation == 96);
    sb_p4_step_complete(&c, 2, 95, true, true);
    assert(c.state == BB_ST_WAKING);
    puts("stale-worker");

    // ---- an old C6 (linked, never answers) is an inhibitor, not "absent" ----
    sb_p4_init(&c);
    sb_p4_set_c6(&c, true, false);
    assert(c.c6_unsupported && !c.c6_present);
    r = call(&c, rq(BB_ST_OP_SLEEP, 1, 11), 0, 0, &fx);
    assert(!r.ready && r.failure == SB_FAIL_BUSY && (r.inhibitors & BB_ST_INH_HAT_UNKNOWN));
    r = call(&c, rq(BB_ST_OP_POLL, 0, 0), 1, 0, &fx);
    assert(r.inhibitors & BB_ST_INH_HAT_UNKNOWN);
    sb_p4_set_c6(&c, false, false);                                     // never linked, display expected: fail closed
    assert(c.c6_missing && !c.c6_unsupported);
    r = call(&c, rq(BB_ST_OP_POLL, 0, 0), 2, 0, &fx);
    assert(r.inhibitors & BB_ST_INH_HAT_UNKNOWN);
    r = call(&c, rq(BB_ST_OP_SLEEP, 1, 13), 2, 0, &fx);
    assert(!r.ready && r.failure == SB_FAIL_BUSY && c.state == BB_ST_ACTIVE);
    sb_p4_set_c6_expected(&c, false);                                   // explicit no-display variant: truly absent
    r = call(&c, rq(BB_ST_OP_POLL, 0, 0), 2, 0, &fx);
    assert(!(r.inhibitors & BB_ST_INH_HAT_UNKNOWN));
    sb_p4_set_c6_expected(&c, true);
    sb_p4_set_c6(&c, true, true);                                       // new C6: answers
    r = call(&c, rq(BB_ST_OP_POLL, 0, 0), 3, 0, &fx);
    assert(!(r.inhibitors & BB_ST_INH_HAT_UNKNOWN));
    // a C6 that goes silent after the sleep began cannot be assumed dark
    sb_p4_init(&c);
    call(&c, rq(BB_ST_OP_SLEEP, 1, 12), 0, 0, &fx);
    run_worker(&c, true, 2, 12, 0, true);
    run_worker(&c, true, 3, 12, 0, true); sleep_stage(&c, 4, 12, 0, &fx);
    run_worker(&c, true, 5, 12, 0, true);
    sb_p4_set_c6(&c, true, false);
    r = sleep_stage(&c, 6, 12, 5, &fx);
    assert(!r.ready && r.failure == SB_FAIL_HW && !fx.forward_c6);
    puts("old-c6");

    // ---- C6 confirmation: retry, timeout, failure, absent ------------------
    sb_p4_init(&c);
    sb_p4_set_c6(&c, true, true);
    call(&c, rq(BB_ST_OP_SLEEP, 1, 50), 0, 0, &fx);
    run_worker(&c, true, 2, 50, 0, true);
    run_worker(&c, true, 3, 50, 0, true);
    call(&c, rq(BB_ST_OP_SLEEP, 4, 50), 0, 0, &fx);
    run_worker(&c, true, 5, 50, 0, true);
    call(&c, rq(BB_ST_OP_SLEEP, 6, 50), 1000, 0, &fx);
    assert(c.step_running == 6 && c.c6_dark);
    assert(!sb_p4_tick(&c, 1000 + SB_C6_RETRY_MS - 1));
    assert(sb_p4_tick(&c, 1000 + SB_C6_RETRY_MS));                      // re-forward due
    assert(!sb_p4_tick(&c, 1000 + SB_C6_RETRY_MS + 1));
    bb_standby_reply_t wrong = c6_reply(49, BB_ST_ASLEEP, 1, 0, 0);     // other generation: ignored
    sb_p4_c6_reply(&c, &wrong, 1300);
    assert(c.step_running == 6);
    bb_standby_reply_t early = c6_reply(50, BB_ST_ASLEEP, 0, 0, 0);     // not ready yet
    sb_p4_c6_reply(&c, &early, 1300);
    assert(c.step_running == 6);
    sb_p4_tick(&c, 1000 + SB_C6_STEP_TIMEOUT_MS);
    assert(c.step_running == 0 && c.failure == SB_FAIL_TIMEOUT && c.state == BB_ST_ASLEEP);
    r = call(&c, rq(BB_ST_OP_SLEEP, 6, 50), 4000, 0, &fx);              // S3 retry re-forwards
    assert(!r.ready && fx.forward_c6);
    bb_standby_reply_t bad = c6_reply(50, BB_ST_ASLEEP, 1, 0, 0); bad.failure = 1;
    sb_p4_c6_reply(&c, &bad, 4001);
    assert(c.step_running == 0 && c.failure == SB_FAIL_HW);
    // The C6 vanished: on a production board (a display is expected) that is NOT "nothing to
    // turn off" - the screen cannot be proven dark, so stage 6 fails and sleep is inhibited.
    sb_p4_set_c6(&c, false, false);
    assert(c.c6_missing && !c.c6_present && !c.c6_unsupported);
    r = call(&c, rq(BB_ST_OP_SLEEP, 6, 50), 5000, 0, &fx);
    assert(!r.ready && r.failure == SB_FAIL_HW && !fx.forward_c6);
    // Only an explicit no-display board variant may treat it as absent.
    sb_p4_set_c6_expected(&c, false);
    assert(!c.c6_missing);
    r = call(&c, rq(BB_ST_OP_SLEEP, 6, 50), 5001, 0, &fx);
    assert(r.ready && !fx.forward_c6);

    // INDICATORS_ON timeout keeps the instrument usable but the failure visible
    sb_p4_init(&c);
    sb_p4_set_c6(&c, true, true);
    go_asleep(&c, 60, 0);
    wake_stage(&c, 7, 61, 100, &fx);
    bb_standby_reply_t ok7 = c6_reply(61, BB_ST_WAKING, 1, 0, 0);
    sb_p4_c6_reply(&c, &ok7, 101);
    run_worker(&c, false, 8, 61, 102, true);
    run_worker(&c, false, 9, 61, 102, true);
    wake_stage(&c, 10, 61, 102, &fx); sb_p4_step_complete(&c, 10, 61, true, false);
    wake_stage(&c, 10, 61, 103, &fx);
    r = wake_stage(&c, 11, 61, 200, &fx);
    assert(!r.ready && fx.forward_c6);
    sb_p4_tick(&c, 200 + SB_C6_STEP_TIMEOUT_MS);
    assert(c.state == BB_ST_ACTIVE && !c.barrier && c.failure == SB_FAIL_TIMEOUT && c.c6_dark);
    assert(sb_p4_admit(&c)); sb_p4_leave(&c);
    r = wake_stage(&c, 11, 61, 5000, &fx);                              // retry re-forwards, stays usable
    assert(!r.ready && fx.forward_c6 && r.state == BB_ST_ACTIVE && !sb_p4_blocked(&c));
    bb_standby_reply_t ok11 = c6_reply(61, BB_ST_ACTIVE, 1, 0, 0);
    sb_p4_c6_reply(&c, &ok11, 5005);
    assert(wake_stage(&c, 11, 61, 5006, &fx).ready && !c.c6_dark);
    puts("c6-confirm");

    // ---- cancelling a prepare never touches hardware or lights the C6 ------
    sb_p4_init(&c);
    sb_p4_set_c6(&c, true, true);
    call(&c, rq(BB_ST_OP_SLEEP, 1, 70), 0, 0, &fx);
    assert(c.state == BB_ST_PREPARING && !c.hw_paused && !c.analog_cut);
    r = wake_stage(&c, 7, 71, 5, &fx);
    assert(r.ready && !fx.forward_c6 && r.state == BB_ST_WAKING);
    r = wake_stage(&c, 8, 71, 5, &fx); assert(r.ready && fx.run_stage == 0);
    r = wake_stage(&c, 9, 71, 5, &fx); assert(r.ready && fx.run_stage == 0);
    r = wake_stage(&c, 10, 71, 5, &fx);
    assert(r.ready && fx.run_stage == 0);                               // nothing was paused: no resume
    r = wake_stage(&c, 11, 71, 5, &fx);
    assert(r.ready && r.state == BB_ST_ACTIVE && !fx.forward_c6 && !c.wake_pending);
    puts("cancel");

    // ---- activity: C6 counter merges monotonically, reboot is not activity --
    sb_p4_init(&c);
    sb_p4_set_c6(&c, true, true);
    uint32_t base = c.activity;
    bb_standby_reply_t cr = c6_reply(0, BB_ST_ACTIVE, 1, 7, BB_ST_INH_OTA);
    sb_p4_c6_reply(&c, &cr, 0);
    assert(c.activity == base);                                         // first sight only baselines
    cr.activity = 9; sb_p4_c6_reply(&c, &cr, 1);
    assert(c.activity == base + 1);
    cr.activity = 9; sb_p4_c6_reply(&c, &cr, 2);
    assert(c.activity == base + 1);
    cr.activity = 0; sb_p4_c6_reply(&c, &cr, 3);                        // C6 rebooted
    assert(c.activity == base + 1);
    cr.activity = 1; sb_p4_c6_reply(&c, &cr, 4);
    assert(c.activity == base + 2);
    r = call(&c, rq(BB_ST_OP_POLL, 0, 0), 5, 0, &fx);
    assert((r.inhibitors & BB_ST_INH_OTA) && r.activity == base + 2);   // C6 inhibitor surfaces
    sb_p4_set_c6(&c, false, false);
    r = call(&c, rq(BB_ST_OP_POLL, 0, 0), 5, 0, &fx);
    assert(!(r.inhibitors & BB_ST_INH_OTA));
    puts("activity");

    // ---- direct-USB leases --------------------------------------------------
    sb_p4_init(&c);
    uint32_t n = 99;
    assert(lease(&c, 1, 0, 5000, 100) == SB_FAIL_ARG);                  // id 0 is malformed
    assert(lease(&c, 2, 7, 5000, 100) == SB_FAIL_ARG);                  // unknown op is malformed
    assert(sb_p4_lease_count(&c, 100) == 0 && c.activity == 0);         // ... and changes nothing
    assert(lease(&c, 1, 7, 0, 100) == SB_FAIL_NONE);                    // default ttl
    assert(c.leases[0].expires_ms == 100 + SB_LEASE_TTL_DEFAULT_MS);
    lease(&c, 1, 8, 10, 100);                                           // clamped up
    assert(c.leases[1].expires_ms == 100 + SB_LEASE_TTL_MIN_MS);
    lease(&c, 1, 9, 10000000, 100);                                     // clamped down
    assert(c.leases[2].expires_ms == 100 + SB_LEASE_TTL_MAX_MS);
    r = call(&c, rq(BB_ST_OP_SLEEP, 1, 1), 200, 0, &fx);
    assert(r.failure == SB_FAIL_BUSY && (r.inhibitors & BB_ST_INH_HOST));
    assert(sb_p4_lease_count(&c, 100 + SB_LEASE_TTL_MIN_MS - 1) == 3);
    assert(sb_p4_lease_count(&c, 100 + SB_LEASE_TTL_MIN_MS) == 2);      // expiry boundary
    uint32_t act = c.activity;
    lease(&c, 1, 7, 0, 5000);                                           // refresh: not new activity
    assert(c.activity == act);
    assert(lease(&c, 0, 7, 0, 5001) == SB_FAIL_NONE);
    assert(c.activity == act + 1);                                      // release restarts the idle timer
    assert(lease(&c, 0, 12345, 0, 5002) == SB_FAIL_NONE);               // releasing an unknown id is harmless

    // a full table refuses a 5th client and evicts nobody
    sb_p4_init(&c);
    for (uint32_t i = 1; i <= SB_LEASE_MAX; ++i) assert(lease(&c, 1, i, 20000 + i * 1000, 0) == SB_FAIL_NONE);
    uint32_t act2 = c.activity;
    assert(sb_p4_lease(&c, 1, 99, 5000, 0, &n) == SB_FAIL_FULL && n == SB_LEASE_MAX);
    assert(c.activity == act2);                                         // refused: no state change
    for (uint32_t i = 1; i <= SB_LEASE_MAX; ++i)
        assert(c.leases[i - 1].id == i && c.leases[i - 1].expires_ms == 20000 + i * 1000);
    assert(lease(&c, 1, 3, 40000, 10) == SB_FAIL_NONE);                 // a refresh of a held id still works
    assert(c.leases[2].expires_ms == 10 + 40000);
    assert(lease(&c, 0, 2, 0, 11) == SB_FAIL_NONE);                     // release frees a slot ...
    assert(lease(&c, 1, 99, 5000, 12) == SB_FAIL_NONE);                 // ... which the 5th client takes
    // an expired lease frees its slot without being asked
    sb_p4_init(&c);
    for (uint32_t i = 1; i <= SB_LEASE_MAX; ++i) lease(&c, 1, i, 2000, 0);
    assert(lease(&c, 1, 50, 5000, 1999) == SB_FAIL_FULL);
    assert(lease(&c, 1, 50, 5000, 2000) == SB_FAIL_NONE);
    sb_p4_init(&c);                                                     // wraparound of the millisecond clock
    lease(&c, 1, 5, 5000, 0xFFFFF000u);
    assert(sb_p4_lease_count(&c, 0x00000100u) == 1);
    assert(sb_p4_lease_count(&c, 0xFFFFF000u + 5000u) == 0);

    // the 16 B acknowledgement
    sb_p4_init(&c);
    bb_standby_reply_t ack;
    sb_p4_lease_ack(&c, SB_FAIL_NONE, 0, &ack);
    assert(ack.schema == BB_STANDBY_SCHEMA && ack.ready == 1 && ack.failure == 0 && ack.state == BB_ST_ACTIVE);
    sb_p4_lease_ack(&c, SB_FAIL_ARG, 0, &ack);
    assert(!ack.ready && ack.failure == SB_FAIL_ARG);
    sb_p4_lease_ack(&c, SB_FAIL_FULL, 0, &ack);
    assert(!ack.ready && ack.failure == SB_FAIL_FULL);
    lease(&c, 1, 4, 5000, 0);
    sb_p4_lease_ack(&c, SB_FAIL_NONE, 1, &ack);
    assert(ack.inhibitors == 1);                                        // live lease count
    lease(&c, 0, 4, 0, 2);
    go_asleep(&c, 33, 100);                                             // a client arrives while asleep:
    lease(&c, 1, 4, 5000, 102);                                         //   recorded, a wake is requested ...
    assert(c.wake_pending);
    sb_p4_lease_ack(&c, SB_FAIL_NONE, 103, &ack);                       //   ... but it is not usable yet
    assert(!ack.ready && ack.failure == SB_FAIL_BUSY && ack.state == BB_ST_ASLEEP && ack.generation == 33);
    puts("leases");

    // ---- policy mirror: only the four legal values are accepted ------------
    sb_p4_init(&c);
    bb_standby_request_t p = rq(BB_ST_OP_POLL, 0, 0); p.timeout_seconds = 123;
    call(&c, p, 0, 0, &fx); assert(!c.timeout_known);
    p.timeout_seconds = 900; call(&c, p, 0, 0, &fx);
    assert(c.timeout_known && c.timeout_s == 900);
    p.timeout_seconds = 0; call(&c, p, 0, 0, &fx);
    assert(c.timeout_known && c.timeout_s == 0);                        // Off is a real value
    sb_p4_build_c6_request(&c, 0, NULL, &mirror);
    assert(mirror.timeout_seconds == 0);
    sb_p4_init(&c);
    sb_p4_build_c6_request(&c, 0, NULL, &mirror);
    assert(mirror.timeout_seconds == 0xFFFFu);                          // unknown stays unknown
    puts("policy-mirror");

    // ---- the loading screen sees real wake results only --------------------
    sb_p4_init(&c);
    go_asleep(&c, 120, 0);
    wake_stage(&c, 7, 121, 10, &fx);
    bb_standby_reply_t cr7 = c6_reply(121, BB_ST_WAKING, 1, 0, 0);
    sb_p4_c6_reply(&c, &cr7, 11);
    run_worker(&c, false, 8, 121, 11, false);                           // power-on fails
    sb_p4_build_c6_request(&c, 8, NULL, &mirror);
    assert(mirror.op == BB_ST_OP_WAKE && mirror.stage == 8);
    assert((mirror.failed & (1u << 8)) && !(mirror.completed & (1u << 8)));
    run_worker(&c, false, 8, 121, 12, true);                            // the retry works
    sb_p4_build_c6_request(&c, 8, NULL, &mirror);
    assert((mirror.completed & (1u << 8)) && !(mirror.failed & (1u << 8)));
    puts("wake-progress");

    // ---- physical buttons ---------------------------------------------------
    // gate(g, blocked, held, raw, events, now, out): held = debounced, raw = pad level
    sb_btn_gate_t g; sb_btn_out_t o;
    memset(&g, 0, sizeof(g));
    sb_btn_gate(&g, false, true, true, 0, 1000, &o);                    // press while awake
    assert(o.events == 0 && o.activity && !o.wake_press && !o.start_discard);
    sb_btn_gate(&g, false, false, false, 0x04, 1050, &o);               // short press releases an OK
    assert(o.events == 0x04 && o.activity);
    sb_btn_gate(&g, false, false, false, 0, 1060, &o);                  // nothing is consumed on a normal gesture
    assert(o.events == 0 && !g.consuming);

    // A gesture that starts asleep is consumed to release PLUS the release debounce.
    memset(&g, 0, sizeof(g));
    sb_btn_gate(&g, true, true, true, 0, 2000, &o);                     // press while asleep
    assert(o.events == 0 && o.wake_press && g.consuming && o.start_discard);
    sb_btn_gate(&g, true, true, true, 0x08, 2550, &o);                  // long-press BACK fires while still asleep
    assert(o.events == 0 && o.wake_press && !o.start_discard);
    sb_btn_gate(&g, false, true, true, 0x08, 2600, &o);                 // system woke mid-gesture: a queued BACK
    assert(o.events == 0 && !o.wake_press && g.consuming);
    sb_btn_gate(&g, false, true, true, 0x01, 2700, &o);                 // auto-repeat still swallowed
    assert(o.events == 0);
    sb_btn_gate(&g, false, false, false, 0x04, 2800, &o);               // release-time OK (would flip VDUT) swallowed
    assert(o.events == 0 && g.consuming);                               // ... and the window is still open
    sb_btn_gate(&g, false, false, false, 0, 2800 + SB_BTN_RELEASE_HOLDOFF_MS - 1, &o);
    assert(o.events == 0 && g.consuming);                               // release debounce not over
    sb_btn_gate(&g, false, false, true, 0, 2800 + SB_BTN_RELEASE_HOLDOFF_MS + 5, &o);
    assert(o.events == 0 && g.consuming);                               // a bounce on the pad restarts it
    sb_btn_gate(&g, false, false, false, 0, 2800 + 2 * SB_BTN_RELEASE_HOLDOFF_MS + 10, &o);
    assert(o.events == 0 && !g.consuming);                              // finally released
    sb_btn_gate(&g, false, true, true, 0, 3000, &o);                    // next gesture is normal again
    assert(o.activity);
    sb_btn_gate(&g, false, false, false, 0x04, 3050, &o);
    assert(o.events == 0x04);

    // A press the debouncer has not confirmed yet is still a gesture asleep.
    memset(&g, 0, sizeof(g));
    sb_btn_gate(&g, true, false, true, 0, 4000, &o);                    // pad low, not yet debounced
    assert(o.events == 0 && !o.wake_press && g.consuming && o.start_discard);
    sb_btn_gate(&g, false, true, true, 0, 4010, &o);                    // woke; the press is confirmed just now
    assert(o.events == 0 && g.consuming);
    sb_btn_gate(&g, false, true, true, 0x01, 4400, &o);                 // repeat
    assert(o.events == 0);

    memset(&g, 0, sizeof(g));                                           // short press fully inside sleep
    sb_btn_gate(&g, true, true, true, 0, 5000, &o);
    sb_btn_gate(&g, true, false, false, 0x04, 5100, &o);                // release event arrives while blocked
    assert(o.events == 0);
    sb_btn_gate(&g, false, false, false, 0, 5100 + SB_BTN_RELEASE_HOLDOFF_MS, &o);   // nothing left over after wake
    assert(o.events == 0 && !g.consuming);
    puts("buttons");

    // ---- routes held, acquisition restart deferred: a wake never does either ----
    sb_p4_init(&c);
    sb_p4_set_c6(&c, true, true);
    go_asleep(&c, 200, 0);
    assert(c.routes_held && c.fast_was_running && !c.acq_resume_pending);
    go_awake(&c, 201, 100);
    assert(c.state == BB_ST_ACTIVE && !sb_p4_blocked(&c));
    assert(c.routes_held);                                              // ACTIVE, yet the analog path is still disconnected
    assert(c.acq_resume_pending && !c.fast_was_running);                // acquisition waits for an explicit request
    assert(sb_p4_take_acq_resume(&c) && !sb_p4_take_acq_resume(&c));    // consumed exactly once
    sb_p4_routes_connected(&c);
    assert(!c.routes_held);
    // a sleep that stopped nothing leaves nothing to restart
    sb_p4_init(&c);
    sb_p4_set_c6(&c, true, true);
    call(&c, rq(BB_ST_OP_SLEEP, 1, 210), 0, 0, &fx);
    r = call(&c, rq(BB_ST_OP_SLEEP, 2, 210), 1, 0, &fx);
    sb_p4_step_complete(&c, 2, 210, true, false);                       // acquisition was NOT running
    run_worker(&c, true, 3, 210, 2, true); sleep_stage(&c, 4, 210, 3, &fx);
    run_worker(&c, true, 5, 210, 4, true);
    sleep_stage(&c, 6, 210, 5, &fx);
    { bb_standby_reply_t cr6 = c6_reply(210, BB_ST_ASLEEP, 1, 0, 0); sb_p4_c6_reply(&c, &cr6, 6); }
    assert(sleep_stage(&c, 6, 210, 7, &fx).ready);
    go_awake(&c, 211, 100);
    assert(!c.acq_resume_pending && c.routes_held);
    puts("routes-held");

    // ---- stage 3 gates stage 5, in both directions ------------------------------
    sb_p4_init(&c);
    sb_p4_set_c6(&c, true, true);
    call(&c, rq(BB_ST_OP_SLEEP, 1, 220), 0, 0, &fx);
    run_worker(&c, true, 2, 220, 0, true);
    r = sleep_stage(&c, 5, 220, 1, &fx);                                // stage 3 never ran
    assert(!r.ready && r.failure == SB_FAIL_ORDER && fx.run_stage == 0 && !c.analog_cut);
    r = sleep_stage(&c, 4, 220, 1, &fx);                                // stage 4 needs it as well
    assert(!r.ready && r.failure == SB_FAIL_ORDER && fx.run_stage == 0);
    r = sleep_stage(&c, 3, 220, 2, &fx);                                // stage 3 is a hardware stage
    assert(!r.ready && fx.run_stage == 3 && fx.run_generation == 220 && c.routes_held);
    r = sleep_stage(&c, 5, 220, 3, &fx);                                // ... and 5 may not overtake it
    assert(!r.ready && r.failure == SB_FAIL_ORDER && fx.run_stage == 0 && !c.analog_cut);
    sb_p4_step_complete(&c, 3, 220, false, false);                      // the readback disagreed
    r = sleep_stage(&c, 3, 220, 4, &fx);
    assert(!r.ready && r.state == BB_ST_FAULT_SAFE && r.failure == SB_FAIL_HW && c.steps_failed == (1u << 3));
    r = sleep_stage(&c, 5, 220, 5, &fx);                                // never reaches the rails
    assert(!r.ready && fx.run_stage == 0 && !c.analog_cut);
    // a stage-3 completion of an older generation changes nothing
    sb_p4_init(&c);
    call(&c, rq(BB_ST_OP_SLEEP, 1, 230), 0, 0, &fx);
    run_worker(&c, true, 2, 230, 0, true);
    sleep_stage(&c, 3, 230, 1, &fx);
    assert(!sb_p4_step_complete(&c, 3, 229, true, false) && c.step_running == 3);
    assert(sb_p4_step_complete(&c, 3, 230, true, false) && !(c.steps_done & (1u << 5)));
    assert(sleep_stage(&c, 3, 230, 2, &fx).ready && (c.steps_done & (1u << 3)));
    puts("mux-stage");
    return 0;
}
"""


def test_p4_standby_participant_executes():
    out = compile_and_run(
        MAIN,
        sources=[f"{P4_STANDBY}/standby_p4_core.c"],
        include_dirs=[P4_STANDBY, COMMON],
    ).split()
    text = " ".join(out)
    for marker in ("full-cycle/idempotent/echo", "stale/wrap/arg", "boot-rebase/mirror",
                   "boot-p4-truth", "busy/race", "admission", "fault-safe", "analog-recovery",
                   "stale-worker", "old-c6", "c6-confirm", "cancel", "activity", "leases",
                   "policy-mirror", "wake-progress", "buttons", "routes-held", "mux-stage"):
        assert marker in text, marker
