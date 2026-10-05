#pragma once

// =============================================================================
// standby_rails_core.h - switched analog supplies of the DAQ P4, pure sequencing.
//
// The analog front end hangs on three board rails: the analog 3V3 LDO (TPS74601,
// EN GPIO42, PG GPIO41) that powers the three ADAQ7769, and the +/-26 V (ADP5071,
// EN GPIO54) and +/-24 V (EN GPIO3) supplies. 3V3_ESP, the 5V3 buck, the C6, the
// DS4424, the temperature sensors and the buttons are separate and stay on.
//
// This file owns ORDER and RECOVERY only. Every pin, delay and bus is reached
// through sb_rails_ops_t so the sequence is executed (and fault-injected) on the
// host by tests/firmware_host/test_daq_standby_rails.py before it touches a GPIO.
//
//   mux : (stage 3, every analog rail still present) MUX_EN low + both address
//         buses low -> bypass parked -> readback proves both muxes disconnected
//   off : GUARD (mux already disconnected and re-verified, else nothing is touched)
//         -> ADC gate closed -> ADAQ *RST held low -> SPI/CS idle high-Z ->
//         +/-24 V -> +/-26 V -> 3V3 -> PG must fall
//   on  : (reset everything first if an earlier attempt left it half done) ->
//         3V3 -> PG must rise (deadline) -> +/-26 V -> +/-24 V -> settle ->
//         SPI restored -> shared ADAQ hardware reset released. The muxes are NOT
//         touched: they stay disconnected through the whole wake.
//   conn: (only on an explicit post-wake request) volt-mux address -> range/FINE
//         mux -> MUX_EN -> readback. Never part of a wake sequence.
//
// A sequence reports failure truthfully (a PG read error is a failure, not "low")
// and always ends in a safe state: a failed power-on switches everything off again.
// =============================================================================

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum { SB_RAIL_24V = 0, SB_RAIL_26V, SB_RAIL_3V3 } sb_rail_t;
typedef enum { SB_GATE_OPEN = 0, SB_GATE_CLOSED, SB_GATE_OWNER_ONLY } sb_gate_t;

// fail_mask bits
#define SB_RF_MUX         (1u << 0)
#define SB_RF_BYPASS      (1u << 1)
#define SB_RF_GATE        (1u << 2)
#define SB_RF_RESET       (1u << 3)
#define SB_RF_BUS         (1u << 4)
#define SB_RF_RAIL_24V    (1u << 5)
#define SB_RF_RAIL_26V    (1u << 6)
#define SB_RF_RAIL_3V3    (1u << 7)
#define SB_RF_PG_READ     (1u << 8)    // the power-good input could not be read
#define SB_RF_PG_STUCK    (1u << 9)    // 3V3 still reported good after it was switched off
#define SB_RF_PG_TIMEOUT  (1u << 10)   // 3V3 never reported good
#define SB_RF_PULSE       (1u << 11)   // the shared ADAQ reset could not be released
#define SB_RF_MUX_GUARD   (1u << 12)   // rails NOT cut: the muxes were not proven disconnected
#define SB_RF_MUX_VERIFY  (1u << 13)   // the readback of the mux pins disagreed with what was driven
#define SB_RF_CANCELLED   (1u << 14)   // a newer transaction took over mid-sequence; left safe
#define SB_RF_NOT_POWERED (1u << 15)   // routes requested while the analog supplies are not restored

#define SB_PG_OFF_MS      150u   // PG must fall within this after EN goes low
#define SB_PG_ON_MS       100u   // PG must rise within this after EN goes high
#define SB_RAIL_GAP_US    1000u  // between disabling two rails
#define SB_RAIL_DRAIN_MS  20u    // rails fully off before a restart-from-scratch
#define SB_RAIL_SETTLE_MS 10u    // after +/-24 V, before the first ADAQ access

typedef struct {
    void *ctx;
    bool (*mux_disconnect)(void *ctx);                 // U24 + U25 MUX_EN low, address buses low
    bool (*bypass_park)(void *ctx);                    // bypass outputs low, autorange edges ignored
    bool (*mux_verify)(void *ctx);                     // pad readback: both muxes disconnected, bypass low
    bool (*mux_connect)(void *ctx);                    // volt-mux addr, range/FINE mux, MUX_EN, readback
    bool (*adc_gate)(void *ctx, sb_gate_t mode);       // every ADAQ SPI access refused unless open
    bool (*adaq_reset)(void *ctx, bool asserted);      // shared *RST: asserted = driven low
    bool (*bus_release)(void *ctx);                    // SCLK/MOSI/CS high-Z, DRDY interrupts off
    bool (*bus_restore)(void *ctx);                    // SPI pins back on the peripheral, DRDY armed
    bool (*rail_set)(void *ctx, sb_rail_t rail, bool on);
    int  (*pg_read)(void *ctx);                        // 1 good, 0 not good, <0 read failure
    bool (*adaq_pulse)(void *ctx);                     // hardware reset pulse + datasheet settle
    void (*delay_us)(void *ctx, uint32_t us);
    // True once the transaction that started this sequence was superseded. Polled
    // between the steps of a power-ON only; a power-OFF always finishes (switching
    // off is the safe direction). May be NULL.
    bool (*cancelled)(void *ctx);
} sb_rails_ops_t;

typedef struct {
    bool     cut;        // a sequence switched (or began switching) the rails off: config must be restored
    bool     dirty;      // a sequence did not finish: the next power-on starts from a full reset
    bool     powered;    // the analog supplies are known to be on
    bool     mux_open;   // a disconnect was attempted: routes are HELD until an explicit request
    bool     mux_verified; // the disconnect was read back and proven (the cut guard)
    uint32_t fail_mask;  // SB_RF_* of the most recent sequence
} sb_rails_t;

void sb_rails_init(sb_rails_t *r, bool powered);

// Stage MUX_OFF: disconnect both muxes with every analog rail still present, park
// the bypass switches and PROVE it by readback. Touches no rail and no converter.
// False = anything failed; r->mux_open is set regardless (routes are held),
// r->mux_verified only when the readback agrees.
bool sb_rails_mux_off(sb_rails_t *r, const sb_rails_ops_t *ops);

// Explicit post-wake request: connect the measurement routes. Needs the analog
// supplies restored (r->powered). A failure leaves them disconnected.
bool sb_rails_mux_connect(sb_rails_t *r, const sb_rails_ops_t *ops);

// Return true when the sequence completed with no failure. Every step is
// attempted even after a failure, so the board always ends as safe as possible.
// sb_rails_off() first applies the GUARD: unless the muxes are proven disconnected
// (now, not just at stage 3) it touches NOTHING, sets only SB_RF_MUX_GUARD and
// leaves r->cut false.
bool sb_rails_off(sb_rails_t *r, const sb_rails_ops_t *ops);
bool sb_rails_on(sb_rails_t *r, const sb_rails_ops_t *ops);

#ifdef __cplusplus
}
#endif
