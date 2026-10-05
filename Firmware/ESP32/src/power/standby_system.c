#include "standby_system.h"

#include <string.h>

#define SB_BIT(step) ((uint16_t)(1u << (step)))

static uint32_t sys_now(const StandbySystem *s) { return s->ops.now_ms(s->ops.ctx); }
static void sys_lock(const StandbySystem *s) { s->ops.lock(s->ops.ctx); }
static void sys_unlock(const StandbySystem *s) { s->ops.unlock(s->ops.ctx); }

uint8_t standby_system_state_code(const StandbyPolicy *p)
{
    switch (p->state) {
    case STANDBY_ACTIVE: return BB_ST_ACTIVE;
    case STANDBY_PREPARING: return BB_ST_PREPARING;
    case STANDBY_ASLEEP: return BB_ST_ASLEEP;
    case STANDBY_WAKING: return BB_ST_WAKING;
    default: return BB_ST_FAULT_SAFE;
    }
}

static void sync_flags(StandbySystem *s)
{
    s->analog_ok = s->policy.state == STANDBY_ACTIVE;
}

/* The HAT reports a state other than ACTIVE while this side is ACTIVE. */
static bool hat_inconsistent(const StandbySystem *s)
{
    return s->hat_state_valid && s->hat_supported && s->hat_remote_state != BB_ST_ACTIVE &&
           s->ops.hat_link(s->ops.ctx) == STANDBY_HAT_LINKED;
}

/* Ready = the system really is usable, not just "not in a transition". */
static bool is_ready(const StandbySystem *s)
{
    return s->policy.state == STANDBY_ACTIVE && s->boot_fault_mask == 0u && !hat_inconsistent(s);
}

/* Everything that refuses sleep: the S3 owners, the HAT's report, a HAT whose
 * standby support is not proven (fail closed) and operations in flight. */
static uint32_t total_inhibitors(const StandbySystem *s, uint32_t local)
{
    uint32_t total = local | s->hat_inhibitors;
    StandbyHatLink link = s->ops.hat_link(s->ops.ctx);
    if (link == STANDBY_HAT_DETECTED || (link == STANDBY_HAT_LINKED && !s->hat_supported))
        total |= BB_ST_INH_HAT_UNKNOWN;
    if (s->work_depth != 0) total |= BB_ST_INH_WORK;
    /* Never sleep on top of a failed boot check or a HAT that is not in the same state. */
    if (s->boot_fault_mask != 0u) total |= BB_ST_INH_SELFTEST;
    if (s->policy.state == STANDBY_ACTIVE && hat_inconsistent(s)) total |= BB_ST_INH_HAT_UNKNOWN;
    return total;
}

static uint32_t policy_view(const StandbySystem *s, uint32_t total)
{
    (void)s;
    return total;
}

void standby_system_init(StandbySystem *s, const StandbyOps *ops, uint32_t timeout_seconds)
{
    memset(s, 0, sizeof(*s));
    s->ops = *ops;
    standby_init(&s->policy, sys_now(s), timeout_seconds);
    s->seen_generation = s->policy.generation;
    s->hat_poll_due_ms = sys_now(s);
    sync_flags(s);
    s->initialised = true;
}

/* ---- HAT exchange ----------------------------------------------------------- */

static void fill_request(const StandbySystem *s, bb_standby_request_t *rq, uint8_t op,
                         uint8_t stage, uint32_t generation)
{
    memset(rq, 0, sizeof(*rq));
    rq->schema = BB_STANDBY_SCHEMA;
    rq->op = op;
    rq->state = standby_system_state_code(&s->policy);
    rq->stage = stage;
    rq->generation = generation;
    rq->timeout_seconds = (uint16_t)(s->policy.timeout_ms / 1000u);
    rq->completed = (uint16_t)s->policy.completed_steps;
    rq->failed = s->failed_mask;
    rq->skipped = s->skipped_mask;
}

