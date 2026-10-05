#include "standby_p4_core.h"

#include <string.h>

static int32_t gen_diff(uint32_t a, uint32_t b) { return (int32_t)(a - b); }

static bool timeout_valid(uint16_t s)
{
    return s == 0u || s == 60u || s == 300u || s == 900u;
}

static bool is_c6_stage(uint8_t stage)
{
    return stage == SB_STAGE_INDICATORS_OFF || stage == SB_STAGE_WAKE_SAFE ||
           stage == SB_STAGE_INDICATORS_ON;
}

void sb_p4_init(sb_p4_t *c)
{
    memset(c, 0, sizeof(*c));
    c->state = BB_ST_ACTIVE;
    c->c6_expected = true;      // fail closed: the production board has a display until told otherwise
}

bool sb_p4_blocked(const sb_p4_t *c)
{
    return c->barrier || c->state != BB_ST_ACTIVE;
}

void sb_p4_note_activity(sb_p4_t *c)
{
    c->activity++;
    if (sb_p4_blocked(c)) c->wake_pending = true;
}

bool sb_p4_admit(sb_p4_t *c)
{
    if (sb_p4_blocked(c)) {
        c->activity++;
        c->wake_pending = true;
        return false;
    }
    c->entered_seq++;
    c->inflight++;
    return true;
}

bool sb_p4_admit_quiet(sb_p4_t *c)
{
    if (sb_p4_blocked(c)) return false;
    c->entered_seq++;
    c->inflight++;
    return true;
}

void sb_p4_leave(sb_p4_t *c)
{
    if (c->inflight) c->inflight--;
}

uint32_t sb_p4_entry_seq(const sb_p4_t *c)
{
    return c->entered_seq;
}

// ---------------------------------------------------------------------------
// Leases
// ---------------------------------------------------------------------------
static bool lease_live(const sb_lease_t *l, uint32_t now_ms)
{
    return l->id != 0u && gen_diff(l->expires_ms, now_ms) > 0;
}

uint32_t sb_p4_lease_count(const sb_p4_t *c, uint32_t now_ms)
{
    uint32_t n = 0;
    for (unsigned i = 0; i < SB_LEASE_MAX; ++i) {
        if (lease_live(&c->leases[i], now_ms)) n++;
    }
    return n;
}

uint8_t sb_p4_lease(sb_p4_t *c, uint8_t op, uint32_t id, uint32_t ttl_ms, uint32_t now_ms,
                    uint32_t *count)
{
    uint8_t result = SB_FAIL_NONE;
    if (id == 0u || op > 1u) {
        result = SB_FAIL_ARG;
    } else if (op == 0u) {
        for (unsigned i = 0; i < SB_LEASE_MAX; ++i) {
            if (c->leases[i].id == id) {
                c->leases[i].id = 0u;
                c->activity++;   // a client leaving is meaningful: it restarts the idle timer
            }
        }
    } else {
        if (ttl_ms == 0u) ttl_ms = SB_LEASE_TTL_DEFAULT_MS;
        if (ttl_ms < SB_LEASE_TTL_MIN_MS) ttl_ms = SB_LEASE_TTL_MIN_MS;
        if (ttl_ms > SB_LEASE_TTL_MAX_MS) ttl_ms = SB_LEASE_TTL_MAX_MS;

        sb_lease_t *slot = NULL;
        for (unsigned i = 0; i < SB_LEASE_MAX; ++i) {
            if (c->leases[i].id == id) { slot = &c->leases[i]; break; }
        }
        const bool fresh = slot == NULL;
        for (unsigned i = 0; i < SB_LEASE_MAX && !slot; ++i) {
            if (!lease_live(&c->leases[i], now_ms)) slot = &c->leases[i];
        }
        if (!slot) {
            result = SB_FAIL_FULL;   // never evict a live lease: a client would silently lose presence
        } else {
            slot->id = id;
            slot->expires_ms = now_ms + ttl_ms;
            if (fresh) {
                c->activity++;
                if (sb_p4_blocked(c)) c->wake_pending = true;
            }
        }
    }
    if (count) *count = sb_p4_lease_count(c, now_ms);
    return result;
}

