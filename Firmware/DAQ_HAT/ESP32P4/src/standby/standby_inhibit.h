#pragma once

// =============================================================================
// standby_inhibit.h - what refuses sleep on the DAQ P4, pure logic (no ESP-IDF).
//
// daq_board_standby_inhibitors() builds an sb_inh_inputs_t from the real owners and
// calls sb_inh_compute(); this is the SAME function the firmware runs, so the host
// tests exercise the production classification, not a copy.
//
// The ctrl queue is shared by user work (set source, rate, ACQ config, SMU apply,
// battery-sim codes, range calibration) and by the standby sequence's own hardware
// stages. Only USER work refuses sleep. The standby stages are the sequence itself:
// counting them would make the mainboard see "work in flight" on the reply to its
// own stage-2 request and cancel the sleep it just started.
//
// sb_ctrl_track_t separates the two with exact counters. Every call below must run
// inside ONE critical section together with the queue operation it describes
// (send / receive with a zero timeout), so the counters can never disagree with the
// queue:
//     enter();  ok = xQueueSend(q, &m, 0);  if (ok) sb_ctrl_posted(t, is_standby);  exit();
//     enter();  ok = xQueueReceive(q, &m, 0);  if (ok) sb_ctrl_taken(t, is_standby);  exit();
// =============================================================================

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t standby_queued;   // CTRL_MSG_STANDBY messages currently in the queue
    bool     standby_busy;     // the ctrl task is executing a standby stage
    bool     user_busy;        // the ctrl task is executing anything else
} sb_ctrl_track_t;

void sb_ctrl_track_init(sb_ctrl_track_t *t);
// A message was placed in the queue.
void sb_ctrl_posted(sb_ctrl_track_t *t, bool is_standby);
// The ctrl task took a message out of the queue and is now executing it.
void sb_ctrl_taken(sb_ctrl_track_t *t, bool is_standby);
// The ctrl task finished (or dropped) the message it took.
void sb_ctrl_done(sb_ctrl_track_t *t);
// Messages in the queue that are NOT standby stages, given the queue's total.
uint32_t sb_ctrl_user_queued(const sb_ctrl_track_t *t, uint32_t queue_total);
// True when user work is queued or executing.
bool sb_ctrl_user_work(const sb_ctrl_track_t *t, uint32_t queue_total);

typedef struct {
    bool     client_stream;        // host START (USB/TCP/S3), a TCP peer, or the WiFi stream bring-up
    bool     trigger_armed;
    bool     battsim_owns_supply;  // a battery-sim run is loaded (paused included)
    bool     calibration;          // SMU or range calibration in progress
    bool     ota;                  // any OTA / relay staging / push in progress
    bool     c6_uart_owner;        // something holds the C6 UART (WiFi bring-up, relay push, flasher)
    uint32_t ctrl_queue_total;     // uxQueueMessagesWaiting(ctrl_queue)
} sb_inh_inputs_t;

// BB_ST_INH_* of the P4's own owners. The standby stages themselves contribute nothing.
uint32_t sb_inh_compute(const sb_inh_inputs_t *in, const sb_ctrl_track_t *t);

#ifdef __cplusplus
}
#endif
