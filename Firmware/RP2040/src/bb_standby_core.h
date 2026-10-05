#pragma once

// =============================================================================
// bb_standby_core.h - LA HAT (RP2040) standby PARTICIPANT, pure logic.
//
// The ESP32-S3 owns the whole standby policy and drives numbered stages
// (Firmware/ESP32/src/power/standby_policy.h StandbyStep). The RP2040 answers
// each one over HAT_CMD_STANDBY / HAT_RSP_STANDBY (DAQ_HAT/common/standby_wire.h,
// 16 B each, schema 1). This file is the host-testable state machine; the Pico
// binding is bb_standby.c. It has no SDK dependency: hardware actions go through
// the bb_sb_ops_t callbacks so tests can record their order and inject failures.
//
// Contract (mirrors the DAQ P4 participant so the S3 treats both HATs alike):
//   * Stage numbers equal the S3 StandbyStep values. SLEEP carries 1..6, WAKE 7..11.
//   * POLL is read-only and reports the live inhibitors + activity counter in any
//     state. PROGRESS never touches hardware: BOOT_STAGE rebases the S3 epoch, any
//     other PROGRESS is "not applied" (this HAT has no display).
//   * The reply always echoes the participant's CURRENT generation. A mutating
//     request older than that (modular compare) is refused with STALE and changes
//     nothing; a repeat of a finished stage is an idempotent ready.
//   * reply.ready for SLEEP/WAKE = "this stage is complete at this generation".
//     The S3 re-sends the same request until it is ready.
//   * Stage 2 sets the operation barrier atomically with the inhibitor recheck. From
//     the first stage on, new work is refused (HAT_ERR_BUSY / START_REJECTED), counted
//     as activity and raises wake_pending; nothing runs against a powered-down rail.
//   * A power-down error is FAULT_SAFE, never ASLEEP.
// =============================================================================

#include <stdbool.h>
#include <stdint.h>
#include "standby_wire.h"