void sb_p4_lease_ack(const sb_p4_t *c, uint8_t failure, uint32_t now_ms, bb_standby_reply_t *out)
{
    memset(out, 0, sizeof(*out));
    out->schema = BB_STANDBY_SCHEMA;
    out->state = c->state;
    out->failure = failure;
    if (failure == SB_FAIL_NONE && c->state != BB_ST_ACTIVE) out->failure = SB_FAIL_BUSY;
    out->ready = out->failure == SB_FAIL_NONE ? 1u : 0u;
    out->generation = c->gen_valid ? c->generation : 0u;
    out->inhibitors = sb_p4_lease_count(c, now_ms);   // live leases, not an inhibitor mask
    out->activity = c->activity;
}

uint32_t sb_p4_inhibitors(const sb_p4_t *c, uint32_t local_inhibitors, uint32_t now_ms)
{
    uint32_t r = local_inhibitors;
    if (c->c6_present) r |= c->c6_inhibitors;
    if (c->c6_unsupported || c->c6_missing) r |= BB_ST_INH_HAT_UNKNOWN;
    if (sb_p4_lease_count(c, now_ms) != 0u) r |= BB_ST_INH_HOST;
    if (c->inflight != 0u || c->wake_pending) r |= BB_ST_INH_WORK;
    return r;
}

// ---------------------------------------------------------------------------
// S3 request
// ---------------------------------------------------------------------------
static void fill_reply(const sb_p4_t *c, bb_standby_reply_t *rp, uint32_t inhibitors,
                       bool ready, uint8_t failure)
{
    rp->schema = BB_STANDBY_SCHEMA;
    rp->state = c->state;
    rp->ready = ready ? 1u : 0u;
    rp->failure = failure;
    rp->generation = c->gen_valid ? c->generation : 0u;
    rp->inhibitors = inhibitors;
    rp->activity = c->activity;
}

static void complete_stage(sb_p4_t *c, uint8_t stage)
{
    c->steps_done |= (uint16_t)(1u << stage);
    c->steps_failed &= (uint16_t)~(1u << stage);   // a retry that worked is no longer a failure
    c->step_running = 0;
    c->failure = SB_FAIL_NONE;
}

static void finish_wake(sb_p4_t *c)
{
    c->state = BB_ST_ACTIVE;
    c->barrier = false;
    c->wake_pending = false;
    c->hw_paused = false;
    c->c6_failed = false;
    c->c6_dark = false;
}

static void start_c6_stage(sb_p4_t *c, uint8_t stage, uint32_t now_ms, sb_p4_fx_t *fx)
{
    c->step_running = stage;
    c->step_started_ms = now_ms;
    c->step_last_forward_ms = now_ms;
    fx->forward_c6 = true;
    fx->forward_stage = stage;
}

static void start_worker(sb_p4_t *c, uint8_t stage, uint32_t now_ms, sb_p4_fx_t *fx)
{
    c->step_running = stage;
    c->step_started_ms = now_ms;
    fx->run_stage = stage;
    fx->run_generation = c->generation;
}

static void fail_stage(sb_p4_t *c, uint8_t stage)
{
    c->step_running = 0;
    c->failure = SB_FAIL_HW;
    c->state = BB_ST_FAULT_SAFE;       // the barrier stays up: nothing runs against the hardware
    c->steps_failed |= (uint16_t)(1u << stage);
}

// A new transaction supersedes the stage in flight. Its completion can no longer be
// accepted (wrong generation), so stop waiting for it - but keep what it may already
// have done on record, so the new wake still undoes it.
static void abandon_stage(sb_p4_t *c)
{
    if (c->step_running == SB_STAGE_HAT_SLEEP) c->hw_paused = true;
    c->step_running = 0;
}

