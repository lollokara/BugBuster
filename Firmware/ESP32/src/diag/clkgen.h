#pragma once

// =============================================================================
// clkgen.h - Bench clock generator: drive a square wave on any of the 12 IOs.
//
// CLI-only feature. Owns all peripheral state (LEDC or MCPWM) and the MUX
// switch needed to route the GPIO signal to the chosen IO terminal.
// No BBP / desktop / web surface — bench / characterisation use only.
// =============================================================================

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum { CLKSRC_LEDC = 0, CLKSRC_MCPWM = 1 } ClkSrc;

void clkgen_init(void);
bool clkgen_start(uint8_t io, ClkSrc src, uint32_t hz, char* err, size_t errlen);
bool clkgen_stop(void);
bool clkgen_status(bool* active, uint8_t* io, int* gpio,
                   ClkSrc* src, uint32_t* req_hz, uint32_t* actual_hz);

#ifdef __cplusplus
}
#endif
