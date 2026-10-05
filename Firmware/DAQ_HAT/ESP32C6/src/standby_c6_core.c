#include "standby_c6_core.h"

#include <string.h>

static int32_t gen_diff(uint32_t a, uint32_t b) { return (int32_t)(a - b); }

static bool timeout_valid(uint16_t s)
{
    return s == 0u || s == 60u || s == 300u || s == 900u;
}

static const struct { uint16_t bit; const char *label; } BOOT_ITEMS[SB_C6_BOOT_ITEMS] = {
    { BB_ST_BOOT_SETTINGS, "Settings" },
    { BB_ST_BOOT_DISPLAY,  "Display" },
    { BB_ST_BOOT_P4,       "Analyzer" },
    { BB_ST_BOOT_IO,       "Mainboard IO" },
    { BB_ST_BOOT_WIFI,     "Wi-Fi" },
    { BB_ST_BOOT_C6_WIFI,  "C6 radio" },
};

static const char *const WAKE_LABELS[SB_C6_WAKE_ITEMS] = {
    "Safe state", "Analog power", "Reinitialise", "Analyzer", "Indicators",
};

void sb_c6_init(sb_c6_t *c, uint32_t now_ms)
{
    memset(c, 0, sizeof(*c));
    c->state = BB_ST_ACTIVE;
    c->boot_active = true;
    c->boot_start_ms = now_ms;
}

// ---------------------------------------------------------------------------
// Boot milestones
// ---------------------------------------------------------------------------
void sb_c6_local(sb_c6_t *c, uint16_t bit, sb_item_t result)
{
    bit &= SB_C6_BOOT_ALL;
    c->local_done &= (uint16_t)~bit;
    c->local_fail &= (uint16_t)~bit;
    c->local_skip &= (uint16_t)~bit;
    if (result == SB_ITEM_OK) c->local_done |= bit;
    else if (result == SB_ITEM_FAILED) c->local_fail |= bit;
    else if (result == SB_ITEM_SKIPPED) c->local_skip |= bit;
}

void sb_c6_p4_alive(sb_c6_t *c, uint32_t now_ms)
{
    if (!c->p4_live) {
        c->p4_live = true;
        c->p4_live_ms = now_ms;
    }
}

static sb_item_t boot_status(const sb_c6_t *c, uint16_t bit)
{
    if ((c->remote_fail | c->local_fail | c->p4_fail) & bit) return SB_ITEM_FAILED;
    if ((c->remote_done | c->local_done | c->p4_done) & bit) return SB_ITEM_OK;
    if ((c->remote_skip | c->local_skip) & bit) return SB_ITEM_SKIPPED;
    if (c->timed & bit) return SB_ITEM_TIMEOUT;
    return SB_ITEM_PENDING;
}

static uint16_t boot_resolved(const sb_c6_t *c)
{
    uint16_t r = 0;
    for (unsigned i = 0; i < SB_C6_BOOT_ITEMS; ++i) {
        if (boot_status(c, BOOT_ITEMS[i].bit) != SB_ITEM_PENDING) r |= BOOT_ITEMS[i].bit;
    }
    return r;
}

static void merge_p4_boot(sb_c6_t *c, const bb_standby_request_t *rq)
{
    // The Analyzer milestone is the P4's own bring-up result. A frame without the bit
    // means "not finished yet" and must not undo an earlier report.
    if ((rq->completed | rq->failed) & BB_ST_BOOT_P4) {
        c->p4_done = (uint16_t)(rq->completed & BB_ST_BOOT_P4);
        c->p4_fail = (uint16_t)(rq->failed & BB_ST_BOOT_P4);
    }
}

static void merge_remote_boot(sb_c6_t *c, const bb_standby_request_t *rq)
{
    // The S3 is authoritative for its own bits: each report REPLACES them. Bits it
    // has no business deciding (display, settings, C6 radio) are ignored, and the
    // Analyzer bit only counts when it comes from the P4 itself.
    c->remote_done = (uint16_t)(rq->completed & SB_C6_BOOT_S3);
    c->remote_fail = (uint16_t)(rq->failed & SB_C6_BOOT_S3);
    c->remote_skip = (uint16_t)(rq->skipped & SB_C6_BOOT_S3);
    merge_p4_boot(c, rq);
    c->s3_boot_seen = true;
}