static void send_boot_progress(StandbySystem *s)
{
    if (s->ops.hat_link(s->ops.ctx) != STANDBY_HAT_LINKED) return;
    bb_standby_request_t rq;
    bb_standby_reply_t rp;
    fill_request(s, &rq, BB_ST_OP_PROGRESS, BB_ST_BOOT_STAGE, s->policy.generation);
    rq.completed = s->boot_completed;
    rq.failed = s->boot_failed;
    rq.skipped = s->boot_skipped;
    /* Only an ack of THIS generation counts: it proves the HAT adopted our epoch. */
    if (s->ops.hat_exchange(s->ops.ctx, &rq, &rp) == STANDBY_XCHG_OK &&
        rp.schema == BB_STANDBY_SCHEMA && rp.failure == 0u && rp.generation == rq.generation)
        s->boot_dirty = false;
}

void standby_system_boot_progress(StandbySystem *s, uint16_t completed, uint16_t failed,
                                  uint16_t skipped)
{
    s->boot_completed |= completed;
    s->boot_failed |= failed;
    s->boot_skipped |= skipped;
    s->boot_dirty = true;
    send_boot_progress(s);
}

/* The HAT reported its own state (under lock). A HAT that stays out of step with an ACTIVE
 * system - this side rebooted while it slept, a transaction was abandoned - is brought back
 * with a full coordinated wake instead of being reported ready. */
static void note_hat_state(StandbySystem *s, const bb_standby_reply_t *rp, bool from_poll)
{
    s->hat_remote_state = rp->state;
    s->hat_state_valid = true;
    if (rp->state == BB_ST_ACTIVE || s->policy.state != STANDBY_ACTIVE) {
        s->hat_mismatch_polls = 0;
        return;
    }
    if (!from_poll) return;
    /* A HAT on an older epoch is rebased by the boot-progress frame first; it cannot
     * accept a wake of this epoch before that. */
    if (s->boot_dirty ||
        (rp->generation != 0u && (int32_t)(rp->generation - s->policy.generation) > 0))
        return;
    if (s->hat_mismatch_polls < 255u) s->hat_mismatch_polls++;
    if (s->hat_mismatch_polls >= STANDBY_HAT_MISMATCH_POLLS) {
        s->hat_mismatch_polls = 0;
        if (standby_resync(&s->policy, sys_now(s))) s->resyncs++;
    }
}

static void poll_hat(StandbySystem *s, uint32_t now)
{
    if (s->exec_active || s->ops.hat_link(s->ops.ctx) != STANDBY_HAT_LINKED) return;
    if (s->policy.state == STANDBY_PREPARING || s->policy.state == STANDBY_WAKING) return;
    if (s->hat_poll_started && (int32_t)(now - s->hat_poll_due_ms) < 0) return;
    s->hat_poll_started = true;
    s->hat_poll_due_ms = now + STANDBY_HAT_POLL_MS;

    bb_standby_request_t rq;
    bb_standby_reply_t rp;
    fill_request(s, &rq, BB_ST_OP_POLL, 0, s->policy.generation);
    StandbyXchg x = s->ops.hat_exchange(s->ops.ctx, &rq, &rp);

    sys_lock(s);
    if (x == STANDBY_XCHG_OK && rp.schema == BB_STANDBY_SCHEMA) {
        s->hat_supported = true;
        s->hat_poll_failures = 0;
        s->hat_inhibitors = rp.inhibitors;
        if (s->hat_activity_valid && (int32_t)(rp.activity - s->hat_activity) > 0)
            standby_request_wake(&s->policy, sys_now(s));   // button / USB / C6 menu
        s->hat_activity = rp.activity;
        s->hat_activity_valid = true;
        note_hat_state(s, &rp, true);
    } else if (x == STANDBY_XCHG_UNSUPPORTED) {
        s->hat_supported = false;
        s->hat_inhibitors = 0;
        s->hat_activity_valid = false;
        s->hat_state_valid = false;
        s->hat_mismatch_polls = 0;
    } else if (s->hat_poll_failures < 255u) {
        s->hat_poll_failures++;
        if (s->hat_poll_failures >= 3u) s->hat_supported = false;
        if (s->hat_poll_failures >= STANDBY_HAT_POLL_FAIL_WAKE &&
            s->policy.state == STANDBY_ASLEEP)
            standby_request_wake(&s->policy, sys_now(s));   // lost the wake path: recover loudly
    }
    bool rebase = x == STANDBY_XCHG_OK && rp.schema == BB_STANDBY_SCHEMA &&
                  rp.generation != 0u &&
                  (int32_t)(rp.generation - s->policy.generation) > 0 &&
                  s->policy.state == STANDBY_ACTIVE;
    sync_flags(s);
    sys_unlock(s);
    if (rebase || s->boot_dirty) send_boot_progress(s);   // a rebooted S3 starts a new epoch
}

