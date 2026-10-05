#pragma once

// =============================================================================
// standby_p4_core.h - DAQ P4 standby PARTICIPANT, pure logic (no ESP-IDF).
//
// The ESP32-S3 mainboard owns the system standby policy and drives a sequence of
// numbered steps (standby_policy.h StandbyStep). The P4 answers each step over
// HATP_CMD_STANDBY (common/standby_wire.h). This file is the testable state
// machine behind that answer; standby_p4.c binds it to the real hardware.
//
// Wire contract (request/reply are 16 B, schema 1):
//   * stage numbers equal the S3 StandbyStep values below.
//   * POLL is read-only. SLEEP carries stages 1..6, WAKE carries stages 7..11.
//     PROGRESS never touches hardware; it feeds the C6 loading screen.
//   * The reply ALWAYS echoes the P4's CURRENT generation, never the request's.
//   * reply.ready for SLEEP/WAKE means "this stage is complete at this
//     generation". The S3 re-sends the same request until ready=1.
//   * A mutating request older than the current generation (modular compare) is
//     rejected with SB_FAIL_STALE; a repeat of a finished stage is a no-op ready.
//   * PROGRESS with stage BB_ST_BOOT_STAGE rebases the generation (an S3 boot is
//     a new epoch), so a rebooted S3 can never be locked out by a stale counter.
//   * Hardware stages run on the ctrl task and complete asynchronously: 2 (pause +
//     save converter config), 3 (analog muxes disconnected and read back, every rail
//     still present), 5 (analog rails off; refused unless 3 completed), 8 (rails on),
//     9 (converters reconfigured, routes STILL disconnected), 10 (supply verified off).
//     Each answers ready=0 until the worker
//     finishes; the worker echoes the generation it was queued with, and a completion
//     from another generation is dropped. 8 and 9 are only needed (and only run)
//     after stage 5 started; the S3 sends them regardless and gets an instant ready
//     when nothing was cut. 10 is refused (SB_FAIL_ORDER) until 9 has completed.
//   * Nothing a wake does connects a measurement route or restarts acquisition: both
//     wait for an EXPLICIT request after the system is ACTIVE (routes_held,
//     acq_resume_pending). Presence, status and telemetry are not requests.
//   * The P4 owns the Analyzer boot milestone: the S3's own claim of that bit is
//     discarded and replaced by the P4's bring-up result.
// =============================================================================

#include <stdbool.h>
#include <stdint.h>
#include "standby_wire.h"