void sb_p4_handle(sb_p4_t *c, const bb_standby_request_t *rq, uint32_t now_ms,
                  uint32_t entry_seq, uint32_t local_inhibitors,
                  bb_standby_reply_t *rp, sb_p4_fx_t *fx)
{
    memset(rp, 0, sizeof(*rp));
    memset(fx, 0, sizeof(*fx));
    const uint32_t inh = sb_p4_inhibitors(c, local_inhibitors, now_ms);

    if (rq->schema != BB_STANDBY_SCHEMA || rq->op > BB_ST_OP_PROGRESS) {
        fill_reply(c, rp, inh, false, SB_FAIL_ARG);
        return;
    }

    switch (rq->op) {
    case BB_ST_OP_POLL: {
        if (timeout_valid(rq->timeout_seconds)) {
            c->timeout_s = rq->timeout_seconds;
            c->timeout_known = true;
        }
        const bool stable = (c->state == BB_ST_ACTIVE || c->state == BB_ST_ASLEEP) &&
                            c->step_running == 0u;
        fill_reply(c, rp, inh, stable, c->failure);
        return;
    }

    case BB_ST_OP_PROGRESS: {
        if (rq->stage == BB_ST_BOOT_STAGE) {
            // An S3 boot starts a new epoch: rebase the generation so a rebooted
            // S3 can never be locked out by this side's counter.
            if (!c->gen_valid || c->generation != rq->generation) {
                abandon_stage(c);
                c->generation = rq->generation;
                c->gen_valid = true;
                c->steps_done = 0;
                c->steps_failed = 0;
                c->failure = SB_FAIL_NONE;
            }
            if (timeout_valid(rq->timeout_seconds)) {
                c->timeout_s = rq->timeout_seconds;
                c->timeout_known = true;
            }
            c->boot_completed = (uint16_t)(rq->completed & ~BB_ST_BOOT_P4);
            c->boot_failed = (uint16_t)(rq->failed & ~BB_ST_BOOT_P4);
            c->boot_skipped = (uint16_t)(rq->skipped & ~BB_ST_BOOT_P4);
            c->boot_seen = true;
            fx->forward_c6 = true;
            fx->forward_stage = 0;
            fill_reply(c, rp, inh, true, SB_FAIL_NONE);
            return;
        }
        if (c->gen_valid && gen_diff(rq->generation, c->generation) < 0) {
            fill_reply(c, rp, inh, false, SB_FAIL_STALE);
            return;
        }
        if (timeout_valid(rq->timeout_seconds)) {
            c->timeout_s = rq->timeout_seconds;
            c->timeout_known = true;
        }
        fx->forward_c6 = true;
        fx->forward_stage = rq->stage;
        fill_reply(c, rp, inh, true, SB_FAIL_NONE);
        return;
    }

    default:
        break;   // SLEEP / WAKE below
    }

    const uint8_t stage = rq->stage;
    const bool sleep_op = rq->op == BB_ST_OP_SLEEP;
    if (sleep_op ? (stage < SB_STAGE_QUIESCE || stage > SB_STAGE_INDICATORS_OFF)
                 : (stage < SB_STAGE_WAKE_SAFE || stage > SB_STAGE_INDICATORS_ON)) {
        fill_reply(c, rp, inh, false, SB_FAIL_ARG);
        return;
    }

    if (c->gen_valid && gen_diff(rq->generation, c->generation) < 0) {
        fill_reply(c, rp, inh, false, SB_FAIL_STALE);
        return;
    }
    if (!c->gen_valid || gen_diff(rq->generation, c->generation) > 0) {
        abandon_stage(c);
        c->generation = rq->generation;
        c->gen_valid = true;
        c->steps_done = 0;
        c->steps_failed = 0;
        c->failure = SB_FAIL_NONE;
    }
    if (timeout_valid(rq->timeout_seconds)) {
        c->timeout_s = rq->timeout_seconds;
        c->timeout_known = true;
    }

    const uint16_t bit = (uint16_t)(1u << stage);
    if (c->steps_done & bit) {                    // duplicate of a finished stage
        fill_reply(c, rp, inh, true, SB_FAIL_NONE);
        return;
    }
    if (c->step_running != 0u) {
        // Same stage still executing: not ready yet. A different stage: refuse.
        fill_reply(c, rp, inh, false,
                   c->step_running == stage ? SB_FAIL_NONE : SB_FAIL_ORDER);
        return;
    }
    if (c->state == BB_ST_FAULT_SAFE && sleep_op) {
        // Nothing may sleep from a fault: keep reporting the original cause.
        fill_reply(c, rp, inh, false, c->failure != SB_FAIL_NONE ? c->failure : SB_FAIL_ORDER);
        return;
    }
    c->failure = SB_FAIL_NONE;

    uint8_t fail = SB_FAIL_NONE;
    bool ready = false;

    switch (stage) {
    case SB_STAGE_QUIESCE:
        if (c->state == BB_ST_ACTIVE) {
            if (inh != 0u) { fail = SB_FAIL_BUSY; break; }
            c->state = BB_ST_PREPARING;
        } else if (c->state != BB_ST_PREPARING && c->state != BB_ST_ASLEEP) {
            fail = SB_FAIL_ORDER;
            break;
        }
        complete_stage(c, stage);
        ready = true;
        break;

    case SB_STAGE_HAT_SLEEP:
        if (c->state == BB_ST_ASLEEP) {
            complete_stage(c, stage);
            ready = true;
        } else if (c->state != BB_ST_PREPARING) {
            fail = SB_FAIL_ORDER;
        } else if (c->entered_seq != entry_seq || inh != 0u) {
            // Work entered after the snapshot, or something still refuses sleep.
            fail = SB_FAIL_BUSY;
        } else {
            c->barrier = true;                    // atomic with the recheck above
            start_worker(c, stage, now_ms, fx);
        }
        break;

    case SB_STAGE_MUX_OFF:
        // The analog multiplexers are disconnected FIRST, with every analog rail still
        // present, and the pads are read back. Needs the pause of stage 2 (acquisition
        // stopped, converter configuration saved).
        if (c->state != BB_ST_ASLEEP || !c->hw_paused) { fail = SB_FAIL_ORDER; break; }
        c->routes_held = true;                    // conservative: held from the moment it is dispatched
        start_worker(c, stage, now_ms, fx);
        break;

    case SB_STAGE_OUTPUTS_OFF:
        // Nothing left for the P4 to switch (the DUT supply went off at stage 2); it
        // only checks the order so a stray request cannot pass.
        if (c->state != BB_ST_ASLEEP || !(c->steps_done & (1u << SB_STAGE_MUX_OFF))) { fail = SB_FAIL_ORDER; break; }
        complete_stage(c, stage);
        ready = true;
        break;

    case SB_STAGE_ANALOG_OFF:
        // The DAQ analog supplies (+/-24 V, +/-26 V, analog 3V3). Not one of them moves
        // unless stage 3 completed AT THIS generation; the worker re-reads the pads too.
        if (c->state != BB_ST_ASLEEP || !c->hw_paused ||
            !(c->steps_done & (1u << SB_STAGE_MUX_OFF))) { fail = SB_FAIL_ORDER; break; }
        c->analog_cut = true;                     // conservative: true from the moment it is dispatched
        start_worker(c, stage, now_ms, fx);
        break;

    case SB_STAGE_INDICATORS_OFF:
        if (c->state != BB_ST_ASLEEP) { fail = SB_FAIL_ORDER; break; }
        if (c->c6_unsupported || c->c6_missing) { fail = SB_FAIL_HW; break; }   // cannot prove the screen is off
        if (!c->c6_present) { complete_stage(c, stage); ready = true; break; }
        c->c6_dark = true;                        // assumed from the moment it is asked
        start_c6_stage(c, stage, now_ms, fx);
        break;

    case SB_STAGE_WAKE_SAFE:
        if (c->state != BB_ST_ACTIVE) c->state = BB_ST_WAKING;
        if (!c->c6_present || !c->c6_dark) {
            complete_stage(c, stage);             // nothing was turned off: nothing to repeat
            ready = true;
            break;
        }
        start_c6_stage(c, stage, now_ms, fx);
        break;

    case SB_STAGE_ANALOG_ON:
        if (c->state != BB_ST_WAKING && c->state != BB_ST_ACTIVE &&
            c->state != BB_ST_FAULT_SAFE) { fail = SB_FAIL_ORDER; break; }
        if (c->analog_cut) {
            start_worker(c, stage, now_ms, fx);   // rails back, PG proven, shared reset released
            break;
        }
        complete_stage(c, stage);                 // the rails were never switched off
        ready = true;
        fx->forward_c6 = c->c6_present;
        fx->forward_stage = stage;
        break;

    case SB_STAGE_REINITIALIZE:
        if (c->state != BB_ST_WAKING && c->state != BB_ST_ACTIVE &&
            c->state != BB_ST_FAULT_SAFE) { fail = SB_FAIL_ORDER; break; }
        if (c->analog_cut) {
            if (!(c->steps_done & (1u << SB_STAGE_ANALOG_ON))) { fail = SB_FAIL_ORDER; break; }
            start_worker(c, stage, now_ms, fx);   // converters reset, identified, reconfigured
            break;
        }
        complete_stage(c, stage);
        ready = true;
        fx->forward_c6 = c->c6_present;
        fx->forward_stage = stage;
        break;

    case SB_STAGE_HAT_WAKE:
        if (c->state != BB_ST_WAKING && c->state != BB_ST_ACTIVE &&
            c->state != BB_ST_FAULT_SAFE) { fail = SB_FAIL_ORDER; break; }
        if (c->analog_cut) { fail = SB_FAIL_ORDER; break; }   // the converters are not restored
        if (!c->hw_paused) {
            complete_stage(c, stage);
            ready = true;
            fx->forward_c6 = c->c6_present;
            fx->forward_stage = stage;
            break;
        }
        start_worker(c, stage, now_ms, fx);
        break;

    case SB_STAGE_INDICATORS_ON:
        if (c->hw_paused) { fail = SB_FAIL_ORDER; break; }    // stage 10 was skipped
        if (!c->c6_present || !c->c6_dark) {
            finish_wake(c);
            complete_stage(c, stage);
            ready = true;
            break;
        }
        if (c->state != BB_ST_ACTIVE) c->state = BB_ST_WAKING;
        start_c6_stage(c, stage, now_ms, fx);
        break;

    default:
        fail = SB_FAIL_ARG;
        break;
    }

    if (fail != SB_FAIL_NONE) c->failure = fail;
    fill_reply(c, rp, inh, ready, fail);
}