/* ---- step execution ----------------------------------------------------------- */

typedef enum { RUN_DONE, RUN_PENDING, RUN_FAILED, RUN_CANCEL } RunResult;

static RunResult run_hat_part(StandbySystem *s, uint32_t now)
{
    StandbyHatLink link = s->ops.hat_link(s->ops.ctx);
    if (link == STANDBY_HAT_NONE) {
        s->exec_hat_skipped = true;
        return RUN_DONE;
    }
    if ((int32_t)(now - s->exec_hat_due_ms) < 0 && s->exec_hat_started) return RUN_PENDING;
    s->exec_hat_started = true;
    s->exec_hat_due_ms = now + STANDBY_HAT_STAGE_RETRY_MS;

    bb_standby_request_t rq;
    bb_standby_reply_t rp;
    fill_request(s, &rq, s->exec_step <= STANDBY_INDICATORS_OFF ? BB_ST_OP_SLEEP : BB_ST_OP_WAKE,
                 (uint8_t)s->exec_step, s->exec_generation);
    StandbyXchg x = s->ops.hat_exchange(s->ops.ctx, &rq, &rp);
    if (x == STANDBY_XCHG_TIMEOUT) return RUN_PENDING;   // bounded by the policy step deadline
    if (x == STANDBY_XCHG_UNSUPPORTED) {
        s->hat_supported = false;
        return RUN_FAILED;
    }
    if (rp.schema != BB_STANDBY_SCHEMA) return RUN_FAILED;
    s->hat_supported = true;
    s->hat_inhibitors = rp.inhibitors;
    s->hat_remote_state = rp.state;
    s->hat_state_valid = true;
    if (rp.failure == 1u) return RUN_CANCEL;              // SB_FAIL_BUSY
    if (rp.failure == 5u) return RUN_PENDING;             // SB_FAIL_ORDER: an earlier stage is still draining
    if (rp.failure != 0u) return RUN_FAILED;              // STALE / ARG / HW / TIMEOUT
    if (rp.generation != s->exec_generation) return RUN_FAILED;   // not our transaction
    return rp.ready ? RUN_DONE : RUN_PENDING;
}

static RunResult run_local_part(StandbySystem *s)
{
    StandbyStepResult r = s->ops.local_step(s->ops.ctx, s->exec_step, s->exec_generation);
    if (r == STANDBY_STEP_DONE) return RUN_DONE;
    return r == STANDBY_STEP_PENDING ? RUN_PENDING : RUN_FAILED;
}