void sb_c6_tick(sb_c6_t *c, uint32_t now_ms)
{
    c->tick_ms = now_ms;
    if (c->boot_active) {
        // An S3 without standby support never reports. The analyzer is up, so say so
        // plainly and let the instrument through instead of stalling the splash.
        if (c->p4_live && !c->s3_boot_seen &&
            (uint32_t)(now_ms - c->p4_live_ms) >= SB_C6_LEGACY_S3_MS) {
            c->timed |= (uint16_t)(SB_C6_BOOT_REMOTE & ~boot_resolved(c));
        }
        if ((uint32_t)(now_ms - c->boot_start_ms) >= SB_C6_BOOT_TIMEOUT_MS) {
            c->timed |= (uint16_t)(SB_C6_BOOT_ALL & ~boot_resolved(c));
        }
        const uint16_t resolved = boot_resolved(c) | c->timed;
        if ((resolved & SB_C6_BOOT_ALL) == SB_C6_BOOT_ALL) {
            if (c->boot_resolved_ms == 0u) c->boot_resolved_ms = now_ms ? now_ms : 1u;
            const bool issues = (c->timed != 0u) || ((c->remote_fail | c->local_fail |
                                                      c->remote_skip | c->local_skip) & SB_C6_BOOT_ALL);
            const uint32_t hold = issues ? SB_C6_HOLD_ISSUES_MS : SB_C6_HOLD_CLEAN_MS;
            if ((uint32_t)(now_ms - c->boot_resolved_ms) >= hold) c->boot_active = false;
        }
    }
    if (c->policy_pending && (uint32_t)(now_ms - c->policy_pending_ms) >= SB_C6_POLICY_PENDING_MS) {
        c->policy_pending = false;
        c->policy_failed = true;
    }
}

// ---------------------------------------------------------------------------
// P4 request
// ---------------------------------------------------------------------------
static void fill_reply(const sb_c6_t *c, bb_standby_reply_t *rp, bool ready, uint8_t failure)
{
    rp->schema = BB_STANDBY_SCHEMA;
    rp->state = c->state;
    rp->ready = ready ? 1u : 0u;
    rp->failure = failure;
    rp->generation = c->gen_valid ? c->generation : 0u;
    rp->inhibitors = c->inhibitors;
    rp->activity = c->activity;
}

static void mirror_timeout(sb_c6_t *c, uint16_t seconds)
{
    if (!timeout_valid(seconds)) return;           // 0xFFFF = "the S3 has not told the P4 yet"
    c->timeout_s = seconds;
    c->timeout_known = true;
    if (c->policy_pending && c->policy_pending_s == seconds) {
        c->policy_pending = false;
        c->policy_failed = false;
    }
}

static void reset_wake(sb_c6_t *c)
{
    c->wake_done = c->wake_fail = c->wake_skip = 0;
    c->wake_hold_valid = false;
}

// A new intent starts a fresh attempt: a failure retained for the OTHER direction is
// history, not a verdict on this one.
static void set_want_dark(sb_c6_t *c, bool dark)
{
    if (c->want_dark != dark) c->display_fail = false;
    c->want_dark = dark;
}

static void merge_wake_masks(sb_c6_t *c, const bb_standby_request_t *rq)
{
    c->wake_done |= (uint16_t)(rq->completed & SB_C6_WAKE_MASK);
    c->wake_fail |= (uint16_t)(rq->failed & SB_C6_WAKE_MASK);
    c->wake_skip |= (uint16_t)(rq->skipped & SB_C6_WAKE_MASK);
}

