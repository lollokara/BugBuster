#pragma once

// =============================================================================
// dio.h - Digital IO (DIO) — ESP32 GPIO-based digital input/output
//
// Manages 12 logical IOs mapped to ESP32 GPIO pins.  In PCB mode the GPIOs
// pass through the ADGS2414D MUX matrix and TXS0108E level shifters to the
// physical terminal blocks.  In breadboard mode they are directly accessible
// on the ESP32 dev-board headers.
//
// The MUX routing (which switch to close) is NOT managed here — that is the
// host library's responsibility (see hal.py).  This module only drives the
// ESP32 GPIO pins themselves.
//
// IO numbering: 1–12 (matches the HAL port numbers)
//   IO 3, 6, 9, 12 — analog-capable (position 3 in each connector block)
//   IO 1, 2, 4, 5, 7, 8, 10, 11 — digital-only
//
// All 12 IOs can be used as digital input or output through this module.
// =============================================================================

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DIO_NUM_IOS     12      // Total logical IOs (1–12)
#define DIO_FIRST_IO    1       // First valid IO number
#define DIO_LAST_IO     12      // Last valid IO number

// IO direction modes
#define DIO_MODE_DISABLED   0   // High-impedance (not configured)
#define DIO_MODE_INPUT      1   // Digital input
#define DIO_MODE_OUTPUT     2   // Digital output

// Per-IO state
typedef struct {
    int8_t   gpio_num;      // ESP32 GPIO pin number (-1 = not mapped)
    uint8_t  mode;          // DIO_MODE_*
    bool     output_level;  // Last written output level
    bool     input_level;   // Last read input level
    bool     pulldown;      // Internal pull-down enabled
} DioState;

/**
 * @brief  Initialize the DIO module.
 *         Sets up the GPIO pin mapping for the PCB but does NOT configure any pins
 *         as input/output — all start as DISABLED.
 */
void dio_init(void);

/**
 * @brief  Configure an IO's direction.
 *
 * @param io    IO number (1–12)
 * @param mode  DIO_MODE_DISABLED, DIO_MODE_INPUT, or DIO_MODE_OUTPUT
 * @return true on success, false if io is out of range or not mapped
 */
bool dio_configure(uint8_t io, uint8_t mode);

/**
 * @brief  Configure an IO's direction and pulldown.
 *
 * @param io        IO number (1–12)
 * @param mode      DIO_MODE_DISABLED, DIO_MODE_INPUT, or DIO_MODE_OUTPUT
 * @param pulldown  true = enable internal pulldown (INPUT or DISABLED mode only)
 * @return true on success
 */
bool dio_configure_ext(uint8_t io, uint8_t mode, bool pulldown);

/**
 * @brief  Set an output IO level.
 *
 * @param io     IO number (1–12), must be configured as DIO_MODE_OUTPUT
 * @param level  true = HIGH, false = LOW
 * @return true on success
 */
bool dio_write(uint8_t io, bool level);

/**
 * @brief  Read an input IO level.
 *
 * @param io  IO number (1–12), must be configured as DIO_MODE_INPUT
 * @return true = HIGH, false = LOW (returns false on error)
 */
bool dio_read(uint8_t io);

/**
 * @brief  Get the full state of an IO.
 *
 * @param io    IO number (1–12)
 * @param out   Pointer to DioState to fill
 * @return true if io is valid
 */
bool dio_get_state(uint8_t io, DioState *out);

/**
 * @brief  Get the state array for all 12 IOs.
 *         Index 0 = IO 1, index 11 = IO 12.
 */
const DioState* dio_get_all(void);

/**
 * @brief  Read all input pins and update cached input_level fields.
 *         Call periodically (e.g. from a timer) for polled input reads.
 */
void dio_poll_inputs(void);

#ifdef __cplusplus
}
#endif