static void finish_step(StandbySystem *s, RunResult r)
{
    uint32_t now = sys_now(s);
    sys_lock(s);
    StandbyPolicy *p = &s->policy;
    if (s->exec_hat_skipped) s->skipped_mask |= SB_BIT(s->exec_step);
    if (r == RUN_DONE) {
        standby_complete(p, s->exec_generation, s->exec_step, true, now);
        if (s->exec_step == STANDBY_INDICATORS_ON && p->state == STANDBY_ACTIVE) {
            s->boot_fault_mask = 0u;       // every stage came back: the boot fault is recovered
            s->hat_mismatch_polls = 0;
        }
    } else if (r == RUN_FAILED) {
        s->failed_mask |= SB_BIT(s->exec_step);
        standby_complete(p, s->exec_generation, s->exec_step, false, now);
    } else if (r == RUN_CANCEL) {
        if (s->exec_step == STANDBY_QUIESCE) {
            standby_cancel_prepare(p, s->exec_generation, now);
        } else {
            standby_request_wake(p, now);
            standby_complete(p, s->exec_generation, s->exec_step, true, now);
        }
    }
    s->exec_active = false;
    sync_flags(s);
    sys_unlock(s);
}

static void run_exec(StandbySystem *s)
{
    uint32_t now = sys_now(s);
    const bool hat_first = s->exec_step == STANDBY_QUIESCE;
    RunResult r = RUN_DONE;

    for (int pass = 0; pass < 2 && r == RUN_DONE; ++pass) {
        const bool hat_pass = hat_first ? pass == 0 : pass == 1;
        if (hat_pass) {
            if (!s->exec_hat_done) {
                r = run_hat_part(s, now);
                if (r == RUN_DONE) s->exec_hat_done = true;
            }
        } else if (!s->exec_local_done) {
            r = run_local_part(s);
            if (r == RUN_DONE) s->exec_local_done = true;
        }
    }
    if (r == RUN_PENDING) return;
    finish_step(s, r);
}

/* ---- FAULT_SAFE ------------------------------------------------------------------------- */

/* A failed sequence can leave outputs powered that its own steps never reached (an early
 * return, a half-finished shutdown). While FAULT_SAFE, force them off - retried every
 * STANDBY_SAFE_RETRY_MS until the cleanup reports a verified result. The state itself is
 * never relaxed: FAULT_SAFE stays FAULT_SAFE whatever this returns. */
static void maintain_safe_state(StandbySystem *s)
{
    if (!s->ops.safe_outputs_off) return;
    uint32_t now = sys_now(s);
    sys_lock(s);
    const uint32_t gen = s->policy.generation;
    const bool need = s->policy.state == STANDBY_FAULT_SAFE &&
                      !(s->safe_ok && s->safe_gen == gen) &&
                      (int32_t)(now - s->safe_due_ms) >= 0;
    sys_unlock(s);
    if (!need) return;

    bool ok = s->ops.safe_outputs_off(s->ops.ctx);

    sys_lock(s);
    s->safe_attempts++;
    s->safe_ok = ok;
    s->safe_gen = gen;
    s->safe_due_ms = now + (ok ? 0u : STANDBY_SAFE_RETRY_MS);
    sys_unlock(s);
}

/* ---- tick -------------------------------------------------------------------------- */

void standby_system_tick(StandbySystem *s)
{
    if (!s->initialised) return;
    uint32_t now = sys_now(s);
    poll_hat(s, now);
    uint32_t local = s->ops.inhibitors(s->ops.ctx);

    sys_lock(s);
    StandbyPolicy *p = &s->policy;
    if (s->exec_active && !p->pending) s->exec_active = false;   // the policy gave up (deadline)

    uint32_t total = total_inhibitors(s, local);
    s->last_inhibitors = total;
    StandbyStep step = standby_tick(p, now, policy_view(s, total));
    if (p->generation != s->seen_generation) {
        s->seen_generation = p->generation;
        s->failed_mask = 0;
        s->skipped_mask = 0;
    }
    if (step == STANDBY_QUIESCE) {
        /* Atomic with admission: work_begin() takes the same lock and refuses once
         * the state left ACTIVE, so nothing can slip in between this re-check and
         * the commit below. */
        uint32_t again = policy_view(s, total_inhibitors(s, s->ops.inhibitors(s->ops.ctx)));
        if (again != 0u) {
            standby_cancel_prepare(p, p->generation, now);
            s->cancelled_prepares++;
            step = STANDBY_NO_STEP;
        }
    }
    if (step != STANDBY_NO_STEP) {
        s->exec_active = true;
        s->exec_step = step;
        s->exec_generation = p->generation;
        s->exec_local_done = false;
        s->exec_hat_done = false;
        s->exec_hat_started = false;
        s->exec_hat_skipped = false;
        s->exec_hat_due_ms = now;
    }
    sync_flags(s);
    bool run = s->exec_active;
    sys_unlock(s);

    if (run) run_exec(s);
    maintain_safe_state(s);
}

