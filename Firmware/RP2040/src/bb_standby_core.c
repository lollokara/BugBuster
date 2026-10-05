#include "bb_standby_core.h"

#include <string.h>

// The only seam for host tests: they pull the HAT_CMD_* macros out of bb_config.h and
// define BB_SB_HOST_TEST, because bb_config.h itself needs the Pico SDK headers.
#ifndef BB_SB_HOST_TEST
#include "bb_config.h"
#endif

static int32_t tdiff(uint32_t a, uint32_t b) { return (int32_t)(a - b); }

static bool elapsed(uint32_t now_ms, uint32_t then_ms, uint32_t span_ms)
{
    return tdiff(now_ms, then_ms) >= (int32_t)span_ms;
}

void bb_sb_init(bb_sb_t *c)
{
    memset(c, 0, sizeof(*c));
    c->state = BB_ST_ACTIVE;
}

// ---------------------------------------------------------------------------
// Operation barrier
// ---------------------------------------------------------------------------
bool bb_sb_blocked(const bb_sb_t *c)
{
    return c->barrier || c->state != BB_ST_ACTIVE;
}

bool bb_sb_monitor_allowed(const bb_sb_t *c)
{
    return !bb_sb_blocked(c);
}

void bb_sb_note_activity(bb_sb_t *c)
{
    c->activity++;
    if (bb_sb_blocked(c)) c->wake_pending = true;
}

bool bb_sb_admit(bb_sb_t *c)
{
    if (bb_sb_blocked(c)) {
        c->activity++;
        c->wake_pending = true;
        return false;
    }
    c->activity++;
    c->entered_seq++;
    c->inflight++;
    return true;
}

void bb_sb_leave(bb_sb_t *c)
{
    if (c->inflight) c->inflight--;
}

uint32_t bb_sb_entry_seq(const bb_sb_t *c)
{
    return c->entered_seq;
}

// ---------------------------------------------------------------------------
// CMSIS-DAP logical session
// ---------------------------------------------------------------------------
static bool dap_live(const bb_sb_t *c, uint32_t now_ms)
{
    return c->dap_open && !elapsed(now_ms, c->dap_last_ms, BB_SB_DAP_TTL_MS);
}

bool bb_sb_dap_enter(bb_sb_t *c, uint32_t now_ms)
{
    if (bb_sb_blocked(c)) {
        c->activity++;
        c->wake_pending = true;
        return false;
    }
    c->entered_seq++;
    c->dap_inflight++;
    c->dap_frames++;
    c->dap_last_ms = now_ms;
    return true;
}

void bb_sb_dap_leave(bb_sb_t *c, uint32_t now_ms, bool port_open)
{
    if (c->dap_inflight) c->dap_inflight--;
    c->dap_last_ms = now_ms;
    if (port_open && !c->dap_open) {
        c->dap_open = true;
        c->dap_connects++;
        c->activity++;
    } else if (!port_open && c->dap_open) {
        c->dap_open = false;
        c->dap_disconnects++;
        c->activity++;               // the logical host left: a fresh idle interval starts
    }
}

void bb_sb_dap_host_gone(bb_sb_t *c)
{
    if (c->dap_open) {
        c->dap_open = false;
        c->dap_disconnects++;
        c->activity++;
    }
}

// ---------------------------------------------------------------------------
// Inhibitors
// ---------------------------------------------------------------------------
static uint32_t facts_mask(const bb_sb_facts_t *f)
{
    uint32_t m = 0;
    if (f->la_armed) m |= BB_ST_INH_TRIGGER;
    if (f->la_capturing || f->la_streaming || f->la_usb_session || f->la_usb_pending)
        m |= BB_ST_INH_STREAM;
    if (f->fw_update) m |= BB_ST_INH_OTA;
    if (f->calibration) m |= BB_ST_INH_CALIBRATION;
    if (f->uart_bridge) m |= BB_ST_INH_UART;
    if (f->bus) m |= BB_ST_INH_BUS;
    return m;
}

uint32_t bb_sb_inhibitors(const bb_sb_t *c, const bb_sb_facts_t *f, uint32_t now_ms)
{
    uint32_t m = facts_mask(f);
    if (dap_live(c, now_ms) || c->dap_inflight != 0u) m |= BB_ST_INH_SWD;
    if (c->inflight != 0u || c->wake_pending || f->cmd_pending) m |= BB_ST_INH_WORK;
    return m;
}

