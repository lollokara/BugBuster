#pragma once
// =============================================================================
// standby_system.h - S3 standby COORDINATOR (pure C, no ESP-IDF).
//
// standby_policy.c decides WHEN and in which order; this file runs it: it owns
// the operation barrier, gathers the real inhibitors, executes every step (local
// hardware via an ops table, the HAT through the standby_wire.h exchange) and
// keeps the results the BBP / HTTP / HAT surfaces report. standby_hw.cpp binds
// the ops table to the real drivers; tests bind it to fakes.
//
// Locking: ops.lock/unlock guard ONLY this struct. They are never held across a
// hardware step or a HAT exchange. Inhibitor getters run under the lock on the
// atomic re-check and therefore must be lock-free or try-lock (fail closed).
// =============================================================================

#include <stdbool.h>
#include <stdint.h>

#include "standby_policy.h"
#include "standby_wire.h"

#ifdef __cplusplus
extern "C" {
#endif

#define STANDBY_HAT_POLL_MS        250u   // read-only POLL cadence while a HAT is linked
#define STANDBY_HAT_STAGE_RETRY_MS 40u    // a SLEEP/WAKE stage is re-sent at most this often
#define STANDBY_HAT_POLL_FAIL_WAKE 8u     // consecutive failed polls while asleep => wake
#define STANDBY_HAT_MISMATCH_POLLS 2u     // polls a non-ACTIVE HAT is tolerated beside an ACTIVE S3
#define STANDBY_SAFE_RETRY_MS      1000u  // FAULT_SAFE output cleanup is retried until it verifies
/* Bit of a step in the failed / skipped / completed masks (bit 0 is NO_STEP, never used). */
#define STANDBY_STEP_BIT(step)     ((uint16_t)(1u << (step)))
#define STANDBY_STATUS_LEN         28u

typedef enum {
    STANDBY_HAT_NONE = 0,   // nothing detected: HAT parts of every step are skipped
    STANDBY_HAT_DETECTED,   // detected but the link is not usable (booting, silent)
    STANDBY_HAT_LINKED,     // UART connected
} StandbyHatLink;

typedef enum {
    STANDBY_XCHG_OK = 0,
    STANDBY_XCHG_TIMEOUT,       // no / corrupt answer
    STANDBY_XCHG_UNSUPPORTED,   // answered with an error: firmware predates standby
} StandbyXchg;

typedef enum {
    STANDBY_STEP_DONE = 0,
    STANDBY_STEP_PENDING,       // call again on the next tick
    STANDBY_STEP_FAILED,
} StandbyStepResult;

typedef enum {
    STANDBY_ADMIT_OK = 0,
    STANDBY_ADMIT_WAKING,       // not ACTIVE: a wake has been requested, retry later
    STANDBY_ADMIT_FAULT,        // FAULT_SAFE: a wake (recovery) has been requested
} StandbyAdmit;

/* Result codes of the explicit requests (map 1:1 onto CmdError in the glue). */
typedef enum {
    STANDBY_RC_OK = 0,
    STANDBY_RC_BAD_ARG,
    STANDBY_RC_BUSY,            // real inhibitor / work in flight / transition running
    STANDBY_RC_INVALID_STATE,
    STANDBY_RC_CAPACITY,        // client table full
    STANDBY_RC_HARDWARE,        // the setting could not be persisted; nothing was applied
} StandbyRc;

/* Which transport an explicit request arrived on. Only USB has a session whose
 * presence is implied by the protocol handshake. */
typedef enum {
    STANDBY_SRC_OTHER = 0,
    STANDBY_SRC_USB,
} StandbySource;

/* Protocol epoch of the USB (BBP) link. A handshake opens a LEGACY epoch: the
 * session itself counts as a logical client because the host cannot say
 * otherwise. The first accepted presence registration of the epoch makes it
 * EXPLICIT: from then on the host is represented only by its bounded client
 * lease, so releasing it lets the system sleep while the CDC port stays open. */
#define STANDBY_USB_NONE     0u
#define STANDBY_USB_LEGACY   1u
#define STANDBY_USB_EXPLICIT 2u

typedef struct {
    void *ctx;
    void (*lock)(void *ctx);
    void (*unlock)(void *ctx);
    uint32_t (*now_ms)(void *ctx);                       // monotonic, wraps
    uint32_t (*inhibitors)(void *ctx);                   // BB_ST_INH_* of the S3 owners
    StandbyHatLink (*hat_link)(void *ctx);
    StandbyXchg (*hat_exchange)(void *ctx, const bb_standby_request_t *rq,
                                bb_standby_reply_t *rp);
    /* Local part of a step. May be called repeatedly while it returns PENDING. */
    StandbyStepResult (*local_step)(void *ctx, StandbyStep step, uint32_t generation);
    /* Store the timeout. false = not stored, and what is stored must then still be
     * the previous value. Runs inside write_lock, never inside lock. */
    bool (*persist_timeout)(void *ctx, uint32_t seconds);
    /* Optional. One policy write at a time (validate, persist, apply) without holding
     * lock across storage I/O. write_lock may fail (bounded wait) => BUSY. */
    bool (*write_lock)(void *ctx);
    void (*write_unlock)(void *ctx);
    /* Optional. Called while FAULT_SAFE until it returns true: force every eligible
     * output off and say whether that was verified. Runs outside lock. */
    bool (*safe_outputs_off)(void *ctx);
} StandbyOps;

typedef struct {
    StandbyPolicy policy;
    StandbyOps    ops;

    uint32_t work_depth;          // admitted operations still running
    uint32_t analog_depth;        // worker iterations currently touching analog hardware
    uint32_t work_admitted;       // monotonic, diagnostics
    uint32_t work_refused;        // monotonic, diagnostics

    /* HAT mirror */
    uint32_t hat_inhibitors;
    uint32_t hat_activity;
    bool     hat_activity_valid;
    bool     hat_supported;       // last POLL was a valid schema-1 answer
    uint8_t  hat_poll_failures;
    uint32_t hat_poll_due_ms;
    bool     hat_poll_started;

    /* Running step */
    bool         exec_active;
    StandbyStep  exec_step;
    uint32_t     exec_generation;
    bool         exec_touched;    // anything (local or HAT) has already acted
    bool         exec_local_done;
    bool         exec_hat_done;
    uint32_t     exec_hat_due_ms;
    bool         exec_hat_started;
    bool         exec_hat_skipped;

    /* Reporting */
    uint32_t seen_generation;
    uint16_t failed_mask;
    uint16_t skipped_mask;
    uint16_t boot_completed, boot_failed, boot_skipped;
    bool     boot_dirty;          // a milestone has not reached the HAT yet
    uint32_t last_inhibitors;
    uint32_t cancelled_prepares;
    volatile bool analog_ok;      // lock-free mirror of state == ACTIVE
    bool     initialised;

    /* USB protocol epoch (STANDBY_USB_*). Lock-free: read by the inhibitor getter. */
    volatile uint8_t usb_mode;

    /* The HAT's own state, as last reported */
    uint8_t  hat_remote_state;
    bool     hat_state_valid;
    uint8_t  hat_mismatch_polls;
    uint32_t resyncs;             // coordinated wakes started to reconcile a stale peer

    /* Steps that failed before the first ACTIVE (SPI / mux / PCA at boot) */
    uint16_t boot_fault_mask;

    /* FAULT_SAFE output cleanup */
    bool     safe_ok;
    uint32_t safe_gen;
    uint32_t safe_due_ms;
    uint32_t safe_attempts;
} StandbySystem;

void standby_system_init(StandbySystem *s, const StandbyOps *ops, uint32_t timeout_seconds);

/* Main-loop driver. Executes at most one slice of one step per call. */
void standby_system_tick(StandbySystem *s);

/* --- operation barrier ------------------------------------------------------ */
/* Count one operation that needs powered hardware. Pair with work_end(). On a
 * non-OK result nothing runs and no work_end() is due; a wake has been requested. */
StandbyAdmit standby_system_work_begin(StandbySystem *s);
void standby_system_work_end(StandbySystem *s);
/* True while ACTIVE and no transition is running: analog hardware may be touched.
 * Lock-free snapshot, for cheap early-outs (SPI gate); workers use analog_enter(). */
bool standby_system_analog_allowed(const StandbySystem *s);
/* Bracket one iteration of a worker that touches analog hardware. enter() is
 * refused unless ACTIVE; the QUIESCE step waits for the depth to reach zero. */
bool standby_system_analog_enter(StandbySystem *s);
void standby_system_analog_leave(StandbySystem *s);
uint32_t standby_system_analog_depth(StandbySystem *s);
/* Meaningful activity that is not itself an operation (button, CLI keystroke...). */
void standby_system_activity(StandbySystem *s);

/* --- explicit requests ------------------------------------------------------- */
/* OK also for releasing an id that is not (or no longer) registered. CAPACITY when the
 * table is full, BAD_ARG for id 0. A successful registration over USB switches the
 * current USB epoch to EXPLICIT. */
StandbyRc standby_system_presence(StandbySystem *s, uint32_t client_id, bool present,
                                  StandbySource src);
/* The USB link opened (handshake: LEGACY epoch) or closed (NONE). Wakes on open. */
void standby_system_usb_session(StandbySystem *s, bool open);
/* True while a USB session exists that never used the presence API. Lock-free. */
bool standby_system_usb_legacy_host(const StandbySystem *s);
/* Validate, persist, then apply - in that order, one writer at a time. HARDWARE: the
 * value was not stored, the previous one is still in force. */
StandbyRc standby_system_set_timeout(StandbySystem *s, uint32_t seconds);
/* Hardware found unusable at boot. Not ready (and sleep refused) until a wake brings
 * every stage back; the mask uses STANDBY_STEP_BIT(). */
void standby_system_boot_fault(StandbySystem *s, uint16_t failed_steps);
void standby_system_wake(StandbySystem *s);
StandbyRc standby_system_sleep(StandbySystem *s);

/* --- reporting ---------------------------------------------------------------- */
typedef struct {
    uint8_t  state;               // BB_ST_*
    uint8_t  ready;               // 1 only in ACTIVE
    uint8_t  stage;               // current StandbyStep, 0 none
    uint32_t generation;
    uint16_t timeout_seconds;
    uint8_t  clients;
    uint8_t  failed_stage;
    uint32_t inhibitors;
    uint16_t completed, failed, skipped;
    uint32_t idle_remaining_ms;
} StandbyStatus;

void standby_system_get(StandbySystem *s, StandbyStatus *out);
uint8_t standby_system_state_code(const StandbyPolicy *p);

/* --- boot progress ------------------------------------------------------------ */
/* Report real boot milestones (BB_ST_BOOT_*) to the HAT as they happen. */
void standby_system_boot_progress(StandbySystem *s, uint16_t completed, uint16_t failed,
                                  uint16_t skipped);

#ifdef __cplusplus
}
#endif