bool sb_p4_step_begin(const sb_p4_t *c, uint8_t stage, uint32_t generation)
{
    return c->gen_valid && c->generation == generation && c->step_running == stage;
}

static void apply_completion(sb_p4_t *c, uint8_t stage, bool ok, bool fast_was_running)
{
    switch (stage) {
    case SB_STAGE_HAT_SLEEP:
        // Conservative: even on failure part of the pause may have happened, so a
        // later wake must attempt the resume.
        c->hw_paused = true;
        c->fast_was_running = fast_was_running;
        if (ok) {
            c->state = BB_ST_ASLEEP;
            complete_stage(c, stage);
        } else {
            fail_stage(c, stage);
        }
        return;

    case SB_STAGE_MUX_OFF:
    case SB_STAGE_ANALOG_OFF:
    case SB_STAGE_ANALOG_ON:
        if (ok) complete_stage(c, stage);
        else fail_stage(c, stage);
        return;

    case SB_STAGE_REINITIALIZE:
        if (ok) {
            c->analog_cut = false;               // the converters are configured again
            complete_stage(c, stage);
        } else {
            fail_stage(c, stage);
        }
        return;

    case SB_STAGE_HAT_WAKE:
        if (ok) {
            c->hw_paused = false;
            // Acquisition is NOT restarted here: that would poll the range logic and
            // reconnect the analog path. The first explicit request after ACTIVE does it.
            if (c->fast_was_running) c->acq_resume_pending = true;
            c->fast_was_running = false;
            if (c->state == BB_ST_FAULT_SAFE) c->state = BB_ST_WAKING;   // the fault is cleared
            complete_stage(c, stage);
        } else {
            fail_stage(c, stage);
        }
        return;

    default:
        c->step_running = 0;
        return;
    }
}