/* ---- barrier ------------------------------------------------------------------------- */

StandbyAdmit standby_system_work_begin(StandbySystem *s)
{
    uint32_t now = sys_now(s);
    sys_lock(s);
    StandbyAdmit rc;
    if (s->policy.state == STANDBY_ACTIVE) {
        s->work_depth++;
        s->work_admitted++;
        standby_activity(&s->policy, now);
        rc = STANDBY_ADMIT_OK;
    } else {
        s->work_refused++;
        rc = s->policy.state == STANDBY_FAULT_SAFE ? STANDBY_ADMIT_FAULT : STANDBY_ADMIT_WAKING;
        standby_request_wake(&s->policy, now);
    }
    sync_flags(s);
    sys_unlock(s);
    return rc;
}

void standby_system_work_end(StandbySystem *s)
{
    uint32_t now = sys_now(s);
    sys_lock(s);
    if (s->work_depth != 0) s->work_depth--;
    if (s->policy.state == STANDBY_ACTIVE) standby_activity(&s->policy, now);   // fresh idle interval
    sys_unlock(s);
}

bool standby_system_analog_allowed(const StandbySystem *s) { return s->analog_ok; }

bool standby_system_analog_enter(StandbySystem *s)
{
    sys_lock(s);
    bool ok = s->policy.state == STANDBY_ACTIVE;
    if (ok) s->analog_depth++;
    sys_unlock(s);
    return ok;
}

void standby_system_analog_leave(StandbySystem *s)
{
    sys_lock(s);
    if (s->analog_depth != 0) s->analog_depth--;
    sys_unlock(s);
}

uint32_t standby_system_analog_depth(StandbySystem *s)
{
    sys_lock(s);
    uint32_t d = s->analog_depth;
    sys_unlock(s);
    return d;
}

void standby_system_activity(StandbySystem *s)
{
    uint32_t now = sys_now(s);
    sys_lock(s);
    standby_activity(&s->policy, now);
    sync_flags(s);
    sys_unlock(s);
}

/* ---- explicit requests ------------------------------------------------------------------- */

StandbyRc standby_system_presence(StandbySystem *s, uint32_t client_id, bool present,
                                  StandbySource src)
{
    if (client_id == 0u) return STANDBY_RC_BAD_ARG;
    uint32_t now = sys_now(s);
    sys_lock(s);
    bool ok = standby_presence(&s->policy, client_id, present, now);
    /* Only an ACCEPTED registration proves the host speaks the presence API. A refused or
     * malformed one must leave the legacy session counting. */
    if (ok && present && src == STANDBY_SRC_USB && s->usb_mode == STANDBY_USB_LEGACY)
        s->usb_mode = STANDBY_USB_EXPLICIT;
    sync_flags(s);
    sys_unlock(s);
    if (ok) return STANDBY_RC_OK;
    /* A refused release means "was not registered": the client is gone either way. */
    return present ? STANDBY_RC_CAPACITY : STANDBY_RC_OK;
}

void standby_system_usb_session(StandbySystem *s, bool open)
{
    s->usb_mode = open ? STANDBY_USB_LEGACY : STANDBY_USB_NONE;
    if (open) standby_system_activity(s);   // a real new host session wakes a sleeping system
}

