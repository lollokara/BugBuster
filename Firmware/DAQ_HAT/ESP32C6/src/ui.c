#include "ui.h"
#include "gfx.h"
#include "format.h"
#include "config.h"
#include "ddp_proto.h"
#include "ddp.h"
#include "perf.h"
#include "theme.h"
#include "settings.h"

#include <math.h>
#include <string.h>
#include <stdio.h>

// ---- Theme (runtime, see theme.c). The macros keep call sites unchanged. ----
#define C_BG0     (g_theme.bg)
#define C_BG1     (g_theme.bg1)
#define C_CARD_T  (g_theme.card_hi)
#define C_CARD_B  (g_theme.card)
#define C_BORDER  (g_theme.border)
#define C_BLUE    (g_theme.blue)
#define C_GREEN   (g_theme.green)
#define C_AMBER   (g_theme.amber)
#define C_CYAN    (g_theme.cyan)
#define C_ROSE    (g_theme.rose)
#define C_TEXT    (g_theme.text)
#define C_DIM     (g_theme.dim)
#define C_MUTED   (g_theme.muted)
#define C_HEADER  (g_theme.header)

// Linear blend of two logical RGB565 colors. t=0 -> a, t=255 -> b. Used to fade
// an accent toward the background for the soft tile glow.
static uint16_t mix565(uint16_t a, uint16_t b, uint8_t t)
{
    int ar = (a >> 11) & 0x1F, ag = (a >> 5) & 0x3F, ab = a & 0x1F;
    int br = (b >> 11) & 0x1F, bg = (b >> 5) & 0x3F, bb = b & 0x1F;
    int r = ar + ((br - ar) * t) / 255;
    int g = ag + ((bg - ag) * t) / 255;
    int bl = ab + ((bb - ab) * t) / 255;
    return (uint16_t)((r << 11) | (g << 5) | bl);
}

// Small filled up-triangle (apex at top), used as the range marker.
static void draw_tri(int cx, int y, int s, uint16_t c)
{
    for (int r = 0; r <= s; r++) gfx_hline(cx - r, y + r, 2 * r + 1, c);
}

static float   s_v = 0.0f, s_i = 0.0f;
static uint8_t s_flags = 0;
static uint8_t s_state = DDP_STATE_BOOT;

// Transient warning banner (e.g. USB-PD guard). Shown for a few render frames.
static char     s_warn[40] = {0};
static uint16_t s_warn_ttl = 0;

// ---- Cached header sprites (rasterized once) -------------------------------
// Pac-Man geometry inside its sprite cell.
#define PAC_R       7.0f
#define PAC_CELL    18
#define PAC_CX      9.0f
#define PAC_CY      9.0f
#define PAC_FRAMES  6           // chomp animation steps

// Bolt sprite cell.
#define BOLT_CELL_W 10
#define BOLT_CELL_H 16
#define BOLT_CX     5.0f
#define BOLT_CY     8.0f
#define BOLT_H      11.0f

static gfx_sprite_t s_pac[PAC_FRAMES];
static gfx_sprite_t s_bolt;
static gfx_sprite_t s_dot;              // pre-rendered status dot (tinted per blit)
static bool s_sprites_ready = false;

// Cell for the shared status dot. Odd size so the AA circle centers cleanly;
// blit offset is -DOT_HALF so ui_draw_dot() takes the visual center.
#define DOT_CELL 9
#define DOT_HALF 4

static void build_sprites(void)
{
    if (!gfx_sprite_alloc(&s_bolt, BOLT_CELL_W, BOLT_CELL_H, C_CYAN)) return;
    gfx_sprite_bolt(&s_bolt, BOLT_CX, BOLT_CY, BOLT_H);

    if (!gfx_sprite_alloc(&s_dot, DOT_CELL, DOT_CELL, C_GREEN)) return;
    gfx_sprite_circle(&s_dot, 4.5f, 4.5f, 2.4f);

    for (int f = 0; f < PAC_FRAMES; f++) {
        if (!gfx_sprite_alloc(&s_pac[f], PAC_CELL, PAC_CELL, C_AMBER)) return;
        float mouth = 4.0f + 26.0f * ((float)f / (PAC_FRAMES - 1)); // 4..30 deg
        gfx_sprite_pacman(&s_pac[f], PAC_CX, PAC_CY, PAC_R, mouth, 0.0f);
        gfx_sprite_punch_circle(&s_pac[f], PAC_CX + 0.5f, PAC_CY - 3.4f, 1.2f); // eye
    }
    s_sprites_ready = true;
}