bool sb_p4_step_complete(sb_p4_t *c, uint8_t stage, uint32_t generation, bool ok,
                         bool fast_was_running)
{
    // A completion belongs to ONE transaction. A worker that was queued before the
    // generation moved on, or that outlived its timeout, changes nothing.
    if (!sb_p4_step_begin(c, stage, generation)) return false;
    apply_completion(c, stage, ok, fast_was_running);
    return true;
}

void sb_p4_set_boot_p4(sb_p4_t *c, bool ok)
{
    c->boot_p4 = ok ? SB_BOOT_P4_OK : SB_BOOT_P4_FAILED;
}

void sb_p4_routes_connected(sb_p4_t *c)
{
    c->routes_held = false;
}

bool sb_p4_take_acq_resume(sb_p4_t *c)
{
    const bool pending = c->acq_resume_pending;
    c->acq_resume_pending = false;
    return pending;
}

// ---------------------------------------------------------------------------
// C6
// ---------------------------------------------------------------------------
void sb_p4_set_c6_expected(sb_p4_t *c, bool expected)
{
    c->c6_expected = expected;
    c->c6_missing = expected && !c->c6_present && !c->c6_unsupported;
}

void sb_p4_set_c6(sb_p4_t *c, bool link_up, bool responsive)
{
    c->c6_present = link_up && responsive;
    c->c6_unsupported = link_up && !responsive;
    c->c6_missing = !link_up && c->c6_expected;
    if (!c->c6_present) c->c6_inhibitors = 0;
}