// ---------------------------------------------------------------------------
// Command classification
// ---------------------------------------------------------------------------
bb_sb_cmd_class_t bb_sb_classify(uint8_t cmd, const uint8_t *payload, uint8_t len)
{
    switch (cmd) {
    case BB_HAT_CMD_STANDBY:
        return BB_SB_CMD_STANDBY;

    // Status reads, cleanup and indicator updates never need a powered rail.
    case HAT_CMD_PING:
    case HAT_CMD_GET_INFO:
    case HAT_CMD_GET_PIN_CONFIG:
    case HAT_CMD_GET_CAPS:
    case HAT_CMD_GET_POWER_STATUS:
    case HAT_CMD_GET_IO_VOLTAGE:
    case HAT_CMD_GET_DAP_STATUS:
    case HAT_CMD_LA_GET_STATUS:
    case HAT_CMD_LA_READ_DATA:
    case HAT_CMD_LA_STOP:
    case HAT_CMD_LA_LOG_ENABLE:
    case HAT_CMD_LA_USB_RESET:
    case HAT_CMD_GET_RAIL_STATUS:
    case HAT_CMD_SET_LED_STATE:
    case HAT_CMD_CALIBRATE_STATUS:
    case HAT_CMD_CALIBRATE_EXPORT:
    case HAT_CMD_FW_CHUNK:
    case HAT_CMD_FW_COMMIT:
    case HAT_CMD_FW_STATUS:
        return BB_SB_CMD_FREE;

    // "Turn it off" is always safe, even asleep, and is not new work.
    case HAT_CMD_SET_POWER:
    case HAT_CMD_SET_RAIL_ENABLE:
        return (payload != NULL && len >= 2u && payload[1] == 0u) ? BB_SB_CMD_FREE
                                                                  : BB_SB_CMD_WORK;
    case HAT_CMD_SET_LEVEL_SHIFT:
        return (payload != NULL && len >= 1u && payload[0] == 0u) ? BB_SB_CMD_FREE
                                                                  : BB_SB_CMD_WORK;

    case HAT_CMD_SET_PIN_CONFIG:
    case HAT_CMD_RESET:
    case HAT_CMD_SET_IO_VOLTAGE:
    case HAT_CMD_GET_TARGET_INFO:
    case HAT_CMD_SET_SWD_CLOCK:
    case HAT_CMD_LA_CONFIG:
    case HAT_CMD_LA_SET_TRIGGER:
    case HAT_CMD_LA_ARM:
    case HAT_CMD_LA_FORCE:
    case HAT_CMD_LA_STREAM_START:
    case HAT_CMD_LA_USB_SEND:
    case HAT_CMD_LA_SET_ROUTE:
    case HAT_CMD_CALIBRATE_START:
    case HAT_CMD_CALIBRATE_IMPORT:
    case HAT_CMD_SET_IO_BANK:
    case HAT_CMD_SET_RAIL_VOLTAGE:
    case HAT_CMD_FW_BEGIN:
        return BB_SB_CMD_WORK;

    default:
        return BB_SB_CMD_FREE;       // unknown: the dispatcher answers INVALID_CMD
    }
}