void ui_init(void)
{
    s_v = 0; s_i = 0; s_flags = 0; s_state = DDP_STATE_BOOT;
    build_sprites();
}

// Re-tint the cached header sprites after a theme switch (their color is baked
// at allocation time).
void ui_refresh_theme(void)
{
    s_bolt.color = C_CYAN;
    for (int f = 0; f < PAC_FRAMES; f++) s_pac[f].color = C_AMBER;
}

void ui_set_data(float v, float i, uint8_t flags, uint8_t state)
{
    s_v = v; s_i = i; s_flags = flags; s_state = state;
}

bool ui_source_on(void)
{
    return (s_flags & DDP_FLAG_SRC_ON) != 0;
}

void ui_show_warning(const char *msg)
{
    if (!msg) return;
    strncpy(s_warn, msg, sizeof(s_warn) - 1);
    s_warn[sizeof(s_warn) - 1] = '\0';
    s_warn_ttl = 180;   // ~ a few seconds of render frames
}

// Blit the pre-rendered status dot centered at (cx,cy) in `color`. Cheap alpha
// blend — no per-frame anti-aliased circle math (the C6 has no FPU).
void ui_draw_dot(int cx, int cy, uint16_t color)
{
    if (!s_sprites_ready) return;
    s_dot.color = color;
    gfx_blit(&s_dot, cx - DOT_HALF, cy - DOT_HALF, 255);
}

// ---- Header: Pac-Man chomping a stream of lightning bolts -------------------
// Slim landscape top bar spanning the full width. The Pac-Man and bolts are
// pre-rasterized sprites (see build_sprites); each frame we only redraw the
// solid bar and BLIT the cached pixels at their new positions — no per-pixel
// shape math at runtime. Motion is driven by a frame counter so it advances a
// fixed ~0.5 px per frame regardless of frame rate.
static void draw_header(uint32_t t_ms)
{
    (void)t_ms;
    static float s_scroll = 0.0f;   // pixels, advances each frame
    static uint32_t s_fc = 0;       // frame counter for the chomp
    s_scroll += 0.8f;
    s_fc++;

    const int H = 18;
    gfx_fill_rect(0, 0, DISP_WIDTH, H, C_HEADER);
    gfx_hline(0, H, DISP_WIDTH, C_BORDER);

    float cx = 12.0f, cy = 9.0f, r = 7.0f;

    // Status pill width (reserve space on the right so bolts never overlap it).
    uint16_t sc; const char *st;
    switch (s_state) {
        case DDP_STATE_LIVE: sc = C_GREEN; st = "LIVE"; break;
        case DDP_STATE_SIM:  sc = C_AMBER; st = "SIM";  break;
        case DDP_STATE_FAULT:sc = C_ROSE;  st = "FLT";  break;
        default:             sc = C_MUTED; st = "...";  break;
    }
    int tw = gfx_text_w(st, 1);
    int pill_left = DISP_WIDTH - tw - 18;   // left edge of the status pill

    // Supply-active badge: a green "SRC" pill just left of the status pill,
    // shown while the DUT supply (SMU) output is on. Reserve its width first so
    // the scrolling bolts never overlap it.
    bool src_on = (s_flags & DDP_FLAG_SRC_ON) != 0;
    int src_badge_x = 0, src_tw = 0;
    if (src_on) {
        src_tw = gfx_text_w("SRC", 1);
        src_badge_x = pill_left - src_tw - 9;   // dot + gap allowance
        pill_left = src_badge_x - 4;            // bolts stop left of the badge
    }

    // Super-Resolution badge: cyan "SR" pill shown while the P4 is running the
    // oversampled 1 ksps / 500 sps acquisition, so the low waveform rate on the
    // tiles is never mistaken for a stalled stream.
    bool sr_on = g_settings.sr_mode;
    int sr_badge_x = 0, sr_tw = 0;
    if (sr_on) {
        sr_tw = gfx_text_w("SR", 1);
        sr_badge_x = pill_left - sr_tw - 9;
        pill_left = sr_badge_x - 4;
    }

    // Temperature indicator: highest of the two AD7415 board sensors, shown as
    // a small readout in the header. Reserving its width here shortens the bolt
    // travel, trading Pac-Man animation space for the temp readout.
    char temp_s[12];
    int  temp_x = 0, temp_w = 0;
    bool temp_valid = false;
    {
        ddp_diag_t dg; uint32_t age;
        if (ddp_get_diag(&dg, &age) && age < 3000) {
            int16_t t0 = dg.t_board0_c10, t1 = dg.t_board1_c10;
            bool v0 = (dg.valid & DDP_DIAG_V_BOARD0) && t0 != DDP_DIAG_TEMP_NA;
            bool v1 = (dg.valid & DDP_DIAG_V_BOARD1) && t1 != DDP_DIAG_TEMP_NA;
            int16_t tmax = 0;
            if (v0 && v1)  { tmax = (t0 > t1) ? t0 : t1; temp_valid = true; }
            else if (v0)   { tmax = t0; temp_valid = true; }
            else if (v1)   { tmax = t1; temp_valid = true; }
            if (temp_valid) {
                int t = tmax, neg = (t < 0); if (neg) t = -t;
                snprintf(temp_s, sizeof(temp_s), "%s%d.%dC", neg ? "-" : "",
                         t / 10, t % 10);
            }
        }
    }
    if (temp_valid) {
        temp_w = gfx_text_w(temp_s, 1);
        temp_x = pill_left - temp_w - 6;
        pill_left = temp_x - 4;                 // bolts stop left of the readout
    }

    // Bolts scroll left toward the mouth, then get "eaten" (alpha fade). They
    // spawn just left of the status pill so they never draw over it.
    const int N = 5;
    float xSpawn = pill_left - 4.0f;
    float xMouth = cx + r - 1.0f;
    float travel = xSpawn - xMouth;
    float spacing = travel / N;
    for (int k = 0; k < N; k++) {
        float d = fmodf(s_scroll + k * spacing, travel);
        float bx = xSpawn - d;                   // moves right -> left
        float p = d / travel;                    // 0 at spawn .. 1 at mouth
        float fade = 1.0f;
        if (p > 0.85f) fade = (1.0f - p) / 0.15f;
        if (fade < 0) fade = 0;
        if (fade > 0.10f && s_sprites_ready) {
            uint8_t gain = (uint8_t)(255 * fade);
            gfx_blit(&s_bolt, (int)(bx - BOLT_CX), (int)(cy - BOLT_CY), gain);
        }
    }

    // Pac-Man: pick the chomp frame from a triangle wave and blit it. The
    // chomp rate is independent of the bolt scroll speed.
    if (s_sprites_ready) {
        float chomp = fabsf(sinf((float)s_fc * 0.28f));       // 0..1
        int f = (int)(chomp * (PAC_FRAMES - 1) + 0.5f);
        if (f < 0) f = 0;
        if (f > PAC_FRAMES - 1) f = PAC_FRAMES - 1;
        gfx_blit(&s_pac[f], (int)(cx - PAC_CX), (int)(cy - PAC_CY), 255);
    }

    // Status pill (drawn last so it's always on top of the bolt stream).
    if (temp_valid) {
        gfx_text(temp_x, 6, temp_s, 1, C_DIM);
    }
    if (sr_on) {
        ui_draw_dot(sr_badge_x, 9, C_CYAN);
        gfx_text(sr_badge_x + 5, 6, "SR", 1, C_CYAN);
    }
    if (src_on) {
        ui_draw_dot(src_badge_x, 9, C_GREEN);
        gfx_text(src_badge_x + 5, 6, "SRC", 1, C_GREEN);
    }
    ui_draw_dot(DISP_WIDTH - tw - 14, 9, sc);
    gfx_text(DISP_WIDTH - tw - 8, 6, st, 1, sc);
}

