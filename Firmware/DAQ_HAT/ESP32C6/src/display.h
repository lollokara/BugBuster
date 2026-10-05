#pragma once

// ST7789 panel driver (ESP-IDF port of the ER-TFTM2.25-1 Arduino example).
// Uses the IDF esp_lcd SPI panel driver with a full-frame RGB565 framebuffer
// kept in PSRAM/SRAM. The UI renders into the framebuffer (see gfx.h) and the
// whole frame is blit to the panel with display_flush().

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

// Initialise SPI bus, ST7789 panel and backlight. Allocates the framebuffer.
esp_err_t display_init(void);

// Pointer to the RGB565 framebuffer (DISP_WIDTH * DISP_HEIGHT pixels,
// row-major, big-endian on the wire is handled by the panel driver).
uint16_t *display_framebuffer(void);

// Push the whole framebuffer to the panel. True only when the frame was handed to the
// panel AND its transfer completed inside the bound (false while asleep).
bool display_flush(void);

// Block until the most recently flushed frame has finished transferring. False if it
// did not finish inside the bound.
bool display_wait_flush(void);

// Push a single dirty rectangle (inclusive bounds clamped internally).
void display_flush_rect(int x, int y, int w, int h);

// Backlight brightness, 0..255 (LEDC PWM). While the panel is asleep or the
// backlight is forced off the level is only remembered.
void display_set_backlight(uint8_t level);

// --- standby (see display_power.h for the ordering) --------------------------
// Every operation that changes what the glass shows returns whether the driver
// reported success: a caller must not claim "screen off" / "screen on" otherwise.
// Gate display_flush()/display_flush_rect(): while asleep they do nothing.
void display_set_asleep(bool asleep);
// Force the active-low backlight pin to its OFF level and stop the PWM. PWM at
// 255/256 is not off.
bool display_backlight_hard_off(void);
// Re-arm the PWM at @p level (the saved brightness) after a hard-off.
bool display_backlight_restore(uint8_t level);
// ST7789 DISPOFF / DISPON. Panel RAM stays writable while it is off.
bool display_panel_power(bool on);
