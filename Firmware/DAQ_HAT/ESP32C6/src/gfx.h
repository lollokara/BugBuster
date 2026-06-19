#pragma once

// Minimal framebuffer graphics for the DAQ HAT UI.
// Ports the drawing-primitive spirit of the ER-TFTM2.25-1 ERGFX library to a
// RAM RGB565 framebuffer, and adds the niceties needed for a modern UI:
// vertical gradients, alpha blending, anti-aliased circles/wedges and rounded
// rectangles. The framebuffer is stored in panel wire order (byte-swapped
// RGB565); all helpers below take/return *logical* colors and handle the swap.

#include <stdint.h>
#include <stdbool.h>

// ---- Color -----------------------------------------------------------------
// Logical RGB565 (R high). Use these everywhere; storage swap is internal.
static inline uint16_t gfx_rgb(uint8_t r, uint8_t g, uint8_t b)
{
    return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

// A few named colors (logical).
#define GFX_BLACK   gfx_rgb(0,0,0)
#define GFX_WHITE   gfx_rgb(255,255,255)

void     gfx_init(uint16_t *framebuffer, int w, int h);

// Whole-screen helpers.
void     gfx_clear(uint16_t color);
void     gfx_gradient_v(uint16_t top, uint16_t bottom); // vertical gradient fill

// Pixels.
void     gfx_pixel(int x, int y, uint16_t color);
void     gfx_blend(int x, int y, uint16_t color, uint8_t a); // a: 0..255 coverage

// Rectangles.
void     gfx_fill_rect(int x, int y, int w, int h, uint16_t color);
void     gfx_hline(int x, int y, int w, uint16_t color);
void     gfx_vline(int x, int y, int h, uint16_t color);
void     gfx_rect(int x, int y, int w, int h, uint16_t color);
void     gfx_round_rect(int x, int y, int w, int h, int r, uint16_t color);       // filled, AA corners
void     gfx_round_rect_border(int x, int y, int w, int h, int r, uint16_t color); // outline
void     gfx_round_rect_grad(int x, int y, int w, int h, int r,
                             uint16_t top, uint16_t bottom);                       // filled gradient

// Circles / shapes (anti-aliased).
void     gfx_fill_circle(float cx, float cy, float radius, uint16_t color);
// Anti-aliased thick line with rounded caps (distance-field rendered).
void     gfx_thick_line(float x0, float y0, float x1, float y1, float thick, uint16_t color);
// Anti-aliased circular arc with rounded caps. Angles in degrees, 0 = +x,
// increasing clockwise (screen y is down). a1 may be < or > a0.
void     gfx_arc(float cx, float cy, float r, float a0_deg, float a1_deg, float thick, uint16_t color);
// Filled Pac-Man: circle of `radius` with a wedge mouth removed. mouth_deg is
// the half-angle of the mouth; facing_deg is the direction the mouth points.
void     gfx_pacman(float cx, float cy, float radius, float mouth_deg,
                    float facing_deg, uint16_t color);
// A small lightning-bolt glyph centered at (cx,cy), `h` tall.
void     gfx_bolt(float cx, float cy, float h, uint16_t color);

// ---- Sprites ---------------------------------------------------------------
// A cached, pre-rasterized single-color shape with a per-pixel coverage
// (alpha) mask. Rasterize the expensive anti-aliased shape ONCE into a sprite,
// then blit it cheaply (just an alpha blend, no transcendentals) at any
// position every frame. Used for the scrolling lightning bolts and the
// chomping Pac-Man so the header costs blits instead of full re-rasterization.
typedef struct {
    int      w, h;
    uint16_t color;   // logical RGB565 solid color
    uint8_t *alpha;   // w*h coverage buffer (heap allocated)
} gfx_sprite_t;

bool gfx_sprite_alloc(gfx_sprite_t *sp, int w, int h, uint16_t color);
void gfx_sprite_clear(gfx_sprite_t *sp);
// Rasterize helpers (slow path, intended to run once at init).
void gfx_sprite_pacman(gfx_sprite_t *sp, float cx, float cy, float r,
                       float mouth_deg, float facing_deg);
void gfx_sprite_bolt(gfx_sprite_t *sp, float cx, float cy, float h);
void gfx_sprite_punch_circle(gfx_sprite_t *sp, float cx, float cy, float r); // zero alpha
// Blit a sprite at (x,y). `gain` (0..255) scales the stored alpha for fading.
void gfx_blit(const gfx_sprite_t *sp, int x, int y, uint8_t gain);

// ---- Baked JetBrains Mono readout font -------------------------------------
// Renders digits/'.'/'-'/' ' using the pre-rasterized glyph bitmaps
// (font_jbmono.c, generated from the desktop app's JetBrains Mono). Glyphs are
// alpha-blended straight from flash — no per-frame vector math.
void gfx_jbtext(int x, int y, const char *s, uint16_t color);
int  gfx_jbtext_w(const char *s);
int  gfx_jbtext_h(void);

// ---- Text ------------------------------------------------------------------
// Small 5x7 font (ported glcdfont), integer-scaled.
void     gfx_text(int x, int y, const char *s, uint8_t size, uint16_t color);
int      gfx_text_w(const char *s, uint8_t size);   // pixel width
#define  GFX_SMALL_H(size) (7 * (size))

// Big "JetBrains-Mono-ish" readout font for the hero numbers, rendered with
// anti-aliased vector strokes (see font_big.c). Supports 0-9, '.', '-', ' '.
// `scale` multiplies the nominal BIGFONT_W x BIGFONT_H cell.
void     gfx_bigtext(int x, int y, const char *s, float scale, uint16_t color);
int      gfx_bigtext_w(const char *s, float scale);
#define  BIGFONT_W  16
#define  BIGFONT_H  28