static void c6_step_failed(sb_p4_t *c, uint8_t failure)
{
    const uint8_t stage = c->step_running;
    c->step_running = 0;
    c->failure = failure;
    c->c6_failed = true;
    if (stage == SB_STAGE_INDICATORS_ON) {
        // The measurement path is already back (stage 10): leave the instrument
        // usable, but keep the failure visible and let a re-sent stage retry.
        c->state = BB_ST_ACTIVE;
        c->barrier = false;
        c->wake_pending = false;
        c->hw_paused = false;
    }
}

void sb_p4_c6_reply(sb_p4_t *c, const bb_standby_reply_t *r, uint32_t now_ms)
{
    (void)now_ms;
    if (r->schema != BB_STANDBY_SCHEMA) return;
    c->c6_inhibitors = r->inhibitors;
    if (c->c6_activity_valid) {
        if (gen_diff(r->activity, c->c6_activity_seen) > 0) c->activity++;
    }
    c->c6_activity_seen = r->activity;            // a lower value just means the C6 rebooted
    c->c6_activity_valid = true;

    const uint8_t stage = c->step_running;
    if (!is_c6_stage(stage) || r->generation != c->generation) return;

    if (r->failure != SB_FAIL_NONE) { c6_step_failed(c, SB_FAIL_HW); return; }
    if (!r->ready) return;

    const uint8_t expect = stage == SB_STAGE_INDICATORS_OFF ? BB_ST_ASLEEP
                         : stage == SB_STAGE_WAKE_SAFE      ? BB_ST_WAKING
                                                            : BB_ST_ACTIVE;
    if (r->state != expect) return;

    c->c6_confirmed_gen = r->generation;
    c->c6_confirmed_stage = stage;
    c->c6_failed = false;
    if (stage == SB_STAGE_INDICATORS_ON) finish_wake(c);
    complete_stage(c, stage);
}

