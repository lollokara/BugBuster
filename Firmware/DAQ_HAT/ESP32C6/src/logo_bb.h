#pragma once

// The real BugBuster app icon (DesktopApp/BugBuster/src-tauri/icons/icon.png),
// downscaled and composited over black by a one-off conversion. Logical RGB565,
// row-major; draw with gfx_pixel(). Used by the boot / wake splash.

#include <stdint.h>

#define LOGO_BB_W 60
#define LOGO_BB_H 60

extern const uint16_t LOGO_BB_PIXELS[LOGO_BB_W * LOGO_BB_H];