void sb_c6_handle(sb_c6_t *c, const bb_standby_request_t *rq, uint32_t now_ms,
                  bb_standby_reply_t *rp)
{
    memset(rp, 0, sizeof(*rp));
    if (rq->schema != BB_STANDBY_SCHEMA || rq->op > BB_ST_OP_PROGRESS) {
        fill_reply(c, rp, false, SB_C6_FAIL_ARG);
        return;
    }

    if (rq->op == BB_ST_OP_POLL) {
        mirror_timeout(c, rq->timeout_seconds);
        if (rq->stage == BB_ST_BOOT_STAGE && c->boot_active) merge_remote_boot(c, rq);
        else if (rq->stage == SB_C6_STAGE_BOOT_P4_ONLY && c->boot_active) merge_p4_boot(c, rq);
        fill_reply(c, rp, c->state == BB_ST_ACTIVE || (c->state == BB_ST_ASLEEP && c->display_dark),
                   SB_C6_FAIL_NONE);
        return;
    }

    if (rq->op == BB_ST_OP_PROGRESS) {
        if (rq->stage == BB_ST_BOOT_STAGE) {
            // An S3 boot starts a new epoch: rebase, never lock a rebooted S3 out.
            c->generation = rq->generation;
            c->gen_valid = true;
            mirror_timeout(c, rq->timeout_seconds);
            if (c->boot_active) merge_remote_boot(c, rq);
            fill_reply(c, rp, true, SB_C6_FAIL_NONE);
            return;
        }
        if (rq->stage == 0u && rq->state == BB_ST_WAKING) {
            // A button woke the P4 before the mainboard asked for anything. Show the
            // loading screen with an empty bar and say what we are waiting for.
            if (!c->wake_loading) reset_wake(c);
            c->wake_loading = true;
            c->wake_notice_only = true;
            set_want_dark(c, false);
            if (c->state != BB_ST_ACTIVE || c->display_dark) c->state = BB_ST_WAKING;
            fill_reply(c, rp, !c->display_dark, SB_C6_FAIL_NONE);
            return;
        }
        if (c->gen_valid && gen_diff(rq->generation, c->generation) < 0) {
            fill_reply(c, rp, false, SB_C6_FAIL_STALE);
            return;
        }
        mirror_timeout(c, rq->timeout_seconds);
        if (c->wake_loading) merge_wake_masks(c, rq);
        fill_reply(c, rp, true, SB_C6_FAIL_NONE);
        return;
    }

    // SLEEP / WAKE
    const uint8_t stage = rq->stage;
    const bool sleep_op = rq->op == BB_ST_OP_SLEEP;
    if (sleep_op ? stage != SB_C6_STAGE_INDICATORS_OFF
                 : (stage < SB_C6_STAGE_WAKE_SAFE || stage > SB_C6_STAGE_INDICATORS_ON)) {
        fill_reply(c, rp, false, SB_C6_FAIL_ARG);
        return;
    }
    if (c->gen_valid && gen_diff(rq->generation, c->generation) < 0) {
        fill_reply(c, rp, false, SB_C6_FAIL_STALE);
        return;
    }
    c->generation = rq->generation;
    c->gen_valid = true;
    mirror_timeout(c, rq->timeout_seconds);

    bool ready = false;
    uint8_t fail = SB_C6_FAIL_NONE;
    switch (stage) {
    case SB_C6_STAGE_INDICATORS_OFF:
        set_want_dark(c, true);
        c->indicators_off = true;                  // LEDs go dark at once; the panel follows
        c->wake_loading = false;
        c->wake_notice_only = false;
        if (c->display_dark) c->state = BB_ST_ASLEEP;
        else if (c->state != BB_ST_ASLEEP) c->state = BB_ST_PREPARING;
        ready = c->display_dark;
        // The driver said the panel sequence failed: that is retained and reported, never
        // turned into a success. The P4 fails the stage; the S3 retries it.
        if (!ready && c->display_fail) fail = SB_C6_FAIL_HW;
        break;

    case SB_C6_STAGE_WAKE_SAFE:
        if (!c->wake_gen_valid || c->wake_gen != rq->generation || !c->wake_loading) {
            reset_wake(c);
            c->wake_gen = rq->generation;
            c->wake_gen_valid = true;
        }
        c->wake_loading = true;
        c->wake_notice_only = false;
        set_want_dark(c, false);
        c->state = BB_ST_WAKING;
        merge_wake_masks(c, rq);
        ready = !c->display_dark;
        if (!ready && c->display_fail) fail = SB_C6_FAIL_HW;
        break;

    case SB_C6_STAGE_ANALOG_ON:
    case SB_C6_STAGE_REINIT:
    case SB_C6_STAGE_HAT_WAKE:
        merge_wake_masks(c, rq);
        ready = true;                              // progress only: nothing to do here
        break;

    default:                                       // INDICATORS_ON
        merge_wake_masks(c, rq);
        c->wake_done |= (uint16_t)(1u << SB_C6_STAGE_INDICATORS_ON);
        c->indicators_off = false;
        set_want_dark(c, false);
        if (!c->display_dark) {
            c->state = BB_ST_ACTIVE;
            c->wake_loading = false;
            c->wake_notice_only = false;
            c->wake_hold_until = now_ms + SB_C6_WAKE_HOLD_MS;
            c->wake_hold_valid = true;
            ready = true;
        } else {
            c->state = BB_ST_WAKING;               // the panel is still being switched on
            if (c->display_fail) fail = SB_C6_FAIL_HW;
        }
        break;
    }
    fill_reply(c, rp, ready, fail);
}