// ---------------------------------------------------------------------------
// S3 request
// ---------------------------------------------------------------------------
static void fill_reply(const bb_sb_t *c, bb_standby_reply_t *rp, uint32_t inhibitors,
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

static void complete_stage(bb_sb_t *c, uint8_t stage)
{
    c->steps_done |= (uint16_t)(1u << stage);
    c->step_running = 0;
    c->failure = BB_SB_FAIL_NONE;
}

static void finish_wake(bb_sb_t *c)
{
    c->state = BB_ST_ACTIVE;
    c->barrier = false;
    c->wake_pending = false;
    c->hw_paused = false;
}

static void cancel_prepare(bb_sb_t *c)
{
    c->state = BB_ST_ACTIVE;
    c->barrier = false;
    c->wake_pending = false;
    c->steps_done = 0;
    c->failure = BB_SB_FAIL_NONE;
    c->activity++;
}

void bb_sb_handle(bb_sb_t *c, const bb_standby_request_t *rq, uint32_t now_ms,
                  uint32_t entry_seq, const bb_sb_facts_t *f,
                  bb_standby_reply_t *rp, bb_sb_fx_t *fx)
{
    memset(rp, 0, sizeof(*rp));
    memset(fx, 0, sizeof(*fx));
    const uint32_t inh = bb_sb_inhibitors(c, f, now_ms);

    if (rq->schema != BB_STANDBY_SCHEMA || rq->op > BB_ST_OP_PROGRESS) {
        fill_reply(c, rp, inh, false, BB_SB_FAIL_ARG);
        return;
    }
    c->last_s3_ms = now_ms;
    c->s3_seen = true;

    switch (rq->op) {
    case BB_ST_OP_POLL: {
        const bool stable = (c->state == BB_ST_ACTIVE || c->state == BB_ST_ASLEEP) &&
                            c->step_running == 0u;
        fill_reply(c, rp, inh, stable, c->failure);
        return;
    }

    case BB_ST_OP_PROGRESS:
        if (rq->stage == BB_ST_BOOT_STAGE) {
            // An S3 boot starts a new epoch: rebase the generation so a rebooted S3
            // can never be locked out by this side's counter.
            if (!c->gen_valid || c->generation != rq->generation) {
                c->generation = rq->generation;
                c->gen_valid = true;
                c->steps_done = 0;
                if (c->step_running == 0u) c->failure = BB_SB_FAIL_NONE;
            }
            fill_reply(c, rp, inh, true, BB_SB_FAIL_NONE);
            return;
        }
        // No display on this HAT: the progress is not applied, and says so.
        fill_reply(c, rp, inh, false, BB_SB_FAIL_NONE);
        return;

    default:
        break;   // SLEEP / WAKE below
    }

    const uint8_t stage = rq->stage;
    const bool sleep_op = rq->op == BB_ST_OP_SLEEP;
    if (sleep_op ? (stage < BB_SB_STAGE_QUIESCE || stage > BB_SB_STAGE_INDICATORS_OFF)
                 : (stage < BB_SB_STAGE_WAKE_SAFE || stage > BB_SB_STAGE_INDICATORS_ON)) {
        fill_reply(c, rp, inh, false, BB_SB_FAIL_ARG);
        return;
    }

    if (c->gen_valid && tdiff(rq->generation, c->generation) < 0) {
        fill_reply(c, rp, inh, false, BB_SB_FAIL_STALE);
        return;
    }
    if (!c->gen_valid || tdiff(rq->generation, c->generation) > 0) {
        c->generation = rq->generation;
        c->gen_valid = true;
        c->steps_done = 0;
        if (c->step_running == 0u) c->failure = BB_SB_FAIL_NONE;
    }

    const uint16_t bit = (uint16_t)(1u << stage);
    if (c->steps_done & bit) {                    // duplicate of a finished stage
        fill_reply(c, rp, inh, true, BB_SB_FAIL_NONE);
        return;
    }
    if (c->step_running != 0u) {
        fill_reply(c, rp, inh, false,
                   c->step_running == stage ? BB_SB_FAIL_NONE : BB_SB_FAIL_ORDER);
        return;
    }
    if (c->state == BB_ST_FAULT_SAFE && sleep_op) {
        // Nothing may sleep from a fault: keep reporting the original cause.
        fill_reply(c, rp, inh, false,
                   c->failure != BB_SB_FAIL_NONE ? c->failure : BB_SB_FAIL_ORDER);
        return;
    }
    c->failure = BB_SB_FAIL_NONE;

    uint8_t fail = BB_SB_FAIL_NONE;
    bool ready = false;

    switch (stage) {
    case BB_SB_STAGE_QUIESCE:
        if (c->state == BB_ST_ACTIVE) {
            if (inh != 0u || c->entered_seq != entry_seq) { fail = BB_SB_FAIL_BUSY; break; }
            c->state = BB_ST_PREPARING;           // blocked from here: monitors stop, work waits
        } else if (c->state != BB_ST_PREPARING && c->state != BB_ST_ASLEEP) {
            fail = BB_SB_FAIL_ORDER;
            break;
        }
        complete_stage(c, stage);
        ready = true;
        break;

    case BB_SB_STAGE_HAT_SLEEP:
        if (c->state == BB_ST_ASLEEP) {
            complete_stage(c, stage);
            ready = true;
        } else if (c->state != BB_ST_PREPARING) {
            fail = BB_SB_FAIL_ORDER;
        } else if (c->entered_seq != entry_seq || inh != 0u) {
            fail = BB_SB_FAIL_BUSY;               // work arrived, or something still refuses sleep
        } else {
            c->barrier = true;                    // atomic with the recheck above
            c->step_running = stage;
            fx->run_stage = stage;
        }
        break;

    case BB_SB_STAGE_MUX_OFF:
    case BB_SB_STAGE_OUTPUTS_OFF:
    case BB_SB_STAGE_ANALOG_OFF:
        // Mainboard-only work. The RP2040 only checks the order.
        if (c->state != BB_ST_ASLEEP) { fail = BB_SB_FAIL_ORDER; break; }
        complete_stage(c, stage);
        ready = true;
        break;

    case BB_SB_STAGE_INDICATORS_OFF:
        if (c->state != BB_ST_ASLEEP) { fail = BB_SB_FAIL_ORDER; break; }
        if (c->leds_dark) { complete_stage(c, stage); ready = true; break; }
        c->step_running = stage;
        fx->run_stage = stage;
        break;

    case BB_SB_STAGE_WAKE_SAFE:
        if (c->state != BB_ST_ACTIVE) c->state = BB_ST_WAKING;
        if (!c->hw_paused) {                      // nothing was powered down: nothing to re-assert
            complete_stage(c, stage);
            ready = true;
            break;
        }
        c->step_running = stage;                  // outputs are re-asserted OFF; none is enabled
        fx->run_stage = stage;
        break;

    case BB_SB_STAGE_ANALOG_ON:
    case BB_SB_STAGE_REINITIALIZE:
        if (c->state != BB_ST_WAKING && c->state != BB_ST_ACTIVE &&
            c->state != BB_ST_FAULT_SAFE) { fail = BB_SB_FAIL_ORDER; break; }
        complete_stage(c, stage);
        ready = true;
        break;

    case BB_SB_STAGE_HAT_WAKE:
        if (c->state != BB_ST_WAKING && c->state != BB_ST_ACTIVE &&
            c->state != BB_ST_FAULT_SAFE) { fail = BB_SB_FAIL_ORDER; break; }
        if (!c->hw_paused) {
            complete_stage(c, stage);
            ready = true;
            break;
        }
        c->step_running = stage;
        fx->run_stage = stage;
        break;

    case BB_SB_STAGE_INDICATORS_ON:
        if (c->hw_paused) { fail = BB_SB_FAIL_ORDER; break; }   // stage 10 was skipped
        if (!c->leds_dark) {
            finish_wake(c);
            complete_stage(c, stage);
            ready = true;
            break;
        }
        if (c->state != BB_ST_ACTIVE) c->state = BB_ST_WAKING;
        c->step_running = stage;
        fx->run_stage = stage;
        break;

    default:
        fail = BB_SB_FAIL_ARG;
        break;
    }

    // BUSY / ORDER are answers to this one request, not state: a later POLL must not
    // keep reporting a refusal that no longer applies. Only hardware failures stick.
    fill_reply(c, rp, inh, ready, fail);
}

void bb_sb_step_done(bb_sb_t *c, uint8_t stage, bool ok)
{
    if (c->step_running != stage) return;          // late or duplicate completion

    switch (stage) {
    case BB_SB_STAGE_HAT_SLEEP:
        // Conservative: even on failure part of the shutdown happened, so wake must
        // re-assert the safe state.
        c->hw_paused = true;
        if (ok) {
            c->state = BB_ST_ASLEEP;
            complete_stage(c, stage);
        } else {
            c->step_running = 0;
            c->failure = BB_SB_FAIL_HW;
            c->state = BB_ST_FAULT_SAFE;           // barrier stays: nothing runs
        }
        return;

    case BB_SB_STAGE_INDICATORS_OFF:
        if (ok) {
            c->leds_dark = true;
            complete_stage(c, stage);
        } else {
            c->step_running = 0;
            c->failure = BB_SB_FAIL_HW;            // outputs are already off: stay ASLEEP
        }
        return;

    case BB_SB_STAGE_WAKE_SAFE:
    case BB_SB_STAGE_HAT_WAKE:
        if (ok) {
            if (stage == BB_SB_STAGE_HAT_WAKE) c->hw_paused = false;
            complete_stage(c, stage);
        } else {
            c->step_running = 0;
            c->failure = BB_SB_FAIL_HW;
            c->state = BB_ST_FAULT_SAFE;
        }
        return;

    case BB_SB_STAGE_INDICATORS_ON:
        if (ok) {
            c->leds_dark = false;
            finish_wake(c);
            complete_stage(c, stage);
        } else {
            // The measurement path is back: leave the instrument usable, keep the
            // failure visible, and let a re-sent stage retry the indicators.
            finish_wake(c);
            c->step_running = 0;
            c->failure = BB_SB_FAIL_HW;
        }
        return;

    default:
        c->step_running = 0;
        return;
    }
}

bool bb_sb_run_stage(uint8_t stage, const bb_sb_ops_t *o)
{
    bool ok = true;
    switch (stage) {
    case BB_SB_STAGE_HAT_SLEEP:
    case BB_SB_STAGE_WAKE_SAFE:
        // Output drivers and OE first, then the rails, logic-side rail last. Every
        // step is attempted even after an error so the hardware ends as safe as it can.
        if (!o->outputs_disable(o->ctx)) ok = false;
        if (!o->pins_highz(o->ctx)) ok = false;
        if (!o->la_route_off(o->ctx)) ok = false;
        if (!o->rail_off(o->ctx, BB_SB_RAIL_VADJ3)) ok = false;
        if (!o->rail_off(o->ctx, BB_SB_RAIL_VADJ4)) ok = false;
        if (!o->rail_off(o->ctx, BB_SB_RAIL_3V3_ADJ)) ok = false;
        if (!o->verify_safe(o->ctx)) ok = false;
        return ok;

    case BB_SB_STAGE_HAT_WAKE:
        // Internal readiness only. No output, OE or route is enabled here.
        if (!o->restore_ready(o->ctx)) ok = false;
        if (!o->verify_safe(o->ctx)) ok = false;
        return ok;

    case BB_SB_STAGE_INDICATORS_OFF:
        return o->leds(o->ctx, true);

    case BB_SB_STAGE_INDICATORS_ON:
        return o->leds(o->ctx, false);

    default:
        return false;
    }
}

void bb_sb_final_reply(const bb_sb_t *c, uint8_t stage, uint32_t inhibitors,
                       bb_standby_reply_t *rp)
{
    const bool ready = stage < 16u && c->step_running == 0u &&
                       ((c->steps_done >> stage) & 1u) != 0u &&
                       c->failure == BB_SB_FAIL_NONE;
    fill_reply(c, rp, inhibitors, ready, c->failure);
}

void bb_sb_tick(bb_sb_t *c, uint32_t now_ms, const bb_sb_facts_t *f, bb_sb_fx_t *fx)
{
    memset(fx, 0, sizeof(*fx));

    if (c->dap_open && c->dap_inflight == 0u && elapsed(now_ms, c->dap_last_ms, BB_SB_DAP_TTL_MS)) {
        c->dap_open = false;                       // a vanished host stops blocking standby
        c->dap_expired++;
        c->activity++;
    }

    if (c->state == BB_ST_PREPARING && c->step_running == 0u && c->s3_seen &&
        elapsed(now_ms, c->last_s3_ms, BB_SB_PREPARE_TIMEOUT_MS)) {
        cancel_prepare(c);
    }

    // An owner starting or finishing is activity. Command-queue depth is not: the S3's
    // own periodic polls would otherwise keep resetting its idle timer.
    const uint32_t m = facts_mask(f);
    if (c->prev_valid && m != c->prev_owner_inh) c->activity++;
    c->prev_owner_inh = m;
    c->prev_valid = true;

    if (c->wake_pending && bb_sb_blocked(c)) {
        if (!c->irq_armed || elapsed(now_ms, c->irq_last_ms, BB_SB_IRQ_REPEAT_MS)) {
            fx->irq = true;
            c->irq_armed = true;
            c->irq_last_ms = now_ms;
        }
    } else {
        c->irq_armed = false;
    }
}

void bb_sb_service(bb_sb_t *c, const bb_sb_env_t *env, const bb_sb_ops_t *ops,
                   const uint8_t *payload, uint8_t len, bb_standby_reply_t *rp)
{
    bb_standby_request_t rq;
    memset(&rq, 0, sizeof(rq));
    if (payload != NULL && len == sizeof(rq)) memcpy(&rq, payload, sizeof(rq));

    // Order matters: entry_seq BEFORE the facts, so work that slips in between the two
    // is caught by the barrier recheck instead of being missed.
    env->lock(env->ctx);
    const uint32_t seq = bb_sb_entry_seq(c);
    env->unlock(env->ctx);

    bb_sb_facts_t f;
    memset(&f, 0, sizeof(f));
    env->sample(env->ctx, &f);

    bb_sb_fx_t fx;
    env->lock(env->ctx);
    bb_sb_handle(c, &rq, env->now_ms(env->ctx), seq, &f, rp, &fx);
    env->unlock(env->ctx);

    if (fx.run_stage != 0u) {
        const bool ok = bb_sb_run_stage(fx.run_stage, ops);   // never under the lock
        env->lock(env->ctx);
        bb_sb_step_done(c, fx.run_stage, ok);
        bb_sb_final_reply(c, fx.run_stage, bb_sb_inhibitors(c, &f, env->now_ms(env->ctx)), rp);
        env->unlock(env->ctx);
    }
}