#ifdef __cplusplus
extern "C" {
#endif

// S3 StandbyStep numbering.
#define BB_SB_STAGE_QUIESCE        1u
#define BB_SB_STAGE_HAT_SLEEP      2u
#define BB_SB_STAGE_MUX_OFF        3u
#define BB_SB_STAGE_OUTPUTS_OFF    4u
#define BB_SB_STAGE_ANALOG_OFF     5u
#define BB_SB_STAGE_INDICATORS_OFF 6u
#define BB_SB_STAGE_WAKE_SAFE      7u
#define BB_SB_STAGE_ANALOG_ON      8u
#define BB_SB_STAGE_REINITIALIZE   9u
#define BB_SB_STAGE_HAT_WAKE       10u
#define BB_SB_STAGE_INDICATORS_ON  11u

// reply.failure
#define BB_SB_FAIL_NONE  0u
#define BB_SB_FAIL_BUSY  1u   // an inhibitor or in-flight operation refuses sleep
#define BB_SB_FAIL_STALE 2u   // request generation older than the current one
#define BB_SB_FAIL_ARG   3u   // bad schema / op / stage
#define BB_SB_FAIL_HW    4u   // a hardware action did not verify
#define BB_SB_FAIL_ORDER 5u   // stage not valid in the current state

// Rails, numbered like HAT_RAIL_* in bb_config.h (a static assert in bb_standby.c
// keeps them equal).
#define BB_SB_RAIL_3V3_ADJ 0u
#define BB_SB_RAIL_VADJ3   1u
#define BB_SB_RAIL_VADJ4   2u

// A PREPARING participant that hears nothing from the S3 for this long returns to
// ACTIVE: stage 1 only freezes monitors, nothing is powered down yet.
#define BB_SB_PREPARE_TIMEOUT_MS 30000u
// A logical CMSIS-DAP session (DAP_Connect .. DAP_Disconnect) keeps inhibiting only
// while DAP frames keep refreshing it. A host that vanished without disconnecting
// stops blocking standby after this long.
#define BB_SB_DAP_TTL_MS         60000u
// While work is waiting for a wake the HAT IRQ is pulsed at this period.
#define BB_SB_IRQ_REPEAT_MS      500u

// Live facts sampled by the glue from the real owners, NOT cached UI flags.
typedef struct {
    bool la_armed;        // LA_STATE_ARMED (trigger armed)
    bool la_capturing;    // LA_STATE_CAPTURING
    bool la_streaming;    // LA_STATE_STREAMING
    bool la_usb_session;  // vendor-bulk stream START..STOP (bb_la_usb_is_streaming)
    bool la_usb_pending;  // readout / control markers / endpoint rearm still draining
    bool fw_update;       // staged firmware image receiving / ready / committing
    bool calibration;     // rail calibration task running or flash journal save pending
    bool uart_bridge;     // debugprobe CDC UART bridge open (compiled out on this PCB)
    bool bus;             // an external bus owner (none exists on this HAT)
    bool cmd_pending;     // HAT UART bytes already queued behind the current frame
} bb_sb_facts_t;

// Hardware actions. Every callback returns true only when the result was verified by
// reading the hardware back. Order of use is part of the contract (tests record it).
typedef struct {
    void *ctx;
    bool (*outputs_disable)(void *ctx);            // level-shifter OE low, then DIR to input
    bool (*pins_highz)(void *ctx);                 // EXP_EXT + IO bank disconnected, no pulls
    bool (*la_route_off)(void *ctx);               // LA route closed; false if LA still uses it
    bool (*rail_off)(void *ctx, uint8_t rail);     // guarded power API + readback
    bool (*verify_safe)(void *ctx);                // all DUT-facing enables low, pins input
    bool (*restore_ready)(void *ctx);              // DS4424 reachable, setpoints retained
    bool (*leds)(void *ctx, bool dark);            // WS2812 off / normal
} bb_sb_ops_t;

typedef struct {
    // --- transaction identity ---
    uint32_t generation;
    bool     gen_valid;
    uint8_t  state;              // BB_ST_*
    uint16_t steps_done;         // bit n = stage n finished at this generation
    uint8_t  failure;            // sticky for the current attempt
    uint8_t  step_running;       // stage whose hardware actions are executing (0 = none)
    uint32_t last_s3_ms;
    bool     s3_seen;

    // --- hardware shadow ---
    bool     hw_paused;          // stage 2 started and wake stage 10 has not undone it
    bool     leds_dark;          // stage 6 done and stage 11 has not undone it

    // --- operation barrier ---
    bool     barrier;            // set atomically by stage 2; no work is admitted
    uint32_t entered_seq;        // bumped by every admitted entry
    uint32_t inflight;           // admitted entries not yet left
    bool     wake_pending;       // work wanted hardware while blocked

    // --- activity: monotonic, one tick per meaningful event ---
    uint32_t activity;

    // --- logical CMSIS-DAP session (control-host counts) ---
    bool     dap_open;
    uint32_t dap_last_ms;
    uint32_t dap_inflight;
    uint32_t dap_frames;
    uint32_t dap_connects;
    uint32_t dap_disconnects;
    uint32_t dap_expired;

    // --- tick bookkeeping ---
    uint32_t prev_owner_inh;
    bool     prev_valid;
    bool     irq_armed;
    uint32_t irq_last_ms;
} bb_sb_t;

typedef struct {
    uint8_t run_stage;           // nonzero: execute bb_sb_run_stage() then bb_sb_step_done()
    bool    irq;                 // tick only: pulse the HAT IRQ line
} bb_sb_fx_t;

void bb_sb_init(bb_sb_t *c);

// --- operation barrier (any task; the glue wraps each call in one spin lock) ---
// A true return counts the caller in flight; false means "blocked": reject the work.
// The attempt has already counted as activity and raised wake_pending.
bool     bb_sb_admit(bb_sb_t *c);
void     bb_sb_leave(bb_sb_t *c);
bool     bb_sb_blocked(const bb_sb_t *c);
uint32_t bb_sb_entry_seq(const bb_sb_t *c);
void     bb_sb_note_activity(bb_sb_t *c);
// Periodic power-ADC monitoring is allowed only while ACTIVE.
bool     bb_sb_monitor_allowed(const bb_sb_t *c);

// --- CMSIS-DAP logical session ---
// enter: false = frame refused (blocked). A refused frame is counted as activity and
// raises wake_pending; the glue must answer with a rejection, not execute it.
bool bb_sb_dap_enter(bb_sb_t *c, uint32_t now_ms);
// leave: port_open is the debugprobe's own view after the frame (DAP_Data.debug_port
// != DISABLED), so DAP_Connect opens and DAP_Disconnect closes the session.
void bb_sb_dap_leave(bb_sb_t *c, uint32_t now_ms, bool port_open);
// The USB host went away (tud_unmount_cb): any DAP session ends with it.
void bb_sb_dap_host_gone(bb_sb_t *c);

// --- inhibitors ---
uint32_t bb_sb_inhibitors(const bb_sb_t *c, const bb_sb_facts_t *f, uint32_t now_ms);

// --- HAT UART commands ---
typedef enum {
    BB_SB_CMD_FREE = 0,      // never gated: status reads, cleanup, indicators, "turn off"
    BB_SB_CMD_WORK = 1,      // needs hardware: admit or refuse with BUSY
    BB_SB_CMD_STANDBY = 2,   // HAT_CMD_STANDBY itself
} bb_sb_cmd_class_t;

bb_sb_cmd_class_t bb_sb_classify(uint8_t cmd, const uint8_t *payload, uint8_t len);

// --- S3 request ---
// entry_seq: bb_sb_entry_seq() read BEFORE the facts were sampled, so work that slips
// in between is caught by the barrier recheck.
void bb_sb_handle(bb_sb_t *c, const bb_standby_request_t *rq, uint32_t now_ms,
                  uint32_t entry_seq, const bb_sb_facts_t *f,
                  bb_standby_reply_t *rp, bb_sb_fx_t *fx);

// A stage's hardware actions finished (ok = every action verified).
void bb_sb_step_done(bb_sb_t *c, uint8_t stage, bool ok);

// The hardware actions of a stage, in order. Stateless: the caller runs it outside any
// lock and reports the result with bb_sb_step_done(). Power-down never stops at the
// first error: it attempts every remaining step and then reports failure.
bool bb_sb_run_stage(uint8_t stage, const bb_sb_ops_t *ops);

// Reply reflecting the state AFTER a stage ran (used so one round trip reports ready).
void bb_sb_final_reply(const bb_sb_t *c, uint8_t stage, uint32_t inhibitors,
                       bb_standby_reply_t *rp);

// Periodic (every ~1 ms from the command task). Expires stale DAP sessions, bounds a
// stuck PREPARING, turns inhibitor releases into activity (a fresh idle interval for
// the S3), and asks for a HAT IRQ pulse while work is waiting for a wake.
void bb_sb_tick(bb_sb_t *c, uint32_t now_ms, const bb_sb_facts_t *f, bb_sb_fx_t *fx);

// --- one HAT_CMD_STANDBY frame, start to finish ---
// The single flow the firmware runs, so tests exercise the real thing: state changes
// happen under env->lock, facts are sampled and hardware actions run OUTSIDE it, and a
// stage's own reply already reports the post-stage truth (one round trip to ready).
typedef struct {
    void *ctx;
    void (*lock)(void *ctx);
    void (*unlock)(void *ctx);
    uint32_t (*now_ms)(void *ctx);
    void (*sample)(void *ctx, bb_sb_facts_t *f);
} bb_sb_env_t;

void bb_sb_service(bb_sb_t *c, const bb_sb_env_t *env, const bb_sb_ops_t *ops,
                   const uint8_t *payload, uint8_t len, bb_standby_reply_t *rp);

#ifdef __cplusplus
}
#endif
