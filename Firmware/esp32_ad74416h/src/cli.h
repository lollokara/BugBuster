#pragma once

#include <Arduino.h>
#include "ad74416h.h"
#include "tasks.h"

// =============================================================================
// cli.h - Serial CLI for AD74416H diagnostic testing
//
// Provides an interactive serial menu for testing all AD74416H functions
// including ADC diagnostics, DAC output, digital I/O, and fault management.
// =============================================================================

/**
 * @brief Initialise the CLI module with a reference to the device HAL.
 *        Must be called after device.begin() in setup().
 */
void cliInit(AD74416H& device);

/**
 * @brief Process any pending serial input. Non-blocking.
 *        Call from loop() on every iteration.
 */
void cliProcess();
