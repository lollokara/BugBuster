#pragma once

// =============================================================================
// standby_p4.h - ESP32-P4 side of the system standby (binds standby_p4_core to
// the DAQ board). The ESP32-S3 mainboard owns the policy; this is the participant.
// See standby_p4_core.h for the wire semantics and
// Firmware/DAQ_HAT/common/standby_wire.h for the packed layouts.
// =============================================================================

#include <stdbool.h>
#include <stdint.h>
#include "standby_wire.h"

#ifdef __cplusplus
extern "C" {
#endif

struct daq_board;

// Bind to the board. Safe to call once, early (daq_board_init). Until it runs,
// every admit() succeeds so boot-time work is never refused.
void standby_p4_init(struct daq_board *b);

// The P4's own bring-up has finished: ok only when the analog rails, all three
// converters and the I2C devices came up. Feeds the C6 "Analyzer" boot milestone.
void standby_p4_boot_report(bool ok);

// --- work admission ----------------------------------------------------------
// Every path that touches the ADCs, the DUT supply or the settings store must
// bracket itself: if (!standby_p4_admit()) return /* rejected, wake requested */;
// ...; standby_p4_leave();
bool standby_p4_admit(void);
// A successful admit() is an EXPLICIT request (acquisition / measurement / setup /
// output from a host, the mainboard, the console or the front panel). After a wake it
// is the one thing that connects the measurement routes (through the range manager
// and the mux owner) and asks the ctrl task to restart the acquisition that was
// running at sleep. admit_quiet(), presence, status and telemetry never do.
// Admission for work that already passed admit() at its entry point and only sat
// in a queue: a refusal is silent (no activity, no wake request).
bool standby_p4_admit_quiet(void);
void standby_p4_leave(void);
bool standby_p4_blocked(void);
uint8_t standby_p4_state(void);                 // BB_ST_*
void standby_p4_note_activity(void);
// True when a number derived from an ADC may be presented as a measurement: the
// system is awake, the converters are configured AND the analog path is connected.
// After a wake the path stays disconnected until an explicit request (admission)
// connects it, so every status/diagnostic/telemetry path reports "unavailable" until
// then. Every path that would present an ADC-derived number checks this.
bool standby_p4_adc_available(void);
// The converters themselves are configured and answering (routes may still be open).
bool standby_p4_adc_ready(void);

// --- S3 link (HATP_CMD_STANDBY 0x7C -> HATP_RSP_STANDBY 0x9C) ----------------
// payload: bb_standby_request_t (16 B). out: bb_standby_reply_t (16 B).
// Returns 16, or -1 for a short/invalid frame (the caller sends RSP_ERROR).
int standby_p4_s3_request(const uint8_t *payload, uint8_t len, uint8_t *out);

// --- ctrl-task worker --------------------------------------------------------
// Runs one hardware stage for @p generation; does nothing when that transaction is
// no longer the one in flight.
void standby_p4_run_step(uint8_t stage, uint32_t generation);

// --- periodic (daq_ui_task, ~20 ms) -------------------------------------------
void standby_p4_service(uint32_t now_ms);

// --- C6 link -------------------------------------------------------------------
// A DDP_CMD_STANDBY frame (bb_standby_reply_t) arrived from the C6.
void standby_p4_on_c6_reply(const uint8_t *payload, uint8_t len);

// --- physical buttons ---------------------------------------------------------
// raw: events from buttons_p4_poll(). Returns the events to relay to the C6.
uint8_t standby_p4_button_filter(uint32_t now_ms, uint8_t raw);

// --- direct-USB client lease (USB_CMD_CLIENT_LEASE, stream/usb_proto.h) ---------
// Applies the frame and fills the 16 B USB_REC_STANDBY_ACK to send back. A frame of
// the wrong size, with a non-zero flags/reserved field, an unknown op or a zero id
// is refused as malformed (SB_FAIL_ARG) and changes nothing; a 5th client while four
// are live is refused (SB_FAIL_FULL), never evicting one. While at least one lease is
// live the P4 reports BB_ST_INH_HOST; a fresh lease while the system is not ACTIVE
// also requests a wake. A mounted USB device or an idle IN poll is never presence.
void standby_p4_usb_lease(const uint8_t *payload, uint16_t len, bb_standby_reply_t *ack);
uint8_t standby_p4_lease_count(void);

#ifdef __cplusplus
}
#endif
