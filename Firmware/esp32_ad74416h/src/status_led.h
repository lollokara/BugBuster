#pragma once

// =============================================================================
// status_led.h - WS2812B Status LED Driver (3 LEDs on GPIO0)
//
// Three RGB LEDs chained on the LED_DIN line (GPIO0):
//
//   LED 0 — ESP32 / Connection Status
//     Blue:   USB or WiFi client connected (BBP binary or HTTP active)
//     Yellow: Booting / connecting
//     Green:  Operative but no client connected
//     Red:    Fault (unrecoverable error)
//
//   LED 1 — MUX & IO Expander Status
//     Green:  All OK (ADGS2414D + PCA9535 healthy)
//     Yellow: Not configured (init pending or no PCA9535)
//     Red:    Fault (MUX write-verify failed or PCA9535 error)
//
//   LED 2 — ADC (AD74416H) Status
//     Green:  Operative (SPI healthy, channels configured)
//     Yellow: Not configured (all channels HIGH_IMP)
//     Red:    Fault (SPI failure or alert active)
//
// Color scheme defined with standard GRB byte order for WS2812B.
// =============================================================================

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// LED indices
#define LED_ESP     0   // ESP32 / connection status
#define LED_MUX     1   // MUX + IO expander status
#define LED_ADC     2   // AD74416H ADC status
#define LED_COUNT   3

// Predefined colors (R, G, B — low brightness to avoid blinding)
#define LED_OFF         0, 0, 0
#define LED_RED         40, 0, 0
#define LED_GREEN       0, 40, 0
#define LED_BLUE        0, 0, 40
#define LED_YELLOW      30, 30, 0
#define LED_WHITE       20, 20, 20

/**
 * @brief  Initialize the WS2812B LED strip on GPIO0 using the RMT peripheral.
 *         All LEDs start as YELLOW (booting).
 */
void status_led_init(void);

/**
 * @brief  Set a single LED to an RGB color.
 * @param  index  LED index (0–2): LED_ESP, LED_MUX, LED_ADC
 * @param  r, g, b  Color values (0–255)
 */
void status_led_set(uint8_t index, uint8_t r, uint8_t g, uint8_t b);

/**
 * @brief  Push current LED colors to the hardware (refresh the strip).
 *         Call after one or more status_led_set() calls.
 */
void status_led_refresh(void);

/**
 * @brief  Convenience: set + refresh a single LED in one call.
 */
void status_led_set_now(uint8_t index, uint8_t r, uint8_t g, uint8_t b);

/**
 * @brief  Update all 3 LEDs based on current system state.
 *         Call periodically (e.g. every 500 ms from the main loop or a task).
 *         Reads connection status, MUX fault flag, ADC SPI health, etc.
 */
void status_led_update(void);

/**
 * @brief  Run one step of the boot breathing animation.
 *         All 3 LEDs breathe yellow together in a smooth sine wave.
 *         Call in a tight loop during boot with ~10ms delay between calls.
 *         Non-blocking — each call takes <1ms.
 */
void status_led_breathe_step(void);

#ifdef __cplusplus
}
#endif
