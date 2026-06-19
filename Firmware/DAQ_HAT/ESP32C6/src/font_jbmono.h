#pragma once

// Baked JetBrains Mono digit glyphs (see tools/gen_font.py). Each glyph is an
// 8-bit alpha bitmap; the firmware uploads them into sprites at boot and blits
// them for the measurement readout — no per-frame vector rendering.

#include <stdint.h>

typedef struct {
    int            w, h;     // glyph bitmap dimensions
    const uint8_t *alpha;    // w*h coverage, row-major
} jb_glyph_t;

// Glyph table order: '0'..'9', '.', '-', ' '.
extern const jb_glyph_t gfx_jbmono[];
extern const int gfx_jbmono_count;
extern const int gfx_jbmono_cell_h;
extern const int gfx_jbmono_advance;   // monospace advance (cell width)

// Map an ASCII char to a glyph index, or -1 if unsupported.
int gfx_jbmono_index(char c);
