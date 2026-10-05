#pragma once

// =============================================================================
// standby_c6_core.h - DAQ C6 standby / boot-progress model, pure logic.
//
// The C6 is a PARTICIPANT of the system standby (the S3 decides, the P4 relays,
// see Firmware/DAQ_HAT/common/standby_wire.h) and the only chip with a screen.
// This file owns no hardware and no authoritative setting:
//   * it mirrors the S3's auto-standby timeout (never persists it),
//   * it turns milestone reports into the boot / wake loading view,
//   * it answers the P4's requests with the same generation/ack rules as the P4,
//   * it counts meaningful local activity (menu, buttons) for the S3's idle timer.
// Every transition that needs hardware is exposed as a "display action" the main
// loop performs and then confirms, so a reply never claims a screen is dark that
// has not actually been switched off.
// =============================================================================

#include <stdbool.h>
#include <stdint.h>
#include "standby_wire.h"

#ifdef __cplusplus
extern "C" {
#endif

// S3 StandbyStep numbers the C6 acts on / displays.
#define SB_C6_STAGE_INDICATORS_OFF 6u
#define SB_C6_STAGE_WAKE_SAFE      7u
#define SB_C6_STAGE_ANALOG_ON      8u
#define SB_C6_STAGE_REINIT         9u
#define SB_C6_STAGE_HAT_WAKE       10u
#define SB_C6_STAGE_INDICATORS_ON  11u

#define SB_C6_FAIL_NONE  0u
#define SB_C6_FAIL_STALE 2u
#define SB_C6_FAIL_ARG   3u
#define SB_C6_FAIL_HW    4u   // the display driver reported a failure; the screen is NOT confirmed in the asked state

// Boot milestones, in the order the splash shows them.
#define SB_C6_BOOT_ALL    ((uint16_t)(BB_ST_BOOT_IO | BB_ST_BOOT_WIFI | BB_ST_BOOT_P4 | \
                                      BB_ST_BOOT_DISPLAY | BB_ST_BOOT_SETTINGS | BB_ST_BOOT_C6_WIFI))
// Bits the S3 is authoritative for. The others are decided on this chip.
#define SB_C6_BOOT_REMOTE ((uint16_t)(BB_ST_BOOT_IO | BB_ST_BOOT_WIFI | BB_ST_BOOT_P4))
#define SB_C6_BOOT_S3     ((uint16_t)(BB_ST_BOOT_IO | BB_ST_BOOT_WIFI))
// The P4 reports the Analyzer milestone itself, in a POLL carrying this stage (and
// inside every forwarded S3 boot report once its own bring-up has finished).
#define SB_C6_STAGE_BOOT_P4_ONLY 0x81u
#define SB_C6_BOOT_ITEMS  6u

// Wake stages shown on the loading bar (bit n = stage n).
#define SB_C6_WAKE_MASK ((uint16_t)((1u << 7) | (1u << 8) | (1u << 9) | (1u << 10) | (1u << 11)))
#define SB_C6_WAKE_ITEMS 5u

#define SB_C6_BOOT_TIMEOUT_MS      25000u  // overall: unresolved milestones become "timed out"
#define SB_C6_LEGACY_S3_MS         15000u  // P4 is live but the S3 never sent a boot report
#define SB_C6_HOLD_CLEAN_MS        400u    // show a clean 100 % this long
#define SB_C6_HOLD_ISSUES_MS       2500u   // show a degraded result long enough to read
#define SB_C6_WAKE_HOLD_MS         400u
#define SB_C6_POLICY_PENDING_MS    5000u
#define SB_C6_DISPLAY_RETRY_MS     500u    // a failed panel sequence is retried at this pace

typedef enum { SB_ITEM_PENDING = 0, SB_ITEM_OK, SB_ITEM_FAILED, SB_ITEM_SKIPPED, SB_ITEM_TIMEOUT } sb_item_t;
typedef enum { SB_UI_NORMAL = 0, SB_UI_BOOT, SB_UI_WAKE, SB_UI_DARK } sb_ui_mode_t;
typedef enum { SB_DISP_NONE = 0, SB_DISP_SLEEP, SB_DISP_WAKE } sb_disp_action_t;

