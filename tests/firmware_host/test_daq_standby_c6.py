"""Host execution of the DAQ C6 standby model, loading view and panel power order."""

from tests.firmware_host.fwhost import compile_and_run

C6 = "Firmware/DAQ_HAT/ESP32C6/src"
COMMON = "Firmware/DAQ_HAT/common"

MAIN = r"""
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "standby_c6_core.h"
#include "display_power.h"

static bb_standby_request_t rq(uint8_t op, uint8_t stage, uint32_t gen) {
    bb_standby_request_t r;
    memset(&r, 0, sizeof(r));
    r.schema = BB_STANDBY_SCHEMA; r.op = op; r.stage = stage; r.generation = gen;
    r.timeout_seconds = 0xFFFF;
    return r;
}
static bb_standby_reply_t call(sb_c6_t *c, bb_standby_request_t r, uint32_t now) {
    bb_standby_reply_t rp;
    sb_c6_handle(c, &r, now, &rp);
    return rp;
}
static bb_standby_request_t boot_rq(uint32_t gen, uint16_t done, uint16_t fail, uint16_t skip) {
    bb_standby_request_t r = rq(BB_ST_OP_PROGRESS, BB_ST_BOOT_STAGE, gen);
    r.completed = done; r.failed = fail; r.skipped = skip;
    return r;
}
static sb_c6_view_t view(sb_c6_t *c, uint32_t now) { sb_c6_view_t v; sb_c6_view(c, now, &v); return v; }
// What the P4 sends once its own bring-up is over (a frame alone proves only the link).
static void p4_boot(sb_c6_t *c, uint32_t now, bool ok) {
    sb_c6_p4_alive(c, now);
    bb_standby_request_t r = rq(BB_ST_OP_POLL, SB_C6_STAGE_BOOT_P4_ONLY, 0);
    if (ok) r.completed = BB_ST_BOOT_P4; else r.failed = BB_ST_BOOT_P4;
    call(c, r, now);
}

// ---- recording display ops --------------------------------------------------
static char trace[256];
static bool f_wait, f_bl_off, f_panel_off, f_panel_on, f_flush, f_bl_on;       // injected driver failures
static void rec(const char *s) { strcat(trace, s); strcat(trace, ";"); }
static void o_asleep(void *x, bool a) { (void)x; rec(a ? "asleep" : "awake"); }
static bool o_wait(void *x) { (void)x; rec("wait"); return !f_wait; }
static bool o_bl_off(void *x) { (void)x; rec("bl_off"); return !f_bl_off; }
static bool o_panel(void *x, bool on) { (void)x; rec(on ? "panel_on" : "panel_off"); return on ? !f_panel_on : !f_panel_off; }
static void o_logo(void *x) { (void)x; rec("logo"); }
static bool o_flush(void *x) { (void)x; rec("flush"); return !f_flush; }
static bool o_bl_on(void *x) { (void)x; rec("bl_restore"); return !f_bl_on; }

int main(void) {
    sb_c6_t c;
    bb_standby_reply_t r;

    // ---- clean boot: every milestone is a real event, then it dismisses -----
    sb_c6_init(&c, 1000);
    assert(sb_c6_ui_mode(&c, 1000) == SB_UI_BOOT);
    sb_c6_view_t v = view(&c, 1000);
    assert(v.n == 6 && v.ok_count == 0 && v.current == 0 && !v.finished);
    sb_c6_local(&c, BB_ST_BOOT_SETTINGS, SB_ITEM_OK);
    sb_c6_local(&c, BB_ST_BOOT_DISPLAY, SB_ITEM_OK);
    sb_c6_local(&c, BB_ST_BOOT_C6_WIFI, SB_ITEM_SKIPPED);       // radio only starts on demand
    p4_boot(&c, 1200, true);
    v = view(&c, 1300);
    assert(v.ok_count == 3 && v.issues && !v.finished && v.current == 3);   // current = Mainboard IO
    assert(!strcmp(v.label[v.current], "Mainboard IO"));
    r = call(&c, boot_rq(7, BB_ST_BOOT_IO, 0, BB_ST_BOOT_WIFI), 1400);
    assert(r.ready && r.generation == 7);                      // boot report rebases the generation
    sb_c6_tick(&c, 1400);
    v = view(&c, 1400);
    assert(v.finished && v.ok_count == 4);                      // Wi-Fi skipped is resolved, not stranding boot
    assert(sb_c6_ui_mode(&c, 1500) == SB_UI_BOOT);              // degraded result stays long enough to read
    sb_c6_tick(&c, 1400 + SB_C6_HOLD_ISSUES_MS - 1);
    assert(sb_c6_ui_mode(&c, 1400 + SB_C6_HOLD_ISSUES_MS - 1) == SB_UI_BOOT);
    sb_c6_tick(&c, 1400 + SB_C6_HOLD_ISSUES_MS);
    assert(sb_c6_ui_mode(&c, 1400 + SB_C6_HOLD_ISSUES_MS) == SB_UI_NORMAL);
    puts("boot-clean/skipped-wifi");

    // ---- an all-ok boot dismisses quickly; failures are shown truthfully -----
    sb_c6_init(&c, 0);
    sb_c6_local(&c, BB_ST_BOOT_SETTINGS, SB_ITEM_OK);
    sb_c6_local(&c, BB_ST_BOOT_DISPLAY, SB_ITEM_OK);
    sb_c6_local(&c, BB_ST_BOOT_C6_WIFI, SB_ITEM_OK);
    p4_boot(&c, 10, true);
    call(&c, boot_rq(1, BB_ST_BOOT_IO | BB_ST_BOOT_WIFI, 0, 0), 20);
    sb_c6_tick(&c, 20);
    v = view(&c, 20);
    assert(v.finished && !v.issues && v.ok_count == 6);
    sb_c6_tick(&c, 20 + SB_C6_HOLD_CLEAN_MS);
    assert(sb_c6_ui_mode(&c, 20 + SB_C6_HOLD_CLEAN_MS) == SB_UI_NORMAL);

    sb_c6_init(&c, 0);
    sb_c6_local(&c, BB_ST_BOOT_SETTINGS, SB_ITEM_OK);
    sb_c6_local(&c, BB_ST_BOOT_DISPLAY, SB_ITEM_OK);
    sb_c6_local(&c, BB_ST_BOOT_C6_WIFI, SB_ITEM_SKIPPED);
    p4_boot(&c, 10, true);
    call(&c, boot_rq(1, BB_ST_BOOT_IO, BB_ST_BOOT_WIFI, 0), 20);        // Wi-Fi failed
    v = view(&c, 20);
    assert(v.status[4] == SB_ITEM_FAILED && v.issues && v.finished);
    // remote reports REPLACE (a retry that succeeds clears the failure) and cannot set local bits
    call(&c, boot_rq(1, BB_ST_BOOT_IO | BB_ST_BOOT_WIFI | BB_ST_BOOT_C6_WIFI | BB_ST_BOOT_DISPLAY, 0, 0), 30);
    v = view(&c, 30);
    assert(v.status[4] == SB_ITEM_OK && v.status[5] == SB_ITEM_SKIPPED);
    puts("boot-failure/replace");

    // ---- deadlines: an S3 that never reports, and no P4 at all --------------
    sb_c6_init(&c, 0);
    sb_c6_local(&c, BB_ST_BOOT_SETTINGS, SB_ITEM_OK);
    sb_c6_local(&c, BB_ST_BOOT_DISPLAY, SB_ITEM_OK);
    sb_c6_local(&c, BB_ST_BOOT_C6_WIFI, SB_ITEM_SKIPPED);
    p4_boot(&c, 100, true);
    sb_c6_tick(&c, 100 + SB_C6_LEGACY_S3_MS - 1);
    assert(!view(&c, 100 + SB_C6_LEGACY_S3_MS - 1).finished);
    sb_c6_tick(&c, 100 + SB_C6_LEGACY_S3_MS);
    v = view(&c, 100 + SB_C6_LEGACY_S3_MS);
    assert(v.finished && v.s3_silent && v.status[3] == SB_ITEM_TIMEOUT && v.status[4] == SB_ITEM_TIMEOUT);
    assert(v.status[2] == SB_ITEM_OK);                         // the analyzer link is real
    call(&c, boot_rq(3, BB_ST_BOOT_IO, 0, 0), 7000);           // a late report still wins over "no report"
    assert(view(&c, 7000).status[3] == SB_ITEM_OK);

    sb_c6_init(&c, 0);                                         // nothing ever answers
    sb_c6_local(&c, BB_ST_BOOT_SETTINGS, SB_ITEM_OK);
    sb_c6_tick(&c, SB_C6_BOOT_TIMEOUT_MS - 1);
    assert(!view(&c, SB_C6_BOOT_TIMEOUT_MS - 1).finished);
    sb_c6_tick(&c, SB_C6_BOOT_TIMEOUT_MS);
    v = view(&c, SB_C6_BOOT_TIMEOUT_MS);
    assert(v.finished && v.status[0] == SB_ITEM_OK && v.status[1] == SB_ITEM_TIMEOUT
           && v.status[2] == SB_ITEM_TIMEOUT);
    sb_c6_tick(&c, SB_C6_BOOT_TIMEOUT_MS + SB_C6_HOLD_ISSUES_MS);
    assert(sb_c6_ui_mode(&c, SB_C6_BOOT_TIMEOUT_MS + SB_C6_HOLD_ISSUES_MS) == SB_UI_NORMAL);
    puts("boot-deadlines");

    // ---- the Analyzer milestone is the P4's own report, never inferred -----------
    sb_c6_init(&c, 0);
    sb_c6_local(&c, BB_ST_BOOT_SETTINGS, SB_ITEM_OK);
    sb_c6_local(&c, BB_ST_BOOT_DISPLAY, SB_ITEM_OK);
    sb_c6_local(&c, BB_ST_BOOT_C6_WIFI, SB_ITEM_SKIPPED);
    sb_c6_p4_alive(&c, 10);                                    // frames flow, but the P4 has said nothing
    assert(view(&c, 10).status[2] == SB_ITEM_PENDING && !view(&c, 10).finished);
    call(&c, boot_rq(1, BB_ST_BOOT_IO | BB_ST_BOOT_WIFI, 0, 0), 20);   // the S3 is up; the P4 bit is not set
    assert(view(&c, 20).status[2] == SB_ITEM_PENDING && !view(&c, 20).finished);
    p4_boot(&c, 30, false);                                    // bring-up failed
    v = view(&c, 30);
    assert(v.status[2] == SB_ITEM_FAILED && v.issues && v.finished && v.ok_count == 4);
    p4_boot(&c, 40, true);                                     // a report replaces the earlier one
    assert(view(&c, 40).status[2] == SB_ITEM_OK);
    call(&c, boot_rq(1, BB_ST_BOOT_IO | BB_ST_BOOT_WIFI, 0, 0), 50);   // a frame without the bit does not undo it
    assert(view(&c, 50).status[2] == SB_ITEM_OK);
    // the P4-only frame never counts as the S3 having reported
    sb_c6_init(&c, 0);
    p4_boot(&c, 5, true);
    assert(!c.s3_boot_seen && c.p4_live);
    // a P4 that never reports anything times out honestly
    sb_c6_init(&c, 0);
    sb_c6_local(&c, BB_ST_BOOT_SETTINGS, SB_ITEM_OK);
    sb_c6_local(&c, BB_ST_BOOT_DISPLAY, SB_ITEM_OK);
    sb_c6_local(&c, BB_ST_BOOT_C6_WIFI, SB_ITEM_SKIPPED);
    sb_c6_p4_alive(&c, 0);
    call(&c, boot_rq(1, BB_ST_BOOT_IO | BB_ST_BOOT_WIFI, 0, 0), 20);
    sb_c6_tick(&c, SB_C6_BOOT_TIMEOUT_MS);
    v = view(&c, SB_C6_BOOT_TIMEOUT_MS);
    assert(v.finished && v.status[2] == SB_ITEM_TIMEOUT && v.issues);
    puts("boot-p4-truth");

    // POLL carries the boot masks without rebasing the generation
    sb_c6_init(&c, 0);
    bb_standby_request_t p = boot_rq(55, BB_ST_BOOT_IO, 0, 0); p.op = BB_ST_OP_POLL;
    r = call(&c, p, 5);
    assert(r.generation == 0 && view(&c, 5).status[3] == SB_ITEM_OK);

    // ---- sleep: LEDs at once, ready only once the panel is really dark -------
    sb_c6_init(&c, 0);
    c.boot_active = false;
    r = call(&c, rq(BB_ST_OP_SLEEP, 6, 10), 100);
    assert(!r.ready && r.state == BB_ST_PREPARING && r.generation == 10);
    assert(sb_c6_indicators_off(&c) && sb_c6_display_action(&c) == SB_DISP_SLEEP);
    assert(!call(&c, rq(BB_ST_OP_SLEEP, 6, 10), 101).ready);   // still not dark: still not ready
    sb_c6_display_done(&c, true, 110);
    r = call(&c, rq(BB_ST_OP_SLEEP, 6, 10), 111);
    assert(r.ready && r.state == BB_ST_ASLEEP);
    assert(sb_c6_display_action(&c) == SB_DISP_NONE && sb_c6_ui_mode(&c, 111) == SB_UI_DARK);
    assert(sb_c6_input_blocked(&c, 111));
    assert(call(&c, rq(BB_ST_OP_SLEEP, 6, 10), 112).ready);   // idempotent
    r = call(&c, rq(BB_ST_OP_SLEEP, 6, 9), 113);                // older generation
    assert(!r.ready && r.failure == SB_C6_FAIL_STALE && r.generation == 10 && sb_c6_indicators_off(&c));
    r = call(&c, rq(BB_ST_OP_SLEEP, 3, 10), 113);               // the C6 only handles stage 6
    assert(r.failure == SB_C6_FAIL_ARG);
    puts("sleep");

    // ---- wake: panel first, progress from real stages, LEDs only at the end --
    r = call(&c, rq(BB_ST_OP_WAKE, 7, 11), 200);
    assert(!r.ready && r.state == BB_ST_WAKING && sb_c6_display_action(&c) == SB_DISP_WAKE);
    assert(sb_c6_indicators_off(&c));                           // LEDs stay off while waking
    sb_c6_display_done(&c, false, 210);
    assert(call(&c, rq(BB_ST_OP_WAKE, 7, 11), 211).ready);
    assert(sb_c6_ui_mode(&c, 211) == SB_UI_WAKE && sb_c6_input_blocked(&c, 211));
    v = view(&c, 211);
    assert(v.n == 5 && v.ok_count == 0 && !v.waiting_mainboard);
    bb_standby_request_t w8 = rq(BB_ST_OP_WAKE, 8, 11); w8.completed = (1u << 7) | (1u << 8);
    assert(call(&c, w8, 220).ready);
    v = view(&c, 220);
    assert(v.ok_count == 2 && v.current == 2 && v.status[0] == SB_ITEM_OK && v.status[1] == SB_ITEM_OK);
    bb_standby_request_t w10 = rq(BB_ST_OP_WAKE, 10, 11);
    w10.completed = (1u << 9); w10.failed = (1u << 10);
    call(&c, w10, 230);
    v = view(&c, 230);
    assert(v.status[2] == SB_ITEM_OK && v.status[3] == SB_ITEM_FAILED && v.issues);
    assert(v.status[4] == SB_ITEM_PENDING);                     // never marked done by a timer
    r = call(&c, rq(BB_ST_OP_WAKE, 11, 11), 240);
    assert(r.ready && r.state == BB_ST_ACTIVE && !sb_c6_indicators_off(&c));
    assert(sb_c6_ui_mode(&c, 240) == SB_UI_WAKE);               // brief hold on the result
    assert(sb_c6_ui_mode(&c, 240 + SB_C6_WAKE_HOLD_MS) == SB_UI_NORMAL);
    assert(!sb_c6_input_blocked(&c, 240 + SB_C6_WAKE_HOLD_MS));
    puts("wake");

    // stage 11 can arrive before the panel is back: not ready until it really is
    sb_c6_init(&c, 0); c.boot_active = false;
    call(&c, rq(BB_ST_OP_SLEEP, 6, 20), 0); sb_c6_display_done(&c, true, 1);
    r = call(&c, rq(BB_ST_OP_WAKE, 11, 21), 50);
    assert(!r.ready && r.state == BB_ST_WAKING && !sb_c6_indicators_off(&c));
    sb_c6_display_done(&c, false, 60);
    r = call(&c, rq(BB_ST_OP_WAKE, 11, 21), 61);
    assert(r.ready && r.state == BB_ST_ACTIVE);

    // ---- a button wake before the mainboard asks for anything -----------------
    sb_c6_init(&c, 0); c.boot_active = false;
    call(&c, rq(BB_ST_OP_SLEEP, 6, 30), 0); sb_c6_display_done(&c, true, 1);
    bb_standby_request_t note = rq(BB_ST_OP_PROGRESS, 0, 30); note.state = BB_ST_WAKING;
    r = call(&c, note, 100);
    assert(!r.ready && sb_c6_display_action(&c) == SB_DISP_WAKE);
    sb_c6_display_done(&c, false, 110);
    v = view(&c, 111);
    assert(v.mode == SB_UI_WAKE && v.waiting_mainboard && v.ok_count == 0 && !v.finished);
    call(&c, rq(BB_ST_OP_WAKE, 7, 31), 500);                    // the mainboard took over
    assert(!view(&c, 500).waiting_mainboard);
    puts("button-wake");

    // ---- activity is a monotonic counter; inhibitors pass through -------------
    sb_c6_init(&c, 0);
    uint32_t a0 = c.activity;
    sb_c6_activity(&c); sb_c6_activity(&c);
    sb_c6_set_inhibitors(&c, BB_ST_INH_OTA);
    r = call(&c, rq(BB_ST_OP_POLL, 0, 0), 1);
    assert(r.activity == a0 + 2 && (r.inhibitors & BB_ST_INH_OTA));
    { bb_standby_request_t bad = rq(BB_ST_OP_POLL, 0, 0); bad.schema = 2;
      r = call(&c, bad, 1); assert(r.failure == SB_C6_FAIL_ARG); }
    puts("activity");

    // ---- policy mirror: authoritative on the S3, never persisted here ---------
    sb_c6_init(&c, 0);
    int idx = -9;
    assert(sb_c6_policy(&c, 0, &idx) == SB_POLICY_UNKNOWN);
    bb_standby_request_t pol = rq(BB_ST_OP_POLL, 0, 0); pol.timeout_seconds = 900;
    call(&c, pol, 0);
    assert(sb_c6_policy(&c, 0, &idx) == SB_POLICY_VALUE && idx == 2);
    pol.timeout_seconds = 0; call(&c, pol, 0);
    assert(sb_c6_policy(&c, 0, &idx) == SB_POLICY_VALUE && idx == 3);   // Off
    pol.timeout_seconds = 77; call(&c, pol, 0);                         // junk is ignored
    assert(sb_c6_policy(&c, 0, &idx) == SB_POLICY_VALUE && idx == 3);
    uint32_t act = c.activity;
    assert(sb_c6_policy_choose(&c, 0, 1000) == 60 && c.activity == act + 1);
    assert(sb_c6_policy(&c, 1000, &idx) == SB_POLICY_PENDING);
    pol.timeout_seconds = 300; call(&c, pol, 1100);                     // a different value does not confirm it
    assert(c.policy_pending);
    sb_c6_policy_choose(&c, 2, 2000);
    pol.timeout_seconds = 900; call(&c, pol, 2100);                     // the mirror confirms it
    assert(!c.policy_pending && sb_c6_policy(&c, 2100, &idx) == SB_POLICY_VALUE && idx == 2);
    sb_c6_policy_choose(&c, 1, 3000);
    sb_c6_policy_response(&c, true, 300);
    assert(sb_c6_policy(&c, 3001, &idx) == SB_POLICY_VALUE && idx == 1 && !c.policy_failed);
    sb_c6_policy_choose(&c, 0, 4000);
    sb_c6_policy_response(&c, false, 0);
    assert(c.policy_failed && sb_c6_policy(&c, 4001, &idx) == SB_POLICY_VALUE && idx == 1);
    sb_c6_policy_choose(&c, 3, 5000);
    sb_c6_tick(&c, 5000 + SB_C6_POLICY_PENDING_MS);                     // no answer: shown as failed, not stuck
    assert(!c.policy_pending && c.policy_failed);
    assert(sb_c6_policy_seconds_for_index(0) == 60 && sb_c6_policy_seconds_for_index(1) == 300
           && sb_c6_policy_seconds_for_index(2) == 900 && sb_c6_policy_seconds_for_index(3) == 0);
    puts("policy");

    // ---- panel power order ---------------------------------------------------
    dpower_ops_t ops = { 0, o_asleep, o_wait, o_bl_off, o_panel, o_logo, o_flush, o_bl_on };
    trace[0] = 0;
    assert(dpower_sleep(&ops));
    assert(!strcmp(trace, "asleep;wait;bl_off;panel_off;"));
    trace[0] = 0;
    assert(dpower_wake(&ops));
    assert(!strcmp(trace, "logo;awake;flush;wait;panel_on;bl_restore;"));
    puts("display-order");

    // ---- a failing driver call is never reported as success --------------------------
    // sleep: every step still runs (the light is cut even if the frame wait timed out) ...
    trace[0] = 0; f_wait = true;
    assert(!dpower_sleep(&ops) && !strcmp(trace, "asleep;wait;bl_off;panel_off;"));
    f_wait = false; f_bl_off = true; trace[0] = 0;
    assert(!dpower_sleep(&ops) && strstr(trace, "panel_off"));
    f_bl_off = false; f_panel_off = true;
    assert(!dpower_sleep(&ops));
    f_panel_off = false;
    // ... wake: the panel and light stay OFF unless the fresh frame reached panel RAM
    f_flush = true; trace[0] = 0;
    assert(!dpower_wake(&ops) && !strstr(trace, "panel_on") && !strstr(trace, "bl_restore"));
    assert(!strcmp(trace, "logo;awake;flush;asleep;"));          // re-gated: nothing can draw
    f_flush = false; f_panel_on = true; trace[0] = 0;
    assert(!dpower_wake(&ops) && !strstr(trace, "bl_restore"));   // no light on a panel that did not confirm
    f_panel_on = false; f_bl_on = true;
    assert(!dpower_wake(&ops));
    f_bl_on = false;
    assert(dpower_wake(&ops));
    puts("display-failures");

    // The core: a failed sequence is retained, reported, retried - and never becomes "dark".
    sb_c6_init(&c, 0); c.boot_active = false;
    r = call(&c, rq(BB_ST_OP_SLEEP, 6, 40), 100);
    assert(!r.ready && sb_c6_display_action(&c) == SB_DISP_SLEEP);
    sb_c6_display_failed(&c, 110);                                  // the driver said no
    assert(!c.display_dark && c.display_fail && c.display_fail_count == 1);
    r = call(&c, rq(BB_ST_OP_SLEEP, 6, 40), 111);
    assert(!r.ready && r.failure == SB_C6_FAIL_HW && r.state != BB_ST_ASLEEP);   // reported, not hidden
    sb_c6_tick(&c, 111);
    assert(sb_c6_display_action(&c) == SB_DISP_NONE);              // backing off, not spinning
    sb_c6_tick(&c, 110 + SB_C6_DISPLAY_RETRY_MS - 1);
    assert(sb_c6_display_action(&c) == SB_DISP_NONE);
    sb_c6_tick(&c, 110 + SB_C6_DISPLAY_RETRY_MS);
    assert(sb_c6_display_action(&c) == SB_DISP_SLEEP);             // retried
    sb_c6_display_failed(&c, 700);                                  // fails again: history keeps counting
    assert(c.display_fail_count == 2);
    sb_c6_tick(&c, 700 + SB_C6_DISPLAY_RETRY_MS);
    sb_c6_display_done(&c, true, 1300);                             // finally dark
    r = call(&c, rq(BB_ST_OP_SLEEP, 6, 40), 1301);
    assert(r.ready && r.failure == 0 && r.state == BB_ST_ASLEEP && !c.display_fail && c.display_fail_count == 2);
    // a wake whose panel sequence fails reports it too, and stays dark
    r = call(&c, rq(BB_ST_OP_WAKE, 7, 41), 1400);
    assert(!r.ready && sb_c6_display_action(&c) == SB_DISP_WAKE);
    sb_c6_display_failed(&c, 1410);
    r = call(&c, rq(BB_ST_OP_WAKE, 7, 41), 1411);
    assert(!r.ready && r.failure == SB_C6_FAIL_HW && c.display_dark);
    r = call(&c, rq(BB_ST_OP_WAKE, 11, 41), 1412);
    assert(!r.ready && r.failure == SB_C6_FAIL_HW);
    // a new intent in the other direction starts a fresh attempt: the old failure is history
    r = call(&c, rq(BB_ST_OP_SLEEP, 6, 42), 1500);
    assert(r.failure == 0 && !c.display_fail && c.display_fail_count == 3 && sb_c6_display_action(&c) == SB_DISP_NONE);
    puts("display-failure-retained");
    return 0;
}
"""


def test_c6_standby_model_executes():
    out = compile_and_run(
        MAIN,
        sources=[f"{C6}/standby_c6_core.c", f"{C6}/display_power.c"],
        include_dirs=[C6, COMMON],
    )
    for marker in ("boot-clean/skipped-wifi", "boot-failure/replace", "boot-deadlines", "boot-p4-truth", "sleep",
                   "wake", "button-wake", "activity", "policy", "display-order", "display-failures",
                   "display-failure-retained"):
        assert marker in out.split(), marker
