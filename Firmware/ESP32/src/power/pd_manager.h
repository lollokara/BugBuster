#pragma once

// =============================================================================
// pd_manager.h — USB-C PD profile manager
//
// Tracks per-consumer output-voltage demands and auto-negotiates the minimum
// USB-C PD profile that satisfies all of them, minimising input power draw.
//
// Converter topologies
//   BUCK       (VADJ1/VADJ2 on ESP32-S3 board): PD input must be ≥ output + headroom.
//   BUCK_BOOST (RP2040 HAT rails): PD input should be ≥ output for best efficiency;
//              the converter can boost above the input when needed, but drawing
//              the higher profile keeps cable current low.
//
// Sequencing contract (caller responsibility):
//   Increasing output → call pd_manager_ensure() BEFORE setting the DCDC.
//   Decreasing output → call pd_manager_ensure() AFTER setting the DCDC.
//   Use pd_manager_consumer_v() to determine the direction.
// =============================================================================

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Minimum headroom (V) above a buck-converter output voltage that the PD input
// must provide.  2 V is the hardware requirement; 0.5 V is a safety margin.
#define PD_DCDC_HEADROOM_V  2.5f

// Total number of tracked consumer slots.
// 2 VADJ buck rails + HAT_RAIL_COUNT (3) buck-boost rails = 5.
#define PD_MAX_CONSUMERS    5

// Slot assignments — keep VADJ indices matching DS4424 channel numbers (ch1/ch2).
typedef enum {
    PD_CONSUMER_VADJ1     = 0,  // DS4424 ch1 → VADJ1  (buck, ESP32-S3 board)
    PD_CONSUMER_VADJ2     = 1,  // DS4424 ch2 → VADJ2  (buck, ESP32-S3 board)
    PD_CONSUMER_HAT_RAIL0 = 2,  // RP2040 HAT rail 0   (buck-boost)
    PD_CONSUMER_HAT_RAIL1 = 3,  // RP2040 HAT rail 1   (buck-boost)
    PD_CONSUMER_HAT_RAIL2 = 4,  // RP2040 HAT rail 2   (buck-boost)
} PdConsumerId;

// Converter topology — determines the headroom calculation.
typedef enum {
    PD_TYPE_BUCK       = 0,  // Needs pd_v ≥ out_v + PD_DCDC_HEADROOM_V
    PD_TYPE_BUCK_BOOST = 1,  // Wants pd_v ≥ out_v; can boost above input if needed
} PdConsumerType;

/**
 * @brief Initialise the PD manager. Creates the internal mutex.
 *        Must be called once at boot, after husb238_init().
 */
void pd_manager_init(void);

/**
 * @brief Ensure the USB-C PD profile is appropriate for the requested output voltage.
 *
 * Updates the consumer's registered demand and negotiates a new PD profile when
 * the current profile no longer matches system requirements:
 *   - If the required PD voltage has increased → negotiates up, then returns.
 *   - If the required PD voltage has decreased → negotiates down, then returns.
 *
 * Negotiation issues GO_SELECT_PDO to the HUSB238 and blocks ~500 ms waiting
 * for the source to re-contract.  Call from a FreeRTOS task (not an ISR).
 *
 * @param id        Consumer slot (PdConsumerId).
 * @param output_v  Target output voltage in volts.  Pass 0 to mark idle.
 * @param type      PD_TYPE_BUCK or PD_TYPE_BUCK_BOOST.
 * @param warn      Optional buffer for a human-readable warning/status string.
 * @param warn_len  Byte length of the warn buffer.
 * @return true  PD profile is now sufficient (or was already).
 * @return false Negotiation failed (HUSB238 absent/detached, or source refuses).
 */
bool pd_manager_ensure(PdConsumerId id, float output_v, PdConsumerType type,
                       char *warn, size_t warn_len);

/**
 * @brief Return the currently registered output-voltage demand for a consumer.
 *        Returns 0.0f if the slot is idle or the id is out of range.
 *        Safe to call without holding any lock (atomic float read).
 */
float pd_manager_consumer_v(PdConsumerId id);

/**
 * @brief Return the minimum PD bus voltage (V) required by all active consumers.
 *        For diagnostics only — does NOT negotiate anything.
 */
float pd_manager_required_v(void);

#ifdef __cplusplus
}
#endif
