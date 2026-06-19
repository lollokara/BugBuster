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

// Push the whole framebuffer to the panel.
void display_flush(void);

// Block until the most recently flushed frame has finished transferring.
void display_wait_flush(void);

// Push a single dirty rectangle (inclusive bounds clamped internally).
void display_flush_rect(int x, int y, int w, int h);

// Backlight brightness, 0..255 (LEDC PWM).
void display_set_backlight(uint8_t level);
