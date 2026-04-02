#pragma once

// =============================================================================
// bb_pins.h — EXP_EXT pin configuration management
// =============================================================================

#include <stdint.h>
#include <stdbool.h>

void bb_pins_init(void);
void bb_pins_set(uint8_t pin, uint8_t function);
void bb_pins_set_all(const uint8_t functions[4]);
void bb_pins_get_all(uint8_t functions[4]);
void bb_pins_reset(void);
const char *bb_pins_func_name(uint8_t function);