void sb_p4_build_c6_request(const sb_p4_t *c, uint8_t forward_stage,
                            const bb_standby_request_t *from_s3,
                            bb_standby_request_t *out)
{
    memset(out, 0, sizeof(*out));
    out->schema = BB_STANDBY_SCHEMA;
    out->state = c->state;
    out->generation = c->gen_valid ? c->generation : 0u;
    out->timeout_seconds = c->timeout_known ? c->timeout_s : 0xFFFFu;

    // The Analyzer milestone is the P4's own bring-up result; the S3 is never
    // allowed to claim it.
    const uint16_t p4_done = c->boot_p4 == SB_BOOT_P4_OK ? BB_ST_BOOT_P4 : 0u;
    const uint16_t p4_fail = c->boot_p4 == SB_BOOT_P4_FAILED ? BB_ST_BOOT_P4 : 0u;

    if (forward_stage == 0u) {
        out->op = BB_ST_OP_POLL;
        if (c->boot_seen) {
            out->stage = BB_ST_BOOT_STAGE;
            out->completed = (uint16_t)(c->boot_completed | p4_done);
            out->failed = (uint16_t)(c->boot_failed | p4_fail);
            out->skipped = c->boot_skipped;
        } else if (c->boot_p4 != SB_BOOT_P4_PENDING) {
            out->stage = SB_STAGE_BOOT_P4_ONLY;
            out->completed = p4_done;
            out->failed = p4_fail;
        }
        return;
    }
    out->stage = forward_stage;
    out->op = from_s3 && from_s3->op == BB_ST_OP_PROGRESS ? BB_ST_OP_PROGRESS
            : forward_stage <= SB_STAGE_INDICATORS_OFF   ? BB_ST_OP_SLEEP
                                                         : BB_ST_OP_WAKE;
    out->completed = (uint16_t)(c->steps_done | (from_s3 ? from_s3->completed : 0u));
    out->failed = (uint16_t)(c->steps_failed | (from_s3 ? from_s3->failed : 0u));
    out->skipped = from_s3 ? from_s3->skipped : 0u;
}

bool sb_p4_tick(sb_p4_t *c, uint32_t now_ms)
{
    bool reforward = false;
    const uint8_t stage = c->step_running;
    if (stage != 0u) {
        const uint32_t age = now_ms - c->step_started_ms;
        if (is_c6_stage(stage)) {
            if (age >= SB_C6_STEP_TIMEOUT_MS) {
                c6_step_failed(c, SB_FAIL_TIMEOUT);
            } else if ((now_ms - c->step_last_forward_ms) >= SB_C6_RETRY_MS) {
                c->step_last_forward_ms = now_ms;
                reforward = true;
            }
        } else if (age >= SB_WORKER_TIMEOUT_MS) {
            fail_stage(c, stage);
        }
    }
    return reforward;
}

// ---------------------------------------------------------------------------
// Physical buttons
// ---------------------------------------------------------------------------
void sb_btn_gate(sb_btn_gate_t *g, bool blocked, bool any_held, bool any_raw, uint8_t raw_events,
                 uint32_t now_ms, sb_btn_out_t *out)
{
    out->events = 0;
    out->activity = false;
    out->wake_press = false;
    out->start_discard = false;

    const bool down = any_held || any_raw;       // a pad pulled low counts before the debouncer confirms it
    const bool press_edge = any_held && !g->prev_held;
    g->prev_held = any_held;

    if (blocked) {
        if (press_edge || raw_events) {
            out->wake_press = true;
            out->activity = true;
        }
        if (down || raw_events) {
            if (!g->consuming) out->start_discard = true;
            g->consuming = true;
            g->holdoff_until_ms = now_ms + SB_BTN_RELEASE_HOLDOFF_MS;
        }
        return;
    }
    if (g->consuming) {
        // Held, auto-repeating, or the release-time event: all swallowed, and each one
        // restarts the window. The gesture ends only when every pad is up, no event is
        // left, and the release debounce has run out - so a BACK or OK the driver
        // already had in flight when the system woke can never flip anything.
        if (down || raw_events) {
            g->holdoff_until_ms = now_ms + SB_BTN_RELEASE_HOLDOFF_MS;
        } else if ((int32_t)(now_ms - g->holdoff_until_ms) >= 0) {
            g->consuming = false;
        }
        return;
    }
    out->events = raw_events;
    if (press_edge || raw_events) out->activity = true;
}