// ---- A single hero value card ----------------------------------------------
// glow: draw a soft accent ring outside the tile (supply on).
// show_off: render "OFF" instead of the value (supply off).
// range_badge: optional short current-range label (triangle + text) in the top row.
static void draw_card(int x, int y, int w, int h, const char *label,
                      uint16_t accent, float value, char base_unit, bool over,
                      bool glow, bool show_off, const char *range_badge)
{
    if (over) accent = C_ROSE;

    // Optional outer glow ring, brightest next to the tile and fading outward.
    if (glow) {
        uint16_t g_in  = mix565(accent, C_BG0, 110);
        uint16_t g_out = mix565(accent, C_BG0, 185);
        gfx_round_rect_border(x - 2, y - 2, w + 4, h + 4, 10, g_out);
        gfx_round_rect_border(x - 1, y - 1, w + 2, h + 2, 9,  g_in);
    }

    gfx_round_rect(x, y, w, h, 8, C_CARD_B);
    gfx_round_rect_border(x, y, w, h, 8, C_BORDER);

    char num[14];
    const char *us;
    uint16_t num_color;
    if (show_off) {
        // No baked-font glyphs for letters, so "OFF" uses the 5x7 font below.
        us = "";
        num_color = C_MUTED;
        num[0] = '\0';
    } else {
        fmt_value_t fv;
        fmt_si(value, base_unit, &fv);
        if (fv.negative) snprintf(num, sizeof(num), "-%s", fv.mantissa);
        else             snprintf(num, sizeof(num), "%s", fv.mantissa);
        us = over ? "OVER" : fv.unit;
        num_color = over ? C_ROSE : C_TEXT;
    }

    // Top row: label on the left, unit on the right — keeps them clear of the
    // big number entirely.
    gfx_text(x + 8, y + 5, label, 1, accent);
    if (us[0]) {
        int uw = gfx_text_w(us, 1);
        gfx_text(x + w - uw - 8, y + 5, us, 1, over ? C_ROSE : C_DIM);
    }
    // Current range marker (triangle + shunt label), tucked left of the unit.
    if (range_badge && range_badge[0]) {
        int uw = us[0] ? gfx_text_w(us, 1) : 0;
        int unit_x = x + w - uw - 8;
        int rbw = gfx_text_w(range_badge, 1);
        int tx = unit_x - 5 - rbw;
        draw_tri(tx - 6, y + 6, 3, accent);
        gfx_text(tx, y + 5, range_badge, 1, accent);
    }
    gfx_hline(x + 8, y + 14, w - 16, C_BORDER);   // thin divider under the row

    int band_top = y + 16;
    int band_bot = y + h - 3;
    if (show_off) {
        // "OFF" rendered with the scalable 5x7 font, centered in the band.
        const int sz = 3;
        int tw = gfx_text_w("OFF", sz);
        int th = 7 * sz;
        int ox = x + (w - tw) / 2;
        int oy = band_top + ((band_bot - band_top) - th) / 2;
        gfx_text(ox, oy, "OFF", sz, num_color);
    } else {
        // Big baked-font number fills the band below the divider, centered.
        int numw = gfx_jbtext_w(num);
        int numh = gfx_jbtext_h();
        int nx = x + (w - numw) / 2;
        if (nx < x + 3) nx = x + 3;                // never clip the left edge
        int ny = band_top + ((band_bot - band_top) - numh) / 2;
        gfx_jbtext(nx, ny, num, num_color);
    }
}

