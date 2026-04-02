#pragma once

// =============================================================================
// bb_hvpak.h — Renesas HVPAK level translator driver
//
// Controls the HVPAK via I2C to set I/O voltage for EXP_EXT level translation.
// TODO: Replace stub I2C register writes with actual HVPAK register map
//       once the specific HVPAK part number and datasheet are available.
// =============================================================================

#include <stdint.h>
#include <stdbool.h>

void bb_hvpak_init(void);
bool bb_hvpak_set_voltage(uint16_t mv);
uint16_t bb_hvpak_get_voltage(void);
bool bb_hvpak_is_ready(void);
