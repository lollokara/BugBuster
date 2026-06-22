#pragma once

// =============================================================================
// daq_trigger.h — DAQ trigger / flag event engine (ESP32-S3 mainboard side).
//
// The 12 mainboard IOs can each be tagged as a FLAG or a TRIGGER source for a
// DAQ acquisition running on the ESP32-P4:
//
//   FLAG    — every matching edge emits a digital MARKER on the P4 USB stream,
//             rendered as a vertical line in the acquisition + timeline views.
//   TRIGGER — when armed, the configured edge(s) start the acquisition window
//             (defines t=0). Multiple triggers combine with OR or AND logic.
//
// Digital IOs (1,2,4,5,7,8,10,11) are read from the ESP32 GPIO (dio module).
// HV / analog-capable IOs (3,6,9,12 → AD74416H channels A..D) may instead use
// an analog voltage threshold or the AD74416H DIN comparator; the engine is fed
// their level/voltage from the ADC/fault task so it stays decoupled from the
// AD74416H driver.
//
// Events are forwarded to the P4 over the HAT link as HAT_CMD_DAQ_MARK; arming
// is forwarded as HAT_CMD_DAQ_ARM. The P4 stamps each marker with the live
// fused-sample index so it survives the multi-resolution decimation on the host.
// =============================================================================

#include <stdint.h>
#include <stdbool.h>
#include "dio.h"

#ifdef __cplusplus
extern "C" {
#endif

// Per-IO role.
#define DAQ_TRIG_ROLE_OFF      0
#define DAQ_TRIG_ROLE_FLAG     1
#define DAQ_TRIG_ROLE_TRIGGER  2

// Edge selection.
#define DAQ_TRIG_EDGE_RISING   0
#define DAQ_TRIG_EDGE_FALLING  1
#define DAQ_TRIG_EDGE_ANY      2

// Event source (analog/DIN only valid for IO 3,6,9,12).
#define DAQ_TRIG_SRC_DIGITAL   0   // ESP32 GPIO level (LV) or AD74416H DIN (HV)
#define DAQ_TRIG_SRC_ANALOG    1   // AD74416H ADC voltage vs threshold (HV)

// Trigger-group combination logic.
#define DAQ_TRIG_LOGIC_NONE    0
#define DAQ_TRIG_LOGIC_OR      1   // first matching trigger IO fires the run
#define DAQ_TRIG_LOGIC_AND     2   // all trigger IOs must match before firing

// Per-IO configuration. Wire-compatible with the desktop/Python TLV encoding.
typedef struct __attribute__((packed)) {
    uint8_t role;          // DAQ_TRIG_ROLE_*
    uint8_t edge;          // DAQ_TRIG_EDGE_*
    uint8_t source;        // DAQ_TRIG_SRC_* (analog ignored for digital-only IOs)
    uint8_t _pad;
    float   threshold_v;   // analog threshold (V), used when source == ANALOG
} daq_trig_io_cfg_t;

/** @brief Reset all IO roles to OFF, logic to OR, disarm. */
void daq_trigger_init(void);

/** @brief Set the trigger-group combination logic (DAQ_TRIG_LOGIC_*). */
void daq_trigger_set_logic(uint8_t logic);
uint8_t daq_trigger_get_logic(void);

/** @brief Returns true if @p io (1..12) is analog-capable (HV: 3,6,9,12). */
bool daq_trigger_io_is_analog_capable(uint8_t io);

/** @brief Configure one IO (1..12). Returns false if io is out of range. */
bool daq_trigger_set_io(uint8_t io, const daq_trig_io_cfg_t *cfg);

/** @brief Read one IO's configuration. Returns false if io is out of range. */
bool daq_trigger_get_io(uint8_t io, daq_trig_io_cfg_t *out);

/**
 * @brief Arm or disarm the trigger logic and forward it to the P4.
 * @param armed        true = wait for trigger edge before t=0; false = free-run.
 * @param pre_samples  requested pre-trigger depth (fused samples) for the host.
 */
void daq_trigger_arm(bool armed, uint32_t pre_samples);
bool daq_trigger_is_armed(void);
bool daq_trigger_has_fired(void);

/**
 * @brief Poll all digital IO levels and emit flag/trigger markers on edges.
 *        Call from the IO poll task with the cached DIO state array.
 *        Analog-source IOs are skipped here (use daq_trigger_feed_analog).
 */
void daq_trigger_poll_digital(const DioState *all_dio);

/**
 * @brief Feed the latest analog reading for an analog-source IO (3,6,9,12).
 *        The engine derives a logical level from @p volts vs the threshold and
 *        emits flag/trigger markers on threshold crossings.
 */
void daq_trigger_feed_analog(uint8_t io, float volts);

#ifdef __cplusplus
}
#endif
