#pragma once

// =============================================================================
// bb_pins.h — EXP_EXT pin configuration management
// =============================================================================

#include <stdint.h>
#include <stdbool.h>

void bb_pins_init(void);
// Returns true on success, false if the pin index is out of range or the
// function code is reserved / invalid. Function codes 1-4 (SWDIO/SWCLK/
// TRACE1/TRACE2) are reserved for wire-protocol compatibility and no
// longer assignable on the EXP_EXT set — the new PCB has a dedicated
// 3-pin SWD connector.
bool bb_pins_set(uint8_t pin, uint8_t function);
void bb_pins_set_all(const uint8_t functions[4]);
void bb_pins_get_all(uint8_t functions[4]);
void bb_pins_reset(void);
const char *bb_pins_func_name(uint8_t function);
