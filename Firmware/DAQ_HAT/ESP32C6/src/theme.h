#pragma once

// Runtime color theme. Two presets: the default "navy" palette (matches the
// desktop app) and a "neon dark" mode. Colors are RGB565 logical values
// (see gfx_rgb). UI code reads g_theme.* so a theme switch is instant.

#include <stdint.h>
#include <stdbool.h>

typedef struct {
    uint16_t bg, bg1, header, card, card_hi, border;
    uint16_t text, dim, muted;
    uint16_t blue, green, amber, rose, cyan, purple;
    // Menu-specific accents.
    uint16_t sel;          // carousel selection-box fill
    uint16_t sel_text;     // text drawn on the selection box
} theme_t;

extern theme_t g_theme;

void theme_init(void);            // load default palette
void theme_set_dark(bool dark);   // switch palette
bool theme_is_dark(void);
