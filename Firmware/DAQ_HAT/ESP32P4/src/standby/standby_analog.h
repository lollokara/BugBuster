#pragma once

// =============================================================================
// standby_analog.h - switch the DAQ analog supplies off and back on.
//
// Binds standby_rails_core (order/recovery) and standby_adaq (register shadow) to
// the real pins, buses and the range manager. Every function runs on the ctrl
// task (the single owner of hardware work) while the standby barrier is up.
// See standby_rails_core.h for the sequences.
// =============================================================================

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct daq_board;

// Record that the rails are up (called at the end of boot bring-up).
void standby_analog_init(void);

// True when the transaction that started the running stage has been superseded.
// Polled between the steps of the long stages (power-on, converter restore) so a
// stale worker stops, leaves the board safe and unavailable, and never runs on.
void standby_analog_set_cancel(bool (*fn)(void));

// stage HAT_SLEEP, acquisition already stopped: copy every ADAQ configuration and
// the range state. Nothing is switched yet. False = a converter could not be read.
bool standby_analog_save(struct daq_board *b);

// stage MUX_OFF: with EVERY analog rail still present, MUX_EN low, both address
// buses low, bypass parked - then read the pads back. False = a pad disagrees.
bool standby_analog_mux_off(struct daq_board *b);

// stage ANALOG_OFF: refused (nothing touched) unless the muxes read back open now;
// then isolate, hold the converters in reset, +/-24 V, +/-26 V, analog 3V3 in that
// order and prove 3V3 fell.
bool standby_analog_off(struct daq_board *b);

// stage ANALOG_ON: 3V3 (power-good with a deadline), +/-26 V, +/-24 V, settle,
// SPI back, shared ADAQ reset released. Starts from a full reset after a partial
// failure; a repeat after success is a no-op. The muxes are not touched.
bool standby_analog_on(struct daq_board *b);

// stage REINITIALIZE: reset + identify + full configure of every converter from
// the saved copy, a conversion/status check, ADC gate open. The measurement ROUTES
// stay disconnected. Does not touch NVS or the settings registry and does not
// re-enable the DUT supply.
bool standby_analog_restore(struct daq_board *b);

// Explicit post-wake request (acquisition / measurement / setup / output): connect
// volt-mux address, range + FINE mux, MUX_EN through their owners, with readback.
// A no-op when nothing was disconnected. Never called by a wake sequence, a status
// poll or telemetry.
bool standby_analog_provision_routes(struct daq_board *b);

// False from the moment the converters are isolated until they are configured
// again: the converters themselves cannot be used.
bool standby_analog_adc_available(void);
// True while the analog path is held disconnected (from stage 3 until an explicit
// request connects it).
bool standby_analog_routes_held(void);
// Converters ready AND the analog path connected: the only state in which a number
// derived from an ADC may be presented as a measurement.
bool standby_analog_measurement_available(void);

// SB_RF_* of the last off/on sequence (for the log and the failure report).
uint32_t standby_analog_fail_mask(void);

#ifdef __cplusplus
}
#endif