typedef struct {
    uint8_t  state;                 // BB_ST_* as this chip sees itself
    uint32_t generation;
    bool     gen_valid;

    // --- mirrored policy ---
    uint16_t timeout_s;
    bool     timeout_known;
    bool     policy_pending;
    uint16_t policy_pending_s;
    uint32_t policy_pending_ms;
    bool     policy_failed;

    // --- boot ---
    bool     boot_active;
    uint32_t boot_start_ms;
    uint32_t boot_resolved_ms;
    uint16_t remote_done, remote_fail, remote_skip;   // as last reported by the S3 (replaced, not OR-ed)
    uint16_t local_done, local_fail, local_skip;      // decided here
    uint16_t p4_done, p4_fail;                        // the Analyzer bit, as reported by the P4 itself
    uint16_t timed;                                   // unresolved at a deadline
    bool     s3_boot_seen;
    bool     p4_live;
    uint32_t p4_live_ms;

    // --- wake loading ---
    bool     wake_loading;
    bool     wake_notice_only;      // a button woke us; the mainboard has not asked for anything yet
    uint32_t wake_gen;
    bool     wake_gen_valid;
    uint16_t wake_done, wake_fail, wake_skip;         // bit n = stage n
    uint32_t wake_hold_until;
    bool     wake_hold_valid;

    // --- hardware intent / confirmation ---
    bool     want_dark;             // the display should be off
    bool     display_dark;          // the main loop confirmed it is off
    bool     indicators_off;        // controllable LEDs forced off (immediate)
    bool     display_fail;          // the last panel sequence in the wanted direction FAILED (retained until one succeeds)
    uint32_t display_fail_count;    // failed sequences since boot (history, never cleared)
    uint32_t display_retry_at;
    uint32_t tick_ms;               // the last sb_c6_tick() time

    uint32_t inhibitors;
    uint32_t activity;
} sb_c6_t;

typedef struct {
    sb_ui_mode_t mode;
    uint8_t      n;                      // items shown
    uint8_t      status[SB_C6_BOOT_ITEMS];
    const char  *label[SB_C6_BOOT_ITEMS];
    uint8_t      ok_count;
    uint8_t      current;                // first pending item, or n if none
    bool         issues;                 // something failed, was skipped or timed out
    bool         finished;               // every item resolved
    bool         waiting_mainboard;      // wake notice with no mainboard request yet
    bool         s3_silent;              // boot: the S3 never reported
} sb_c6_view_t;

void sb_c6_init(sb_c6_t *c, uint32_t now_ms);

// --- local milestones ------------------------------------------------------
void sb_c6_local(sb_c6_t *c, uint16_t bit, sb_item_t result);
// A valid frame arrived from the P4. This only proves the LINK (it starts the wait for
// an S3 report); the Analyzer milestone itself is whatever the P4 reports about its
// own bring-up - a frame is never taken as proof that the analyzer works.
void sb_c6_p4_alive(sb_c6_t *c, uint32_t now_ms);

// --- P4 request -------------------------------------------------------------
void sb_c6_handle(sb_c6_t *c, const bb_standby_request_t *rq, uint32_t now_ms,
                  bb_standby_reply_t *rp);
void sb_c6_tick(sb_c6_t *c, uint32_t now_ms);

// --- activity / inhibitors ----------------------------------------------------
void sb_c6_activity(sb_c6_t *c);
void sb_c6_set_inhibitors(sb_c6_t *c, uint32_t inhibitors);

// --- display / LED intent ------------------------------------------------------
sb_disp_action_t sb_c6_display_action(const sb_c6_t *c);
void sb_c6_display_done(sb_c6_t *c, bool now_dark, uint32_t now_ms);
// The panel sequence for the wanted direction ran but the display driver reported a
// failure. Nothing is confirmed: display_dark is untouched (never fabricated), the
// failure is retained and reported to the P4 (SB_C6_FAIL_HW, ready=0) until a
// sequence succeeds, and the sequence is retried after SB_C6_DISPLAY_RETRY_MS.
void sb_c6_display_failed(sb_c6_t *c, uint32_t now_ms);
bool sb_c6_indicators_off(const sb_c6_t *c);
// True when button events must be dropped (dark, booting, or waking).
bool sb_c6_input_blocked(const sb_c6_t *c, uint32_t now_ms);

// --- rendering ----------------------------------------------------------------
sb_ui_mode_t sb_c6_ui_mode(const sb_c6_t *c, uint32_t now_ms);
void sb_c6_view(const sb_c6_t *c, uint32_t now_ms, sb_c6_view_t *v);
const char *sb_c6_item_text(sb_item_t s);

// --- auto-standby policy mirror (menu) -------------------------------------------
typedef enum { SB_POLICY_UNKNOWN = 0, SB_POLICY_PENDING, SB_POLICY_VALUE } sb_policy_kind_t;
// idx: 0=1 min, 1=5 min, 2=15 min, 3=Off (valid for SB_POLICY_VALUE)
sb_policy_kind_t sb_c6_policy(const sb_c6_t *c, uint32_t now_ms, int *idx);
// Ask the S3 (via the P4 mailbox) to change it. Returns the seconds to send.
uint16_t sb_c6_policy_choose(sb_c6_t *c, int idx, uint32_t now_ms);
// The S3's answer to that request (DDP_MB_STANDBY_POLICY result data).
void sb_c6_policy_response(sb_c6_t *c, bool ok, uint16_t seconds);
uint16_t sb_c6_policy_seconds_for_index(int idx);

#ifdef __cplusplus
}
#endif