// ---------------------------------------------------------------------------
// Activity, inhibitors, hardware intent
// ---------------------------------------------------------------------------
void sb_c6_activity(sb_c6_t *c) { c->activity++; }

void sb_c6_set_inhibitors(sb_c6_t *c, uint32_t inhibitors) { c->inhibitors = inhibitors; }

sb_disp_action_t sb_c6_display_action(const sb_c6_t *c)
{
    if (c->display_fail && (int32_t)(c->tick_ms - c->display_retry_at) < 0) return SB_DISP_NONE;   // back off
    if (c->want_dark && !c->display_dark) return SB_DISP_SLEEP;
    if (!c->want_dark && c->display_dark) return SB_DISP_WAKE;
    return SB_DISP_NONE;
}

void sb_c6_display_failed(sb_c6_t *c, uint32_t now_ms)
{
    c->display_fail = true;
    c->display_fail_count++;
    c->display_retry_at = now_ms + SB_C6_DISPLAY_RETRY_MS;
}

void sb_c6_display_done(sb_c6_t *c, bool now_dark, uint32_t now_ms)
{
    c->display_fail = false;
    c->display_dark = now_dark;
    if (now_dark) {
        c->state = BB_ST_ASLEEP;
        return;
    }
    // Panel is lit again. If the indicators-on request already arrived while the
    // panel was still coming up, finish the wake now.
    if (c->state == BB_ST_WAKING && !c->indicators_off && !c->wake_notice_only &&
        (c->wake_done & (1u << SB_C6_STAGE_INDICATORS_ON))) {
        c->state = BB_ST_ACTIVE;
        c->wake_loading = false;
        c->wake_hold_until = now_ms + SB_C6_WAKE_HOLD_MS;
        c->wake_hold_valid = true;
    }
}

bool sb_c6_indicators_off(const sb_c6_t *c) { return c->indicators_off; }

sb_ui_mode_t sb_c6_ui_mode(const sb_c6_t *c, uint32_t now_ms)
{
    if (c->display_dark) return SB_UI_DARK;
    if (c->boot_active) return SB_UI_BOOT;
    if (c->wake_loading) return SB_UI_WAKE;
    if (c->wake_hold_valid && gen_diff(c->wake_hold_until, now_ms) > 0) return SB_UI_WAKE;
    return SB_UI_NORMAL;
}

bool sb_c6_input_blocked(const sb_c6_t *c, uint32_t now_ms)
{
    return sb_c6_ui_mode(c, now_ms) != SB_UI_NORMAL || c->state != BB_ST_ACTIVE;
}

