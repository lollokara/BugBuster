#pragma once

// =============================================================================
// daq_led_codes.h — shared channel-status LED colour scheme.
//
// Mirrors the RP2040 HAT's WS2812 colour-code table so the DAQ HAT's neopixels
// indicate channel status with the SAME colours as the main BugBuster HAT LEDs.
// The ESP32-S3 already computes a per-channel colour CODE (fault/supply/io) for
// the mainboard IOBLOCKs; for the DAQ HAT it sends the 4 channel codes down to
// the C6, which renders each on a PAIR of neopixels (8 LEDs = 4 channels x 2).
//
//   Code legend (matches RP2040 bb_hat_v2.c get_rgb_from_color_code):
//     0 = Off      (no supply, no IO)
//     1 = Red      (EFUSE fault)
//     2 = Green    (supply present + IO/MUX configured)
//     3 = Blue     (supply present, no IO)
//     4 = Yellow   (IO configured, no supply)
//     5 = Cyan
//     6 = Magenta
//     7 = White
//   else = Orange  (alert / unknown)
// =============================================================================

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Decode an LED colour code to 8-bit RGB. Always defined for any input.
static inline void daq_led_code_rgb(uint8_t code, uint8_t *r, uint8_t *g, uint8_t *b)
{
    uint8_t cr, cg, cb;
    switch (code) {
        case 0: cr = 0;   cg = 0;   cb = 0;   break;  // Off
        case 1: cr = 255; cg = 0;   cb = 0;   break;  // Red (fault)
        case 2: cr = 0;   cg = 255; cb = 0;   break;  // Green (supply+io)
        case 3: cr = 0;   cg = 0;   cb = 255; break;  // Blue (supply)
        case 4: cr = 255; cg = 255; cb = 0;   break;  // Yellow (io)
        case 5: cr = 0;   cg = 255; cb = 255; break;  // Cyan
        case 6: cr = 255; cg = 0;   cb = 255; break;  // Magenta
        case 7: cr = 255; cg = 255; cb = 255; break;  // White
        default: cr = 255; cg = 127; cb = 0;  break;  // Orange (alert)
    }
    if (r) *r = cr;
    if (g) *g = cg;
    if (b) *b = cb;
}

#ifdef __cplusplus
}
#endif
