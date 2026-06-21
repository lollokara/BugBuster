#pragma once

// =============================================================================
// npx.h — WS2812 neopixel driver for the DAQ HAT display board (ESP32-C6).
//
// 8x WS2812 on NPX_PIN (IO15), driven by the RMT TX peripheral with a bytes
// encoder. A low-priority render task animates the chain from g_settings
// (npx_mode / npx_color / npx_brightness), so changing the LED settings from
// the menu or the app takes effect live.
// =============================================================================
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

// Initialise the RMT channel + encoder and start the render task. Safe to call
// once at boot after settings_init().
void npx_init(void);

// Set the 4 channel-status colour codes (front 4-connector). In DAQ_NPX_CHANNEL
// mode each code drives a PAIR of neopixels. Codes use the shared scheme in
// common/daq_led_codes.h (0=off,1=red,2=green,3=blue,4=yellow,...). Called from
// the DDP RX path when the P4 relays the S3 channel status.
void npx_set_channel_codes(const uint8_t codes[4]);

#ifdef __cplusplus
}
#endif