bool standby_system_usb_legacy_host(const StandbySystem *s)
{
    return s->usb_mode == STANDBY_USB_LEGACY;
}

StandbyRc standby_system_set_timeout(StandbySystem *s, uint32_t seconds)
{
    if (!standby_timeout_valid(seconds)) return STANDBY_RC_BAD_ARG;

    /* One writer at a time, so what is stored and what runs are written in the same order.
     * The state lock is NOT held across the storage write: every other request stays live. */
    const bool serialised = s->ops.write_lock != NULL;
    if (serialised && !s->ops.write_lock(s->ops.ctx)) return STANDBY_RC_BUSY;

    StandbyRc rc = STANDBY_RC_OK;
    if (s->ops.persist_timeout && !s->ops.persist_timeout(s->ops.ctx, seconds))
        rc = STANDBY_RC_HARDWARE;   // not stored: the previous value stays in force
    if (rc == STANDBY_RC_OK) {
        uint32_t now = sys_now(s);
        sys_lock(s);
        if (!standby_set_timeout(&s->policy, seconds, now)) rc = STANDBY_RC_BAD_ARG;
        sync_flags(s);
        sys_unlock(s);
    }

    if (serialised && s->ops.write_unlock) s->ops.write_unlock(s->ops.ctx);
    return rc;
}

void standby_system_boot_fault(StandbySystem *s, uint16_t failed_steps)
{
    sys_lock(s);
    s->boot_fault_mask |= (uint16_t)(failed_steps & ~1u);
    sys_unlock(s);
}

void standby_system_wake(StandbySystem *s)
{
    uint32_t now = sys_now(s);
    sys_lock(s);
    standby_request_wake(&s->policy, now);
    /* A system that booted with unusable hardware is retried through the full wake chain. */
    if (s->boot_fault_mask != 0u && standby_resync(&s->policy, now)) s->resyncs++;
    sync_flags(s);
    sys_unlock(s);
}

StandbyRc standby_system_sleep(StandbySystem *s)
{
    sys_lock(s);
    StandbyRc rc;
    if (!is_ready(s)) {
        rc = STANDBY_RC_INVALID_STATE;
    } else {
        uint32_t total = total_inhibitors(s, s->ops.inhibitors(s->ops.ctx));
        if (total != 0u || standby_client_count(&s->policy) != 0) {
            rc = STANDBY_RC_BUSY;
        } else {
            standby_request_sleep(&s->policy);
            rc = STANDBY_RC_OK;
        }
    }
    sys_unlock(s);
    return rc;
}

/* ---- reporting ------------------------------------------------------------------------------- */

void standby_system_get(StandbySystem *s, StandbyStatus *out)
{
    uint32_t now = sys_now(s);
    sys_lock(s);
    const StandbyPolicy *p = &s->policy;
    memset(out, 0, sizeof(*out));
    out->state = standby_system_state_code(p);
    out->ready = is_ready(s) ? 1u : 0u;
    out->stage = (uint8_t)p->step;
    out->generation = p->generation;
    out->timeout_seconds = (uint16_t)(p->timeout_ms / 1000u);
    out->clients = standby_client_count(p);
    out->failed_stage = p->state == STANDBY_FAULT_SAFE ? (uint8_t)p->failed_step : 0u;
    for (unsigned bit = 1; bit < 16u && out->failed_stage == 0u; ++bit)
        if (s->boot_fault_mask & (1u << bit)) out->failed_stage = (uint8_t)bit;
    out->inhibitors = total_inhibitors(s, s->ops.inhibitors(s->ops.ctx));
    out->completed = (uint16_t)p->completed_steps;
    out->failed = (uint16_t)(s->failed_mask | s->boot_fault_mask);
    out->skipped = s->skipped_mask;
    out->idle_remaining_ms = standby_idle_remaining_ms(p, now);
    sys_unlock(s);
}