void ui_render(uint32_t t_ms)
{
    PERF_FRAME_BEGIN();

    gfx_clear(C_BG0);
    PERF_MARK("bg");

    draw_header(t_ms);
    PERF_MARK("header");

    int top = 22;
    int cardh = DISP_HEIGHT - top - 3;     // ~51 px tall
    int gap = 6;
    int cardw = (DISP_WIDTH - gap - 8) / 2; // two side-by-side cards
    int x0 = 4;
    int x1 = x0 + cardw + gap;

    bool src_on = (s_flags & DDP_FLAG_SRC_ON) != 0;
    // Only blank the current readout to "OFF" for real (LIVE) data; the demo
    // sweep never asserts SRC_ON and should keep animating.
    bool cur_off = !src_on && s_state == DDP_STATE_LIVE;

    // Decode the live current range for the badge (LIVE data only).
    const char *rbadge = NULL;
    if (s_state == DDP_STATE_LIVE && !cur_off) {
        switch ((s_flags & DDP_FLAG_RANGE_MASK) >> DDP_FLAG_RANGE_SHIFT) {
            case DDP_RANGE_HI:  rbadge = "51R"; break;
            case DDP_RANGE_MID: rbadge = "2R";  break;
            case DDP_RANGE_LO:  rbadge = "50m"; break;
            default:            rbadge = NULL;  break;
        }
    }

    draw_card(x0, top, cardw, cardh, "VOLTAGE", C_BLUE,
              s_v, 'V', (s_flags & DDP_FLAG_V_OVERRANGE) != 0,
              src_on, false, NULL);
    PERF_MARK("cardV");

    draw_card(x1, top, cardw, cardh, "CURRENT", C_GREEN,
              s_i, 'A', (s_flags & DDP_FLAG_I_OVERRANGE) != 0,
              src_on, cur_off, rbadge);
    PERF_MARK("cardI");

    // Transient warning banner over the tiles (drawn last, on top).
    if (s_warn_ttl > 0) {
        s_warn_ttl--;
        int bh = 22, by = (DISP_HEIGHT - bh) / 2 + 6;
        gfx_round_rect(6, by, DISP_WIDTH - 12, bh, 5, C_ROSE);
        gfx_round_rect_border(6, by, DISP_WIDTH - 12, bh, 5, C_TEXT);
        int tw = gfx_text_w(s_warn, 1);
        gfx_text((DISP_WIDTH - tw) / 2, by + (bh - 7) / 2, s_warn, 1, C_BG0);
    }
}
