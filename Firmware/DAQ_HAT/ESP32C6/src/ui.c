#include "ui.h"
#include "gfx.h"
#include "format.h"
#include "config.h"
#include "ddp_proto.h"
#include "perf.h"
#include "theme.h"

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

static float   s_v = 0.0f, s_i = 0.0f;
static uint8_t s_flags = 0;
static uint8_t s_state = DDP_STATE_BOOT;

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
static bool s_sprites_ready = false;

static void build_sprites(void)
{
    if (!gfx_sprite_alloc(&s_bolt, BOLT_CELL_W, BOLT_CELL_H, C_CYAN)) return;
    gfx_sprite_bolt(&s_bolt, BOLT_CX, BOLT_CY, BOLT_H);

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
    if (src_on) {
        gfx_fill_circle(src_badge_x, 9, 2.2f, C_GREEN);
        gfx_text(src_badge_x + 5, 6, "SRC", 1, C_GREEN);
    }
    gfx_fill_circle(DISP_WIDTH - tw - 14, 9, 2.2f, sc);
    gfx_text(DISP_WIDTH - tw - 8, 6, st, 1, sc);
}

// ---- A single hero value card ----------------------------------------------
static void draw_card(int x, int y, int w, int h, const char *label,
                      uint16_t accent, float value, char base_unit, bool over)
{
    if (over) accent = C_ROSE;

    gfx_round_rect(x, y, w, h, 8, C_CARD_B);
    gfx_round_rect_border(x, y, w, h, 8, C_BORDER);

    fmt_value_t fv;
    fmt_si(value, base_unit, &fv);

    char num[14];
    if (fv.negative) snprintf(num, sizeof(num), "-%s", fv.mantissa);
    else             snprintf(num, sizeof(num), "%s", fv.mantissa);

    const char *us = over ? "OVER" : fv.unit;

    // Top row: label on the left, unit on the right — keeps them clear of the
    // big number entirely.
    gfx_text(x + 8, y + 5, label, 1, accent);
    int uw = gfx_text_w(us, 1);
    gfx_text(x + w - uw - 8, y + 5, us, 1, over ? C_ROSE : C_DIM);
    gfx_hline(x + 8, y + 14, w - 16, C_BORDER);   // thin divider under the row

    // Big baked-font number fills the band below the divider, centered.
    int numw = gfx_jbtext_w(num);
    int numh = gfx_jbtext_h();
    int band_top = y + 16;
    int band_bot = y + h - 3;
    int nx = x + (w - numw) / 2;
    if (nx < x + 3) nx = x + 3;                    // never clip the left edge
    int ny = band_top + ((band_bot - band_top) - numh) / 2;
    gfx_jbtext(nx, ny, num, over ? C_ROSE : C_TEXT);
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

    draw_card(x0, top, cardw, cardh, "VOLTAGE", C_BLUE,
              s_v, 'V', (s_flags & DDP_FLAG_V_OVERRANGE) != 0);
    PERF_MARK("cardV");

    draw_card(x1, top, cardw, cardh, "CURRENT", C_GREEN,
              s_i, 'A', (s_flags & DDP_FLAG_I_OVERRANGE) != 0);
    PERF_MARK("cardI");
}
