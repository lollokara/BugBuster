"""S3 standby coordinator: barrier, atomic re-check, step ordering, HAT exchange."""

from tests.firmware_host.fwhost import compile_and_run

POWER = "Firmware/ESP32/src/power"
BBP = "Firmware/ESP32/src/bbp"
P4_STANDBY = "Firmware/DAQ_HAT/ESP32P4/src/standby"

SOURCES = [
    f"{POWER}/standby_policy.c",
    f"{POWER}/standby_system.c",
    f"{P4_STANDBY}/standby_p4_core.c",
]

MAIN = r"""
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "standby_system.h"
#include "standby_p4_core.h"

typedef struct {
    sb_p4_t p4;
    uint32_t p4_local_inh;
    int worker_stage, worker_countdown;
    int c6_stage, c6_countdown;
    int exchanges;
    int stage_log[256]; int nstage;       /* every SLEEP/WAKE stage ever sent */
    int busy_on_stage;                    /* reply BUSY once for this stage */
    int force_failure_stage; uint8_t force_failure;
    int unsupported;                      /* answer like an old firmware */
    int silent;                           /* never answer */
    int wrong_generation;
    int polls;
    int worker_fail_stage;
} HatModel;

typedef struct {
    uint32_t now;
    int locked;
    uint32_t inh;
    int inh_calls, flip_in_lock; uint32_t inh_flip_val;
    StandbyHatLink link;
    HatModel hat;
    int local_log[256]; int nlocal;
    int local_fail_step;
    int local_pending_left;
    int persisted;
    int trace;
    int progress_frames; uint16_t last_completed, last_failed, last_skipped;
    int analog_open_during_local;         /* analog_allowed() sampled inside a local step */
    int persist_fail, persist_calls;      /* storage fake */
    int wlock_held, wlock_deny;           /* policy writer barrier fake */
    int hook_mode, hook_rc, hook_get_ok;  /* a second writer arriving inside persist */
    int inh_usb_host;                     /* mirrors the glue: a legacy USB session counts as HOST */
    int safe_calls, safe_ok;              /* FAULT_SAFE output cleanup fake */
    StandbySystem *sys;
} Env;

static void e_lock(void *c) { Env *e = c; assert(!e->locked); e->locked = 1; }
static void e_unlock(void *c) { Env *e = c; assert(e->locked); e->locked = 0; }
static uint32_t e_now(void *c) { return ((Env *)c)->now; }
static uint32_t e_inh(void *c) {
    Env *e = c;
    e->inh_calls++;
    if (e->flip_in_lock && e->locked) return e->inh_flip_val;   /* only the atomic re-check */
    return e->inh | ((e->inh_usb_host && standby_system_usb_legacy_host(e->sys)) ? BB_ST_INH_HOST : 0u);
}
static StandbyHatLink e_link(void *c) { return ((Env *)c)->link; }

static bb_standby_reply_t c6_reply(uint32_t gen, uint8_t state) {
    bb_standby_reply_t r;
    memset(&r, 0, sizeof(r));
    r.schema = BB_STANDBY_SCHEMA; r.state = state; r.ready = 1; r.generation = gen;
    return r;
}

static StandbyXchg e_xchg(void *c, const bb_standby_request_t *rq, bb_standby_reply_t *rp) {
    Env *e = c;
    HatModel *m = &e->hat;
    assert(!e->locked);                       /* never across a UART exchange */
    m->exchanges++;
    if (m->silent) return STANDBY_XCHG_TIMEOUT;
    if (m->unsupported) return STANDBY_XCHG_UNSUPPORTED;
    if (rq->op == BB_ST_OP_PROGRESS && rq->stage == BB_ST_BOOT_STAGE) {
        e->progress_frames++;
        e->last_completed = rq->completed; e->last_failed = rq->failed; e->last_skipped = rq->skipped;
    }
    if (rq->op == BB_ST_OP_POLL) m->polls++;
    if (rq->op == BB_ST_OP_SLEEP || rq->op == BB_ST_OP_WAKE) m->stage_log[m->nstage++] = rq->stage;
    sb_p4_fx_t fx;
    int busy_now = m->busy_on_stage && rq->stage == m->busy_on_stage && rq->op != BB_ST_OP_POLL;
    uint32_t inh_now = busy_now ? (m->p4_local_inh | BB_ST_INH_STREAM) : m->p4_local_inh;
    sb_p4_handle(&m->p4, rq, e->now, sb_p4_entry_seq(&m->p4), inh_now, rp, &fx);
    if (busy_now) m->busy_on_stage = 0;           /* the real P4 core refuses with SB_FAIL_BUSY */
    if (m->force_failure_stage && rq->stage == m->force_failure_stage && rq->op != BB_ST_OP_POLL) {
        rp->ready = 0; rp->failure = m->force_failure;
    }
    if (fx.run_stage) { m->worker_stage = fx.run_stage; m->worker_countdown = 2; }
    if (fx.forward_c6) { m->c6_stage = fx.forward_stage; m->c6_countdown = 2; }
    if (m->worker_stage && --m->worker_countdown <= 0) {
        sb_p4_step_done(&m->p4, (uint8_t)m->worker_stage, m->worker_fail_stage != m->worker_stage, false);
        m->worker_stage = 0;
    }
    if (m->c6_stage && --m->c6_countdown <= 0) {
        uint8_t st = m->c6_stage == 6 ? BB_ST_ASLEEP : m->c6_stage == 7 ? BB_ST_WAKING : BB_ST_ACTIVE;
        bb_standby_reply_t cr = c6_reply(m->p4.generation, st);
        sb_p4_c6_reply(&m->p4, &cr, e->now);
        m->c6_stage = 0;
    }
    sb_p4_tick(&m->p4, e->now);
    if (m->wrong_generation) rp->generation += 5;
    if (e->trace && rq->op != BB_ST_OP_POLL)
        fprintf(stderr, "  xchg op=%d stage=%d gen=%u -> ready=%d fail=%d gen=%u p4state=%d\n", rq->op, rq->stage,
                rq->generation, rp->ready, rp->failure, rp->generation, m->p4.state);
    return STANDBY_XCHG_OK;
}

static StandbyStepResult e_local(void *c, StandbyStep step, uint32_t gen) {
    Env *e = c;
    assert(!e->locked);                       /* never across a hardware step */
    if (e->local_pending_left > 0) { e->local_pending_left--; return STANDBY_STEP_PENDING; }
    e->local_log[e->nlocal++] = (int)step;
    e->analog_open_during_local = standby_system_analog_allowed(e->sys);
    return e->local_fail_step == (int)step ? STANDBY_STEP_FAILED : STANDBY_STEP_DONE;
}
static bool e_persist(void *c, uint32_t s) {
    Env *e = c;
    e->persist_calls++;
    assert(e->wlock_held);                    /* inside the writer barrier */
    assert(!e->locked);                       /* never inside the state lock */
    if (e->hook_mode == 1) {
        e->hook_mode = 0;                     /* the racing writer */
        e->hook_rc = standby_system_set_timeout(e->sys, 60);
        StandbyStatus st; standby_system_get(e->sys, &st);   /* every other request stays live */
        e->hook_get_ok = st.state == BB_ST_ACTIVE;
    }
    if (e->persist_fail) return false;
    e->persisted = (int)s;
    return true;
}
static bool e_wlock(void *c) {
    Env *e = c;
    if (e->wlock_deny || e->wlock_held) return false;
    e->wlock_held = 1;
    return true;
}
static void e_wunlock(void *c) { Env *e = c; assert(e->wlock_held); e->wlock_held = 0; }
static bool e_safe(void *c) { Env *e = c; assert(!e->locked); e->safe_calls++; return e->safe_ok != 0; }

static void make_ops(Env *e, StandbyOps *ops) {
    StandbyOps o = { e, e_lock, e_unlock, e_now, e_inh, e_link, e_xchg, e_local, e_persist,
                     e_wlock, e_wunlock, e_safe };
    *ops = o;
}

static void env_init(Env *e, StandbySystem *s, uint32_t timeout, StandbyHatLink link) {
    memset(e, 0, sizeof(*e));
    e->link = link;
    e->sys = s;
    e->now = 1000;
    sb_p4_init(&e->hat.p4);
    sb_p4_set_c6_present(&e->hat.p4, true);
    e->safe_ok = 1;
    StandbyOps ops;
    make_ops(e, &ops);
    standby_system_init(s, &ops, timeout);
}

/* The S3 restarts: a fresh coordinator, the HAT (and the clock) carry on. */
static void s3_reboot(Env *e, StandbySystem *s, uint32_t timeout) {
    StandbyOps ops;
    make_ops(e, &ops);
    standby_system_init(s, &ops, timeout);
    e->nlocal = 0; e->hat.nstage = 0;
}

static void drive(Env *e, StandbySystem *s, uint32_t ms) {
    for (uint32_t t = 0; t < ms; t += 10) { e->now += 10; standby_system_tick(s); }
}

static int drive_until(Env *e, StandbySystem *s, StandbyState want, uint32_t max_ms) {
    for (uint32_t t = 0; t < max_ms; t += 10) {
        if (s->policy.state == want) return 1;
        e->now += 10;
        standby_system_tick(s);
    }
    return s->policy.state == want;
}

static void to_asleep(Env *e, StandbySystem *s) {
    assert(drive_until(e, s, STANDBY_ASLEEP, 120000));
}
static void to_active(Env *e, StandbySystem *s) {
    if (!drive_until(e, s, STANDBY_ACTIVE, 30000))
        fprintf(stderr, "stuck: state=%d step=%d pending=%d p4state=%d p4fail=%d p4run=%d barrier=%d hwpaused=%d\n",
                s->policy.state, s->policy.step, s->policy.pending, e->hat.p4.state, e->hat.p4.failure,
                e->hat.p4.step_running, e->hat.p4.barrier, e->hat.p4.hw_paused);
    assert(drive_until(e, s, STANDBY_ACTIVE, 1));
}

int main(void) {
    static Env env; static StandbySystem sys;
    setvbuf(stdout, NULL, _IONBF, 0);

    /* ---- full cycle with the real P4 participant ------------------------------ */
    env_init(&env, &sys, 60, STANDBY_HAT_LINKED);
    drive(&env, &sys, 1000);                       /* a few polls: HAT proven, nothing to do */
    assert(env.hat.polls >= 3 && sys.hat_supported && sys.policy.state == STANDBY_ACTIVE);
    to_asleep(&env, &sys);
    assert(!standby_system_analog_allowed(&sys));
    {
        int expect[] = {1, 2, 3, 4, 5, 6};         /* the glue makes HAT_SLEEP a local no-op */
        assert(env.nlocal == 6);
        for (int i = 0; i < 6; ++i) assert(env.local_log[i] == expect[i]);
        int expect_hat[] = {1, 2, 3, 4, 5, 6};
        int n = 0;
        for (int i = 0; i < env.hat.nstage && n < 6; ++i)
            if (env.hat.stage_log[i] == expect_hat[n]) n++;
        assert(n == 6);                            /* every stage 1..6 reached the HAT in order */
    }
    assert(env.hat.p4.state == BB_ST_ASLEEP);
    assert(env.analog_open_during_local == 0);
    {
        StandbyStatus st; standby_system_get(&sys, &st);
        assert(st.state == BB_ST_ASLEEP && !st.ready && st.completed == ((1u<<1)|(1u<<2)|(1u<<3)|(1u<<4)|(1u<<5)|(1u<<6)));
    }
    puts("full-sleep-with-p4");

    /* idle while asleep: still asleep, polling continues */
    int polls = env.hat.polls;
    drive(&env, &sys, 2000);
    assert(sys.policy.state == STANDBY_ASLEEP && env.hat.polls > polls);

    /* a new request while asleep: refused, wake requested, never executed */
    assert(standby_system_work_begin(&sys) == STANDBY_ADMIT_WAKING);
    assert(sys.work_depth == 0 && sys.policy.state == STANDBY_WAKING);
    to_active(&env, &sys);
    assert(standby_system_analog_allowed(&sys) && env.hat.p4.state == BB_ST_ACTIVE);
    {
        int expect[] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11};
        assert(env.nlocal == 11);
        for (int i = 0; i < 11; ++i) assert(env.local_log[i] == expect[i]);
        assert(env.analog_open_during_local == 0);      /* gate stays shut until ACTIVE */
    }
    puts("wake-on-request-no-replay");

    /* admitted work blocks the transition and a fresh idle interval follows */
    to_active(&env, &sys);
    assert(standby_system_work_begin(&sys) == STANDBY_ADMIT_OK);
    drive(&env, &sys, 90000);
    assert(sys.policy.state == STANDBY_ACTIVE);
    standby_system_work_end(&sys);
    drive(&env, &sys, 59000);
    assert(sys.policy.state == STANDBY_ACTIVE);       /* old timer would have fired long ago */
    drive(&env, &sys, 2000);
    assert(sys.policy.state != STANDBY_ACTIVE);
    puts("work-blocks-then-fresh-timer");

    /* ---- atomic re-check cancels without touching anything ------------------- */
    env_init(&env, &sys, 60, STANDBY_HAT_LINKED);
    drive(&env, &sys, 59000);
    env.hat.nstage = 0; env.nlocal = 0;
    env.inh_calls = 0; env.flip_in_lock = 1; env.inh_flip_val = BB_ST_INH_STREAM;   /* appears after the tick sample */
    drive(&env, &sys, 3000);
    env.flip_in_lock = 0;
    assert(sys.cancelled_prepares >= 1);
    assert(env.nlocal == 0 && env.hat.nstage == 0);
    assert(sys.policy.state == STANDBY_ACTIVE);
    puts("atomic-recheck-cancel");

    /* ---- every inhibitor bit keeps the system awake -------------------------- */
    for (int bit = 0; bit < 15; ++bit) {
        env_init(&env, &sys, 60, STANDBY_HAT_LINKED);
        env.inh = 1u << bit;
        drive(&env, &sys, 200000);
        assert(sys.policy.state == STANDBY_ACTIVE && env.nlocal == 0);
        StandbyStatus st; standby_system_get(&sys, &st);
        assert(st.inhibitors & (1u << bit));
    }
    puts("each-inhibitor");

    /* ---- HAT contributed inhibitors ------------------------------------------ */
    env_init(&env, &sys, 60, STANDBY_HAT_LINKED);
    env.hat.p4_local_inh = BB_ST_INH_STREAM;
    drive(&env, &sys, 100000);
    assert(sys.policy.state == STANDBY_ACTIVE && (sys.hat_inhibitors & BB_ST_INH_STREAM));
    env.hat.p4_local_inh = 0;
    drive(&env, &sys, 1000);
    assert(sys.hat_inhibitors == 0);
    puts("hat-inhibitors");

    /* ---- fail closed: link unusable / old firmware ----------------------------- */
    env_init(&env, &sys, 60, STANDBY_HAT_DETECTED);
    drive(&env, &sys, 200000);
    assert(sys.policy.state == STANDBY_ACTIVE && env.hat.exchanges == 0);
    { StandbyStatus st; standby_system_get(&sys, &st); assert(st.inhibitors & BB_ST_INH_HAT_UNKNOWN); }
    env_init(&env, &sys, 60, STANDBY_HAT_LINKED);
    env.hat.unsupported = 1;
    drive(&env, &sys, 200000);
    assert(sys.policy.state == STANDBY_ACTIVE && !sys.hat_supported);
    { StandbyStatus st; standby_system_get(&sys, &st); assert(st.inhibitors & BB_ST_INH_HAT_UNKNOWN); }
    env_init(&env, &sys, 60, STANDBY_HAT_LINKED);
    env.hat.silent = 1;
    drive(&env, &sys, 200000);
    assert(sys.policy.state == STANDBY_ACTIVE && !sys.hat_supported);
    puts("fail-closed-hat");

    /* ---- no HAT at all: skipped honestly ---------------------------------------- */
    env_init(&env, &sys, 60, STANDBY_HAT_NONE);
    to_asleep(&env, &sys);
    assert(env.hat.exchanges == 0 && env.nlocal == 6);
    { StandbyStatus st; standby_system_get(&sys, &st);
      assert(st.skipped == ((1u<<1)|(1u<<2)|(1u<<3)|(1u<<4)|(1u<<5)|(1u<<6))); }
    standby_system_wake(&sys);
    to_active(&env, &sys);
    { StandbyStatus st; standby_system_get(&sys, &st); assert(st.ready && st.skipped == ((1u<<7)|(1u<<8)|(1u<<9)|(1u<<10)|(1u<<11))); }
    puts("missing-hat-skipped");

    /* ---- HAT refuses QUIESCE: cancel, nothing local touched ---------------------- */
    env_init(&env, &sys, 60, STANDBY_HAT_LINKED);
    drive(&env, &sys, 59000);
    env.hat.busy_on_stage = 1;
    drive(&env, &sys, 3000);
    assert(env.nlocal == 0 && sys.policy.state == STANDBY_ACTIVE);
    puts("hat-busy-at-quiesce");

    /* ---- HAT refuses HAT_SLEEP: recover through the wake path -------------------- */
    env_init(&env, &sys, 60, STANDBY_HAT_LINKED);
    drive(&env, &sys, 59000);
    env.hat.busy_on_stage = 2;
    to_active(&env, &sys);                           /* trivially true until it starts */
    drive(&env, &sys, 5000);
    assert(drive_until(&env, &sys, STANDBY_ACTIVE, 30000));
    assert(env.hat.p4.state == BB_ST_ACTIVE || env.hat.p4.state == BB_ST_PREPARING);
    drive(&env, &sys, 5000);
    assert(sys.policy.state == STANDBY_ACTIVE && standby_system_analog_allowed(&sys));
    puts("hat-busy-at-hat-sleep-recovers");

    /* ---- failure at each stage ends in FAULT_SAFE, then recovers ------------------ */
    for (int stage = 2; stage <= 6; ++stage) {
        env_init(&env, &sys, 60, STANDBY_HAT_LINKED);
        fprintf(stderr, "failure loop stage %d\n", stage);
        env.trace = 1;
        drive(&env, &sys, 59000);
        if (stage == 2) env.hat.worker_fail_stage = 2;
        else { env.hat.force_failure_stage = stage; env.hat.force_failure = SB_FAIL_HW; }
        assert(drive_until(&env, &sys, STANDBY_FAULT_SAFE, 30000));
        StandbyStatus st; standby_system_get(&sys, &st);
        assert(!st.ready && st.state == BB_ST_FAULT_SAFE && st.failed_stage == (uint8_t)stage);
        assert(st.failed & (1u << stage));
        assert(!standby_system_analog_allowed(&sys));
        env.hat.force_failure_stage = 0; env.hat.worker_fail_stage = 0;
        assert(standby_system_work_begin(&sys) == STANDBY_ADMIT_FAULT);
        to_active(&env, &sys);
    }
    puts("failure-then-recover");

    /* ---- local step failure -------------------------------------------------------- */
    env_init(&env, &sys, 60, STANDBY_HAT_NONE);
    env.local_fail_step = STANDBY_MUX_OFF;
    drive(&env, &sys, 60000);
    assert(drive_until(&env, &sys, STANDBY_FAULT_SAFE, 5000));
    puts("local-failure");

    /* ---- stale/foreign generation in a reply ---------------------------------------- */
    env_init(&env, &sys, 60, STANDBY_HAT_LINKED);
    drive(&env, &sys, 59000);
    env.hat.wrong_generation = 1;
    assert(drive_until(&env, &sys, STANDBY_FAULT_SAFE, 30000));
    puts("generation-mismatch");

    /* ---- pending local step spans ticks, deadline still bounded ---------------------- */
    env_init(&env, &sys, 60, STANDBY_HAT_NONE);
    env.local_pending_left = 1000000;
    drive(&env, &sys, 60000);
    assert(drive_until(&env, &sys, STANDBY_FAULT_SAFE, STANDBY_STEP_TIMEOUT_MS + 2000));
    puts("pending-deadline");

    /* ---- explicit sleep -------------------------------------------------------------------- */
    env_init(&env, &sys, 900, STANDBY_HAT_LINKED);
    drive(&env, &sys, 1000);
    assert(standby_system_presence(&sys, 7, true, STANDBY_SRC_OTHER) == STANDBY_RC_OK);
    env.inh = BB_ST_INH_HOST;                          /* BBP USB session of the requester */
    drive(&env, &sys, 1000);
    assert(standby_system_sleep(&sys) == STANDBY_RC_BUSY);
    assert(standby_system_presence(&sys, 7, false, STANDBY_SRC_OTHER) == STANDBY_RC_OK);
    assert(standby_system_sleep(&sys) == STANDBY_RC_BUSY);
    env.inh = 0;
    assert(standby_system_sleep(&sys) == STANDBY_RC_OK);
    to_asleep(&env, &sys);
    assert(sys.policy.forced || sys.policy.state == STANDBY_ASLEEP);
    standby_system_wake(&sys); to_active(&env, &sys);
    env.inh = BB_ST_INH_SCRIPT;
    assert(standby_system_sleep(&sys) == STANDBY_RC_BUSY);
    env.inh = 0;
    assert(standby_system_work_begin(&sys) == STANDBY_ADMIT_OK);
    assert(standby_system_sleep(&sys) == STANDBY_RC_BUSY);
    standby_system_work_end(&sys);
    standby_system_wake(&sys);
    puts("explicit-sleep");

    /* ---- wake sources ------------------------------------------------------------------------ */
    env_init(&env, &sys, 60, STANDBY_HAT_LINKED);
    to_asleep(&env, &sys);
    env.hat.p4.activity++;                                  /* physical button on the HAT */
    assert(drive_until(&env, &sys, STANDBY_ACTIVE, 30000));
    to_asleep(&env, &sys);
    assert(standby_system_presence(&sys, 99, true, STANDBY_SRC_OTHER) == STANDBY_RC_OK);
    assert(sys.policy.state == STANDBY_WAKING);
    to_active(&env, &sys);
    env.hat.silent = 0;
    to_asleep(&env, &sys);
    env.hat.silent = 1;                                     /* wake path lost while asleep */
    assert(drive_until(&env, &sys, STANDBY_WAKING, 5000) ||
           drive_until(&env, &sys, STANDBY_FAULT_SAFE, 20000));
    puts("wake-sources");

    /* ---- configuration persists, bad values refused ------------------------------------------- */
    env_init(&env, &sys, 60, STANDBY_HAT_NONE);
    assert(standby_system_set_timeout(&sys, 120) == STANDBY_RC_BAD_ARG && env.persisted == 0);
    assert(standby_system_set_timeout(&sys, 900) == STANDBY_RC_OK && env.persisted == 900);
    { StandbyStatus st; standby_system_get(&sys, &st); assert(st.timeout_seconds == 900); }
    puts("policy-persist");

    /* ---- boot progress -------------------------------------------------------------------------- */
    env_init(&env, &sys, 300, STANDBY_HAT_DETECTED);
    standby_system_boot_progress(&sys, BB_ST_BOOT_IO, 0, 0);
    assert(env.progress_frames == 0 && sys.boot_dirty);       /* nothing is faked while unlinked */
    env.link = STANDBY_HAT_LINKED;
    standby_system_boot_progress(&sys, BB_ST_BOOT_WIFI, 0, BB_ST_BOOT_C6_WIFI);
    assert(env.progress_frames == 1 && env.last_completed == (BB_ST_BOOT_IO | BB_ST_BOOT_WIFI));
    assert(env.last_skipped == BB_ST_BOOT_C6_WIFI && !sys.boot_dirty);
    assert(env.hat.p4.boot_seen && !(env.hat.p4.boot_completed & BB_ST_BOOT_P4));
    puts("boot-progress");

    /* ---- gate counters ---------------------------------------------------------------------------- */
    env_init(&env, &sys, 60, STANDBY_HAT_NONE);
    assert(standby_system_analog_enter(&sys) && standby_system_analog_depth(&sys) == 1);
    standby_system_analog_leave(&sys);
    to_asleep(&env, &sys);
    assert(!standby_system_analog_enter(&sys) && standby_system_analog_depth(&sys) == 0);
    puts("analog-gate");

    /* ---- USB protocol epoch: legacy handshake vs explicit presence ---------------------- */
    {
        env_init(&env, &sys, 900, STANDBY_HAT_NONE);
        env.inh_usb_host = 1;                                /* the glue samples the epoch */
        drive(&env, &sys, 1000);
        assert(!standby_system_usb_legacy_host(&sys) && sys.usb_mode == STANDBY_USB_NONE);
        assert(standby_system_sleep(&sys) == STANDBY_RC_OK);
        drive_until(&env, &sys, STANDBY_ASLEEP, 30000);      /* no host, no client: sleeps */
        standby_system_wake(&sys); to_active(&env, &sys);

        /* legacy: the handshake alone is a logical client */
        standby_system_usb_session(&sys, true);
        assert(standby_system_usb_legacy_host(&sys) && sys.usb_mode == STANDBY_USB_LEGACY);
        drive(&env, &sys, 1000);
        assert(standby_system_sleep(&sys) == STANDBY_RC_BUSY);
        { StandbyStatus st; standby_system_get(&sys, &st); assert(st.inhibitors & BB_ST_INH_HOST); }

        /* refused or malformed registrations must not downgrade the inhibitor */
        assert(standby_system_presence(&sys, 0, true, STANDBY_SRC_USB) == STANDBY_RC_BAD_ARG);
        assert(standby_system_presence(&sys, 0, false, STANDBY_SRC_USB) == STANDBY_RC_BAD_ARG);
        assert(standby_system_usb_legacy_host(&sys));
        for (uint32_t id = 1; id <= STANDBY_MAX_CLIENTS; ++id)
            assert(standby_system_presence(&sys, id, true, STANDBY_SRC_OTHER) == STANDBY_RC_OK);
        assert(standby_system_presence(&sys, 99, true, STANDBY_SRC_USB) == STANDBY_RC_CAPACITY);
        assert(standby_system_usb_legacy_host(&sys));        /* full table: still legacy */
        /* another transport's registration is not the USB host speaking the API */
        assert(standby_system_presence(&sys, 1, true, STANDBY_SRC_OTHER) == STANDBY_RC_OK);
        assert(standby_system_usb_legacy_host(&sys));
        /* a release of an unknown id is a success and changes nothing */
        assert(standby_system_presence(&sys, 500, false, STANDBY_SRC_USB) == STANDBY_RC_OK);
        assert(standby_system_usb_legacy_host(&sys));
        for (uint32_t id = 1; id <= STANDBY_MAX_CLIENTS; ++id)
            assert(standby_system_presence(&sys, id, false, STANDBY_SRC_OTHER) == STANDBY_RC_OK);
        assert(standby_client_count(&sys.policy) == 0);
        assert(standby_system_sleep(&sys) == STANDBY_RC_BUSY);   /* the legacy session remains */

        /* explicit: the first accepted USB registration of the epoch */
        assert(standby_system_presence(&sys, 7, true, STANDBY_SRC_USB) == STANDBY_RC_OK);
        assert(!standby_system_usb_legacy_host(&sys) && sys.usb_mode == STANDBY_USB_EXPLICIT);
        assert(standby_system_sleep(&sys) == STANDBY_RC_BUSY);   /* the client lease itself */
        assert(standby_system_presence(&sys, 7, true, STANDBY_SRC_USB) == STANDBY_RC_OK);  /* renewal */
        assert(sys.usb_mode == STANDBY_USB_EXPLICIT);

        /* other clients and a local HOST (BLE) still count */
        assert(standby_system_presence(&sys, 8, true, STANDBY_SRC_OTHER) == STANDBY_RC_OK);
        assert(standby_system_presence(&sys, 7, false, STANDBY_SRC_USB) == STANDBY_RC_OK);
        assert(standby_client_count(&sys.policy) == 1);
        assert(standby_system_sleep(&sys) == STANDBY_RC_BUSY);
        assert(standby_system_presence(&sys, 8, false, STANDBY_SRC_OTHER) == STANDBY_RC_OK);
        env.inh = BB_ST_INH_HOST;
        assert(standby_system_sleep(&sys) == STANDBY_RC_BUSY);
        env.inh = 0;

        /* explicit release: gone although the CDC session is still open */
        assert(standby_client_count(&sys.policy) == 0 && sys.usb_mode == STANDBY_USB_EXPLICIT);
        { StandbyStatus st; standby_system_get(&sys, &st); assert(!(st.inhibitors & BB_ST_INH_HOST) && st.clients == 0); }
        assert(standby_system_presence(&sys, 7, false, STANDBY_SRC_USB) == STANDBY_RC_OK);   /* idempotent */
        assert(standby_system_sleep(&sys) == STANDBY_RC_OK);
        to_asleep(&env, &sys);
        assert(standby_system_presence(&sys, 7, false, STANDBY_SRC_USB) == STANDBY_RC_OK);   /* status calls work asleep */
        standby_system_wake(&sys); to_active(&env, &sys);

        /* a lease that is never renewed expires: the host is then gone too */
        assert(standby_system_presence(&sys, 9, true, STANDBY_SRC_USB) == STANDBY_RC_OK);
        drive(&env, &sys, STANDBY_CLIENT_TTL_MS + 2000);
        assert(standby_client_count(&sys.policy) == 0 && sys.usb_mode == STANDBY_USB_EXPLICIT);
        assert(!standby_system_usb_legacy_host(&sys));

        /* reconnect: every handshake starts a new LEGACY epoch, disconnect ends it */
        if (sys.policy.state != STANDBY_ACTIVE) { standby_system_wake(&sys); to_active(&env, &sys); }
        standby_system_usb_session(&sys, true);
        assert(sys.usb_mode == STANDBY_USB_LEGACY && standby_system_usb_legacy_host(&sys));
        assert(standby_system_sleep(&sys) == STANDBY_RC_BUSY);
        standby_system_usb_session(&sys, false);
        assert(sys.usb_mode == STANDBY_USB_NONE && !standby_system_usb_legacy_host(&sys));
        /* a registration with no session open never invents an explicit epoch */
        assert(standby_system_presence(&sys, 11, true, STANDBY_SRC_USB) == STANDBY_RC_OK);
        assert(sys.usb_mode == STANDBY_USB_NONE);
        assert(standby_system_presence(&sys, 11, false, STANDBY_SRC_USB) == STANDBY_RC_OK);
        standby_system_usb_session(&sys, true);
        assert(standby_system_presence(&sys, 12, true, STANDBY_SRC_USB) == STANDBY_RC_OK);
        assert(sys.usb_mode == STANDBY_USB_EXPLICIT);
        standby_system_usb_session(&sys, true);              /* the host reconnected */
        assert(sys.usb_mode == STANDBY_USB_LEGACY);          /* until it registers again */
        assert(standby_system_presence(&sys, 12, false, STANDBY_SRC_USB) == STANDBY_RC_OK);
        assert(standby_system_usb_legacy_host(&sys));
        puts("usb-epoch");
    }

    /* ---- an S3 reboot beside a HAT that is still asleep ------------------------------------- */
    {
        env_init(&env, &sys, 60, STANDBY_HAT_LINKED);
        to_asleep(&env, &sys);
        assert(env.hat.p4.state == BB_ST_ASLEEP && env.hat.p4.generation > 1u);
        s3_reboot(&env, &sys, 300);
        assert(sys.policy.state == STANDBY_ACTIVE && sys.policy.generation == 1u);
        drive(&env, &sys, 100);                            /* first poll: epoch rebased, nothing woken yet */
        assert(env.hat.p4.generation == 1u && sys.resyncs == 0 && sys.policy.state == STANDBY_ACTIVE);
        drive(&env, &sys, 200);
        { StandbyStatus st; standby_system_get(&sys, &st);
          assert(st.state == BB_ST_ACTIVE && !st.ready && sys.hat_remote_state == BB_ST_ASLEEP); }
        assert(standby_system_sleep(&sys) == STANDBY_RC_INVALID_STATE);   /* never "ready" beside a stale HAT */
        assert(drive_until(&env, &sys, STANDBY_WAKING, 2000));
        assert(sys.resyncs == 1);
        to_active(&env, &sys);
        assert(env.hat.p4.state == BB_ST_ACTIVE && !env.hat.p4.hw_paused);
        { int expect[] = {7, 8, 9, 10, 11};
          assert(env.nlocal == 5);
          for (int i = 0; i < 5; ++i) assert(env.local_log[i] == expect[i]); }
        { StandbyStatus st; standby_system_get(&sys, &st); assert(st.ready && st.state == BB_ST_ACTIVE); }
        drive(&env, &sys, 2000);
        assert(sys.resyncs == 1 && sys.policy.state == STANDBY_ACTIVE);    /* converged: no further wakes */

        /* a HAT stuck in any non-ACTIVE state is reconciled, never reported ready */
        static const uint8_t bad_states[] = { BB_ST_ASLEEP, BB_ST_PREPARING, BB_ST_WAKING, BB_ST_FAULT_SAFE };
        for (unsigned i = 0; i < sizeof(bad_states); ++i) {
            env_init(&env, &sys, 900, STANDBY_HAT_LINKED);
            drive(&env, &sys, 600);
            assert(sys.resyncs == 0);
            env.hat.p4.state = bad_states[i];
            if (bad_states[i] == BB_ST_FAULT_SAFE) { env.hat.p4.failure = SB_FAIL_HW; env.hat.p4.hw_paused = true; }
            drive(&env, &sys, 300);
            { StandbyStatus st; standby_system_get(&sys, &st); assert(!st.ready); }
            assert(drive_until(&env, &sys, STANDBY_WAKING, 2000) && sys.resyncs == 1);
            to_active(&env, &sys);
            drive(&env, &sys, 600);
            assert(env.hat.p4.state == BB_ST_ACTIVE && sys.resyncs == 1);
            { StandbyStatus st; standby_system_get(&sys, &st); assert(st.ready); }
        }

        /* the poll that proves nothing is a no-op: one stale reading does not wake anything */
        env_init(&env, &sys, 900, STANDBY_HAT_LINKED);
        drive(&env, &sys, 600);
        env.hat.p4.state = BB_ST_ASLEEP;
        drive(&env, &sys, 300);
        env.hat.p4.state = BB_ST_ACTIVE;
        drive(&env, &sys, 2000);
        assert(sys.resyncs == 0 && sys.policy.state == STANDBY_ACTIVE);

        /* an unproven or unreachable HAT never starts a wake of its own */
        env_init(&env, &sys, 900, STANDBY_HAT_LINKED);
        env.hat.silent = 1;
        drive(&env, &sys, 5000);
        assert(sys.resyncs == 0);
        puts("hat-resync");
    }

    /* ---- the boot-progress ack must carry OUR generation --------------------------------------- */
    {
        env_init(&env, &sys, 300, STANDBY_HAT_LINKED);
        env.hat.wrong_generation = 1;
        standby_system_boot_progress(&sys, BB_ST_BOOT_IO, 0, 0);
        assert(env.progress_frames == 1 && sys.boot_dirty);       /* answered, but for someone else's epoch */
        drive(&env, &sys, 600);
        assert(sys.boot_dirty && env.progress_frames > 2);         /* retried until a valid ack */
        env.hat.wrong_generation = 0;
        drive(&env, &sys, 600);
        assert(!sys.boot_dirty);
        puts("progress-ack-generation");
    }

    /* ---- persistence: real failure, ordering, one writer ---------------------------------------- */
    {
        env_init(&env, &sys, 300, STANDBY_HAT_NONE);
        env.persist_fail = 1;
        assert(standby_system_set_timeout(&sys, 900) == STANDBY_RC_HARDWARE);
        assert(sys.policy.timeout_ms == 300000u && env.persisted == 0 && env.persist_calls == 1);
        assert(!env.wlock_held);
        env.persist_fail = 0;
        assert(standby_system_set_timeout(&sys, 900) == STANDBY_RC_OK);
        assert(sys.policy.timeout_ms == 900000u && env.persisted == 900);
        env.persist_fail = 1;
        assert(standby_system_set_timeout(&sys, 60) == STANDBY_RC_HARDWARE);
        assert(sys.policy.timeout_ms == 900000u && env.persisted == 900);    /* previous choice kept */
        env.persist_fail = 0;

        int calls = env.persist_calls;
        assert(standby_system_set_timeout(&sys, 120) == STANDBY_RC_BAD_ARG && env.persist_calls == calls);
        assert(sys.policy.timeout_ms == 900000u);                       /* invalid value never reaches storage */

        env.hook_mode = 1;                                             /* a second writer inside persist */
        assert(standby_system_set_timeout(&sys, 300) == STANDBY_RC_OK);
        assert(env.hook_rc == STANDBY_RC_BUSY && env.hook_get_ok);
        assert(sys.policy.timeout_ms == 300000u && env.persisted == 300);   /* stored == running */
        assert(env.persist_calls == calls + 1);                        /* the refused writer never persisted */

        env.wlock_deny = 1;                                            /* barrier not acquired in time */
        assert(standby_system_set_timeout(&sys, 60) == STANDBY_RC_BUSY);
        assert(sys.policy.timeout_ms == 300000u && env.persisted == 300);
        env.wlock_deny = 0;
        assert(standby_system_set_timeout(&sys, 0) == STANDBY_RC_OK && env.persisted == 0 && sys.policy.timeout_ms == 0u);
        puts("policy-write-barrier");
    }

    /* ---- FAULT_SAFE forces outputs off until it is verified -------------------------------------- */
    {
        env_init(&env, &sys, 60, STANDBY_HAT_NONE);
        env.local_fail_step = STANDBY_OUTPUTS_OFF;
        env.safe_ok = 0;
        assert(drive_until(&env, &sys, STANDBY_FAULT_SAFE, 70000));
        assert(env.safe_calls == 1 && !sys.safe_ok);                  /* first attempt in the failing tick */
        { StandbyStatus st; standby_system_get(&sys, &st); assert(st.state == BB_ST_FAULT_SAFE && !st.ready); }
        drive(&env, &sys, STANDBY_SAFE_RETRY_MS - 100);
        assert(env.safe_calls == 1);
        drive(&env, &sys, 200);
        assert(env.safe_calls == 2);                                   /* retried, not forgotten */
        env.safe_ok = 1;
        drive(&env, &sys, STANDBY_SAFE_RETRY_MS + 100);
        assert(env.safe_calls == 3 && sys.safe_ok);
        drive(&env, &sys, 10000);
        assert(env.safe_calls == 3);                                   /* verified: no more attempts */
        { StandbyStatus st; standby_system_get(&sys, &st);
          assert(st.state == BB_ST_FAULT_SAFE && !st.ready && st.failed_stage == STANDBY_OUTPUTS_OFF); }

        /* recovery, then a new fault gets a fresh cleanup */
        env.local_fail_step = 0;
        standby_system_wake(&sys); to_active(&env, &sys);
        env.local_fail_step = STANDBY_MUX_OFF;
        assert(drive_until(&env, &sys, STANDBY_FAULT_SAFE, 70000));
        assert(env.safe_calls == 4);

        /* a policy deadline (nothing local failed) is cleaned up as well */
        env_init(&env, &sys, 60, STANDBY_HAT_NONE);
        env.local_pending_left = 1000000;
        drive(&env, &sys, 60000);
        assert(drive_until(&env, &sys, STANDBY_FAULT_SAFE, STANDBY_STEP_TIMEOUT_MS + 2000));
        drive(&env, &sys, 200);
        assert(env.safe_calls >= 1 && sys.safe_ok);
        puts("fault-safe-outputs");
    }

    /* ---- hardware unusable at boot: not ready, recoverable, truthful ------------------------------ */
    {
        env_init(&env, &sys, 300, STANDBY_HAT_NONE);
        standby_system_boot_fault(&sys, STANDBY_STEP_BIT(STANDBY_REINITIALIZE) | 1u);
        { StandbyStatus st; standby_system_get(&sys, &st);
          assert(st.state == BB_ST_ACTIVE && !st.ready && st.failed_stage == STANDBY_REINITIALIZE);
          assert(st.failed == STANDBY_STEP_BIT(STANDBY_REINITIALIZE)); }        /* bit 0 is not a stage */
        assert(standby_system_sleep(&sys) == STANDBY_RC_INVALID_STATE);
        assert(standby_system_work_begin(&sys) == STANDBY_ADMIT_OK);          /* USB / Wi-Fi tooling stays usable */
        standby_system_work_end(&sys);
        drive(&env, &sys, 400000);
        assert(sys.policy.state == STANDBY_ACTIVE && env.nlocal == 0);        /* never sleeps with a boot fault... */
        standby_system_wake(&sys);                                            /* ...but can be recovered */
        assert(sys.policy.state == STANDBY_WAKING && sys.resyncs == 1);
        to_active(&env, &sys);
        { StandbyStatus st; standby_system_get(&sys, &st);
          assert(st.ready && st.failed == 0 && st.failed_stage == 0 && sys.boot_fault_mask == 0u); }

        /* a recovery that fails says so and can be retried */
        env_init(&env, &sys, 300, STANDBY_HAT_NONE);
        standby_system_boot_fault(&sys, STANDBY_STEP_BIT(STANDBY_ANALOG_ON) | STANDBY_STEP_BIT(STANDBY_REINITIALIZE));
        env.local_fail_step = STANDBY_REINITIALIZE;
        standby_system_wake(&sys);
        assert(drive_until(&env, &sys, STANDBY_FAULT_SAFE, 5000));
        { StandbyStatus st; standby_system_get(&sys, &st);
          assert(!st.ready && st.failed_stage == STANDBY_REINITIALIZE);
          assert(st.failed & STANDBY_STEP_BIT(STANDBY_ANALOG_ON)); }          /* the boot record survives */
        env.local_fail_step = 0;
        standby_system_wake(&sys);
        to_active(&env, &sys);
        { StandbyStatus st; standby_system_get(&sys, &st); assert(st.ready && st.failed == 0); }
        puts("boot-fault");
    }
    return 0;
}
"""


def test_standby_system_with_real_p4_participant():
    output = compile_and_run(
        MAIN,
        sources=SOURCES,
        include_dirs=[POWER, P4_STANDBY],
        extra_flags=["-Werror"],
    )
    assert output.splitlines() == [
        "full-sleep-with-p4",
        "wake-on-request-no-replay",
        "work-blocks-then-fresh-timer",
        "atomic-recheck-cancel",
        "each-inhibitor",
        "hat-inhibitors",
        "fail-closed-hat",
        "missing-hat-skipped",
        "hat-busy-at-quiesce",
        "hat-busy-at-hat-sleep-recovers",
        "failure-then-recover",
        "local-failure",
        "generation-mismatch",
        "pending-deadline",
        "explicit-sleep",
        "wake-sources",
        "policy-persist",
        "boot-progress",
        "analog-gate",
        "usb-epoch",
        "hat-resync",
        "progress-ack-generation",
        "policy-write-barrier",
        "fault-safe-outputs",
        "boot-fault",
    ]