#ifdef __cplusplus
extern "C" {
#endif

// S3 StandbyStep numbering (Firmware/ESP32/src/power/standby_policy.h).
#define SB_STAGE_QUIESCE        1u
#define SB_STAGE_HAT_SLEEP      2u
#define SB_STAGE_MUX_OFF        3u
#define SB_STAGE_OUTPUTS_OFF    4u
#define SB_STAGE_ANALOG_OFF     5u
#define SB_STAGE_INDICATORS_OFF 6u
#define SB_STAGE_WAKE_SAFE      7u
#define SB_STAGE_ANALOG_ON      8u
#define SB_STAGE_REINITIALIZE   9u
#define SB_STAGE_HAT_WAKE       10u
#define SB_STAGE_INDICATORS_ON  11u
#define SB_STAGE_MAX            11u

// reply.failure
#define SB_FAIL_NONE     0u
#define SB_FAIL_BUSY     1u   // an inhibitor or in-flight operation refuses sleep
#define SB_FAIL_STALE    2u   // request generation is older than the current one
#define SB_FAIL_ARG      3u   // bad schema / op / stage
#define SB_FAIL_HW       4u   // a hardware action did not succeed
#define SB_FAIL_ORDER    5u   // stage is not valid in the current state
#define SB_FAIL_TIMEOUT  6u   // the C6 (or a worker) did not confirm in time
#define SB_FAIL_FULL     7u   // lease table holds only live leases

// P4-only boot report to the C6 (BB_ST_BOOT_STAGE | 1): carries the Analyzer
// milestone alone, for a mainboard that has not reported its own yet.
#define SB_STAGE_BOOT_P4_ONLY 0x81u

// Analyzer boot milestone, decided by the P4 from its own hardware bring-up.
#define SB_BOOT_P4_PENDING 0u
#define SB_BOOT_P4_OK      1u
#define SB_BOOT_P4_FAILED  2u

#define SB_LEASE_MAX            4u
#define SB_LEASE_TTL_MIN_MS     1000u
#define SB_LEASE_TTL_DEFAULT_MS 15000u
#define SB_LEASE_TTL_MAX_MS     60000u

#define SB_C6_STEP_TIMEOUT_MS   2000u   // C6 must confirm stage 6 / 11 inside this
#define SB_C6_RETRY_MS          250u    // forward is repeated until confirmed
#define SB_WORKER_TIMEOUT_MS    15000u  // stop_fast bounds itself near 2 s; this is the backstop

typedef struct {
    uint32_t id;           // 0 = free slot
    uint32_t expires_ms;
} sb_lease_t;

typedef struct {
    // --- mirrored system policy (S3 authoritative) ---
    uint16_t timeout_s;
    bool     timeout_known;

    // --- transaction identity ---
    uint32_t generation;
    bool     gen_valid;
    uint8_t  state;              // BB_ST_*
    uint16_t steps_done;         // bit n = stage n finished at this generation
    uint8_t  failure;            // sticky for the current generation
    uint8_t  step_running;       // stage a worker / the C6 is executing (0 = none)
    uint32_t step_started_ms;
    uint32_t step_last_forward_ms;

    // --- hardware shadow ---
    bool     hw_paused;          // stage 2 completed and not yet undone by stage 10
    bool     fast_was_running;   // acquisition was running when stage 2 paused it
    bool     analog_cut;         // stage 5 started and the converters are not yet restored (stage 9)
    bool     routes_held;        // stage 3 started: the analog path stays disconnected until an explicit request
    bool     acq_resume_pending; // acquisition was running at sleep and is restarted by the first explicit request
    uint16_t steps_failed;       // bit n = stage n failed at this generation (reported to the C6)

    // --- work admission ---
    bool     barrier;            // set atomically by stage 2; no work is admitted
    uint32_t entered_seq;        // bumped by every admitted entry
    uint32_t inflight;           // admitted entries not yet left
    bool     wake_pending;       // work (or a button) wanted hardware while blocked

    // --- activity: monotonic, increments on every meaningful event ---
    uint32_t activity;

    // --- C6 mirror ---
    bool     c6_present;         // linked AND answering standby frames
    bool     c6_unsupported;     // linked but silent: an older C6 that cannot prove its screen is off
    bool     c6_expected;        // this board variant has a C6 display (the production DAQ HAT always does)
    bool     c6_missing;         // expected but not linked: the screen cannot be proven off
    uint32_t c6_inhibitors;
    uint32_t c6_activity_seen;
    bool     c6_activity_valid;
    uint32_t c6_confirmed_gen;   // generation of the last C6 reply with ready=1
    uint8_t  c6_confirmed_stage; // stage that reply confirmed (0 = none)
    bool     c6_failed;          // the last C6-confirmed stage failed or timed out
    bool     c6_dark;            // the C6 was told to go dark and is not yet restored

    // --- boot / loading progress, forwarded to the C6 ---
    uint16_t boot_completed, boot_failed, boot_skipped;   // the S3's report; the P4 bit is never taken from it
    bool     boot_seen;
    uint8_t  boot_p4;            // SB_BOOT_P4_*

    sb_lease_t leases[SB_LEASE_MAX];
} sb_p4_t;

typedef struct {
    uint8_t run_stage;       // nonzero: execute this stage on the worker now
    uint32_t run_generation; // the transaction it belongs to: the worker echoes it back
    bool    forward_c6;      // send a DDP_CMD_STANDBY request to the C6
    uint8_t forward_stage;   // stage the C6 request should carry
} sb_p4_fx_t;

void sb_p4_init(sb_p4_t *c);

// --- admission (any task; the glue wraps these in one critical section) ------
// Returns true and counts the caller as in-flight; false means "blocked": the
// work must be rejected, and the attempt has already bumped activity and raised
// wake_pending so the S3 wakes the system.
bool     sb_p4_admit(sb_p4_t *c);
// For work that was ALREADY admitted at its entry point and merely sat in a queue
// (the ctrl queue): the same gate, but a refusal is silent - it neither bumps
// activity nor raises wake_pending, so the standby sequence's own queued
// side effects can never wake the system they just put to sleep.
bool     sb_p4_admit_quiet(sb_p4_t *c);
void     sb_p4_leave(sb_p4_t *c);
bool     sb_p4_blocked(const sb_p4_t *c);
// Count a meaningful event without admitting work (button press, user command).
void     sb_p4_note_activity(sb_p4_t *c);
uint32_t sb_p4_entry_seq(const sb_p4_t *c);

// --- direct-USB client leases ------------------------------------------------
// op 1 = acquire/refresh, 0 = release. ttl_ms is clamped; 0 selects the default.
// Returns SB_FAIL_NONE, SB_FAIL_ARG (op > 1 or id 0) or SB_FAIL_FULL (a NEW id while
// every slot holds a live lease: nothing is evicted, nothing changes). A refresh of
// a held id and a release never fail. *count (optional) receives the live count.
uint8_t  sb_p4_lease(sb_p4_t *c, uint8_t op, uint32_t id, uint32_t ttl_ms, uint32_t now_ms,
                     uint32_t *count);
uint32_t sb_p4_lease_count(const sb_p4_t *c, uint32_t now_ms);
// The 16 B answer to a lease frame (USB_REC_STANDBY_ACK). failure: SB_FAIL_NONE, _ARG
// (malformed), _FULL, or _BUSY when the lease was recorded but the system is not
// ACTIVE yet (ready = 0; a wake is already requested).
void     sb_p4_lease_ack(const sb_p4_t *c, uint8_t failure, uint32_t now_ms, bb_standby_reply_t *out);

// --- S3 request ---------------------------------------------------------------
// local_inhibitors: BB_ST_INH_* bits from the real owners, sampled by the glue
// AFTER it read entry_seq via sb_p4_entry_seq(); entry_seq is that earlier value.
void sb_p4_handle(sb_p4_t *c, const bb_standby_request_t *rq, uint32_t now_ms,
                  uint32_t entry_seq, uint32_t local_inhibitors,
                  bb_standby_reply_t *rp, sb_p4_fx_t *fx);

// The ctrl task is about to run @p stage for @p generation. False = that stage is no
// longer the one in flight (a newer transaction took over, or it timed out): the
// worker must do nothing.
bool sb_p4_step_begin(const sb_p4_t *c, uint8_t stage, uint32_t generation);
// A worker finished a stage. Ignored unless @p generation is the current one and the
// stage is the one in flight, so a late completion can never advance a newer
// transaction. Returns true when the completion was ACCEPTED (the caller reports and
// forwards progress only then). fast_was_running is only meaningful for stage 2.
bool sb_p4_step_complete(sb_p4_t *c, uint8_t stage, uint32_t generation, bool ok,
                         bool fast_was_running);
// For simulators that do not track the transaction: completes @p stage of the CURRENT
// generation. The firmware never calls this; its worker echoes the generation it was
// queued with (sb_p4_step_complete).
static inline void sb_p4_step_done(sb_p4_t *c, uint8_t stage, bool ok, bool fast_was_running)
{
    sb_p4_step_complete(c, stage, c->generation, ok, fast_was_running);
}
// The Analyzer boot milestone, decided once the P4's own bring-up has finished.
void sb_p4_set_boot_p4(sb_p4_t *c, bool ok);

// An explicit request connected the measurement routes: they are no longer held.
void sb_p4_routes_connected(sb_p4_t *c);
// True once (and clears) when acquisition was running at sleep and has not been
// restarted yet; the explicit-request path restarts it.
bool sb_p4_take_acq_resume(sb_p4_t *c);

// --- C6 ----------------------------------------------------------------------
// This board variant carries a C6 display (default: yes). Only an explicit
// no-display variant may clear it; then an unlinked C6 is genuinely absent.
void sb_p4_set_c6_expected(sb_p4_t *c, bool expected);
// link_up: the C6 has said hello on the DDP link. responsive: it answered a standby
// frame recently. link_up && !responsive is an OLD C6, and !link_up on a board that
// expects one is a MISSING C6: either way the screen cannot be proven off, so both
// are reported as an inhibitor and stage 6 fails - never "absent".
void sb_p4_set_c6(sb_p4_t *c, bool link_up, bool responsive);
// Older callers: a C6 that is present is also answering.
static inline void sb_p4_set_c6_present(sb_p4_t *c, bool present)
{
    sb_p4_set_c6(c, present, present);
}
// A DDP_CMD_STANDBY reply arrived from the C6.
void sb_p4_c6_reply(sb_p4_t *c, const bb_standby_reply_t *r, uint32_t now_ms);
// Fill the request the P4 forwards to the C6. forward_stage 0 builds the periodic
// mirror (POLL, or boot PROGRESS while boot milestones are still arriving).
void sb_p4_build_c6_request(const sb_p4_t *c, uint8_t forward_stage,
                            const bb_standby_request_t *from_s3,
                            bb_standby_request_t *out);
// Periodic: time out C6 / worker steps. Returns true when a C6 re-forward is due.
bool sb_p4_tick(sb_p4_t *c, uint32_t now_ms);
// Union of everything that currently refuses sleep, for the reply.
uint32_t sb_p4_inhibitors(const sb_p4_t *c, uint32_t local_inhibitors, uint32_t now_ms);

// --- physical button gate ----------------------------------------------------
// Wraps the P4 button driver. While the system is not ACTIVE every event is
// swallowed, and a gesture that began while blocked (or that was swallowed) is
// consumed until every button is physically released AND the release debounce has
// run out, so no auto-repeat, queued BACK or release-time OK can toggle VDUT or
// open a menu after wake. any_held is the debounced state; any_raw is the raw pad
// level (a press the debouncer has not confirmed yet still counts as a gesture).
#define SB_BTN_RELEASE_HOLDOFF_MS 60u
typedef struct {
    bool consuming;
    bool prev_held;
    uint32_t holdoff_until_ms;
} sb_btn_gate_t;

typedef struct {
    uint8_t events;          // what may be relayed to the C6
    bool    activity;        // a meaningful press happened (count it)
    bool    wake_press;      // a press happened while blocked
    bool    start_discard;   // a gesture was just claimed: the driver must drop its pending state for it
} sb_btn_out_t;

void sb_btn_gate(sb_btn_gate_t *g, bool blocked, bool any_held, bool any_raw, uint8_t raw_events,
                 uint32_t now_ms, sb_btn_out_t *out);

#ifdef __cplusplus
}
#endif