// ---------------------------------------------------------------------------
// View
// ---------------------------------------------------------------------------
const char *sb_c6_item_text(sb_item_t s)
{
    switch (s) {
    case SB_ITEM_OK:      return "ok";
    case SB_ITEM_FAILED:  return "failed";
    case SB_ITEM_SKIPPED: return "skipped";
    case SB_ITEM_TIMEOUT: return "no report";
    default:              return "waiting";
    }
}

void sb_c6_view(const sb_c6_t *c, uint32_t now_ms, sb_c6_view_t *v)
{
    memset(v, 0, sizeof(*v));
    v->mode = sb_c6_ui_mode(c, now_ms);

    if (v->mode == SB_UI_BOOT) {
        v->n = SB_C6_BOOT_ITEMS;
        for (unsigned i = 0; i < SB_C6_BOOT_ITEMS; ++i) {
            v->status[i] = (uint8_t)boot_status(c, BOOT_ITEMS[i].bit);
            v->label[i] = BOOT_ITEMS[i].label;
        }
        v->s3_silent = !c->s3_boot_seen && ((c->timed & SB_C6_BOOT_REMOTE) != 0u);
    } else if (v->mode == SB_UI_WAKE) {
        v->n = SB_C6_WAKE_ITEMS;
        for (unsigned i = 0; i < SB_C6_WAKE_ITEMS; ++i) {
            const uint16_t bit = (uint16_t)(1u << (SB_C6_STAGE_WAKE_SAFE + i));
            sb_item_t s = SB_ITEM_PENDING;
            if (c->wake_fail & bit) s = SB_ITEM_FAILED;
            else if (c->wake_done & bit) s = SB_ITEM_OK;
            else if (c->wake_skip & bit) s = SB_ITEM_SKIPPED;
            v->status[i] = (uint8_t)s;
            v->label[i] = WAKE_LABELS[i];
        }
        v->waiting_mainboard = c->wake_notice_only;
    } else {
        return;
    }

    v->current = v->n;
    v->finished = true;
    for (unsigned i = 0; i < v->n; ++i) {
        if (v->status[i] == SB_ITEM_OK) v->ok_count++;
        if (v->status[i] == SB_ITEM_PENDING) {
            v->finished = false;
            if (v->current == v->n) v->current = (uint8_t)i;
        } else if (v->status[i] != SB_ITEM_OK) {
            v->issues = true;
        }
    }
}

// ---------------------------------------------------------------------------
// Policy mirror
// ---------------------------------------------------------------------------
uint16_t sb_c6_policy_seconds_for_index(int idx)
{
    switch (idx) {
    case 0: return 60u;
    case 1: return 300u;
    case 2: return 900u;
    default: return 0u;                            // 3 = Off
    }
}

sb_policy_kind_t sb_c6_policy(const sb_c6_t *c, uint32_t now_ms, int *idx)
{
    (void)now_ms;
    if (c->policy_pending) return SB_POLICY_PENDING;
    if (!c->timeout_known) return SB_POLICY_UNKNOWN;
    if (idx) *idx = c->timeout_s == 60u ? 0 : c->timeout_s == 300u ? 1 : c->timeout_s == 900u ? 2 : 3;
    return SB_POLICY_VALUE;
}

uint16_t sb_c6_policy_choose(sb_c6_t *c, int idx, uint32_t now_ms)
{
    const uint16_t seconds = sb_c6_policy_seconds_for_index(idx);
    c->policy_pending = true;
    c->policy_pending_s = seconds;
    c->policy_pending_ms = now_ms;
    c->policy_failed = false;
    c->activity++;
    return seconds;
}

void sb_c6_policy_response(sb_c6_t *c, bool ok, uint16_t seconds)
{
    c->policy_pending = false;
    if (ok && timeout_valid(seconds)) {
        c->timeout_s = seconds;
        c->timeout_known = true;
        c->policy_failed = false;
    } else {
        c->policy_failed = true;
    }
}
