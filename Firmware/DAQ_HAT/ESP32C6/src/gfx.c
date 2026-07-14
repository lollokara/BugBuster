#include "gfx.h"
#include <math.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// Ported 5x7 font table (Adafruit/ER glcdfont), column-major, 5 bytes/glyph.
extern const unsigned char gfx_font5x7[];

static uint16_t *s_fb = NULL;
static int s_w = 0, s_h = 0;

static inline uint16_t swap16(uint16_t v) { return (uint16_t)((v << 8) | (v >> 8)); }

static inline void put(int x, int y, uint16_t logical)
{
    if ((unsigned)x >= (unsigned)s_w || (unsigned)y >= (unsigned)s_h) return;
    s_fb[y * s_w + x] = swap16(logical);
}

static inline void unpack(uint16_t logical, int *r, int *g, int *b)
{
    *r = (logical >> 8) & 0xF8;
    *g = (logical >> 3) & 0xFC;
    *b = (logical << 3) & 0xF8;
}

void gfx_init(uint16_t *framebuffer, int w, int h)
{
    s_fb = framebuffer;
    s_w = w;
    s_h = h;
}

void gfx_pixel(int x, int y, uint16_t color) { put(x, y, color); }

void gfx_blend(int x, int y, uint16_t color, uint8_t a)
{
    if ((unsigned)x >= (unsigned)s_w || (unsigned)y >= (unsigned)s_h) return;
    if (a == 0) return;
    if (a == 255) { put(x, y, color); return; }

    uint16_t dst = swap16(s_fb[y * s_w + x]);
    int sr, sg, sb, dr, dg, db;
    unpack(color, &sr, &sg, &sb);
    unpack(dst,   &dr, &dg, &db);
    int ia = 255 - a;
    int r = (sr * a + dr * ia) / 255;
    int g = (sg * a + dg * ia) / 255;
    int b = (sb * a + db * ia) / 255;
    s_fb[y * s_w + x] = swap16(gfx_rgb(r, g, b));
}

void gfx_clear(uint16_t color)
{
    uint16_t v = swap16(color);
    for (int i = 0; i < s_w * s_h; i++) s_fb[i] = v;
}

void gfx_gradient_v(uint16_t top, uint16_t bottom)
{
    int tr, tg, tb, br, bg, bb;
    unpack(top, &tr, &tg, &tb);
    unpack(bottom, &br, &bg, &bb);
    for (int y = 0; y < s_h; y++) {
        int r = tr + (br - tr) * y / (s_h - 1);
        int g = tg + (bg - tg) * y / (s_h - 1);
        int b = tb + (bb - tb) * y / (s_h - 1);
        uint16_t v = swap16(gfx_rgb(r, g, b));
        for (int x = 0; x < s_w; x++) s_fb[y * s_w + x] = v;
    }
}

void gfx_fill_rect(int x, int y, int w, int h, uint16_t color)
{
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > s_w) w = s_w - x;
    if (y + h > s_h) h = s_h - y;
    if (w <= 0 || h <= 0) return;
    uint16_t v = swap16(color);
    for (int yy = y; yy < y + h; yy++)
        for (int xx = x; xx < x + w; xx++)
            s_fb[yy * s_w + xx] = v;
}

void gfx_hline(int x, int y, int w, uint16_t color) { gfx_fill_rect(x, y, w, 1, color); }
void gfx_vline(int x, int y, int h, uint16_t color) { gfx_fill_rect(x, y, 1, h, color); }

void gfx_rect(int x, int y, int w, int h, uint16_t color)
{
    gfx_hline(x, y, w, color);
    gfx_hline(x, y + h - 1, w, color);
    gfx_vline(x, y, h, color);
    gfx_vline(x + w - 1, y, h, color);
}

// Coverage of a circle of radius r at pixel (px,py) center, super-sampled 4x4.
static uint8_t circle_cov(float px, float py, float cx, float cy, float r)
{
    int hits = 0;
    for (int sy = 0; sy < 4; sy++) {
        for (int sx = 0; sx < 4; sx++) {
            float dx = (px + (sx + 0.5f) / 4.0f) - cx;
            float dy = (py + (sy + 0.5f) / 4.0f) - cy;
            if (dx * dx + dy * dy <= r * r) hits++;
        }
    }
    return (uint8_t)(hits * 255 / 16);
}

void gfx_fill_circle(float cx, float cy, float r, uint16_t color)
{
    int x0 = (int)floorf(cx - r) - 1, x1 = (int)ceilf(cx + r) + 1;
    int y0 = (int)floorf(cy - r) - 1, y1 = (int)ceilf(cy + r) + 1;
    for (int y = y0; y <= y1; y++)
        for (int x = x0; x <= x1; x++) {
            uint8_t c = circle_cov(x, y, cx, cy, r);
            if (c) gfx_blend(x, y, color, c);
        }
}

// Coverage from a signed distance: 1px-wide smooth edge.
static inline uint8_t sd_cov(float dist, float half)
{
    float e = half - dist; // >0 inside
    if (e >= 0.5f) return 255;
    if (e <= -0.5f) return 0;
    return (uint8_t)((e + 0.5f) * 255.0f);
}

void gfx_thick_line(float x0, float y0, float x1, float y1, float thick, uint16_t color)
{
    float half = thick * 0.5f;
    float minx = (x0 < x1 ? x0 : x1) - half - 1;
    float maxx = (x0 > x1 ? x0 : x1) + half + 1;
    float miny = (y0 < y1 ? y0 : y1) - half - 1;
    float maxy = (y0 > y1 ? y0 : y1) + half + 1;
    float dx = x1 - x0, dy = y1 - y0;
    float len2 = dx * dx + dy * dy;
    for (int y = (int)floorf(miny); y <= (int)ceilf(maxy); y++) {
        for (int x = (int)floorf(minx); x <= (int)ceilf(maxx); x++) {
            float px = x + 0.5f - x0, py = y + 0.5f - y0;
            float t = len2 > 0 ? (px * dx + py * dy) / len2 : 0.0f;
            if (t < 0) t = 0; else if (t > 1) t = 1;
            float cxp = px - t * dx, cyp = py - t * dy;
            float d = sqrtf(cxp * cxp + cyp * cyp);
            uint8_t a = sd_cov(d, half);
            if (a) gfx_blend(x, y, color, a);
        }
    }
}

void gfx_arc(float cx, float cy, float r, float a0_deg, float a1_deg, float thick, uint16_t color)
{
    float half = thick * 0.5f;
    float a0 = a0_deg * (float)M_PI / 180.0f;
    float a1 = a1_deg * (float)M_PI / 180.0f;
    if (a1 < a0) { float t = a0; a0 = a1; a1 = t; }
    float outer = r + half + 1;
    float r0 = r - half - 0.5f; if (r0 < 0) r0 = 0;
    float r1 = r + half + 0.5f;
    float r0sq = r0 * r0, r1sq = r1 * r1;

    // Fast path: a full ring (>= ~359 deg) needs no angle test at all — just
    // the radial distance. atan2f is the single most expensive op on this
    // FPU-less core, so this saves a lot for '0','3','6','8','9'.
    bool full_ring = (a1 - a0) >= (2.0f * (float)M_PI - 0.02f);

    float e0x = cosf(a0) * r, e0y = sinf(a0) * r;
    float e1x = cosf(a1) * r, e1y = sinf(a1) * r;

    for (int y = (int)floorf(cy - outer); y <= (int)ceilf(cy + outer); y++) {
        for (int x = (int)floorf(cx - outer); x <= (int)ceilf(cx + outer); x++) {
            float px = x + 0.5f - cx, py = y + 0.5f - cy;
            float distsq = px * px + py * py;
            // Cheap reject: outside the ring band entirely.
            if (distsq > r1sq || distsq < r0sq) {
                if (full_ring) continue;
                // For partial arcs the rounded caps can stick out; fall through
                // only if near a cap endpoint.
            }
            float d;
            if (full_ring) {
                d = fabsf(sqrtf(distsq) - r);
            } else {
                float ang = atan2f(py, px);
                while (ang < a0) ang += 2.0f * (float)M_PI;
                if (ang <= a1) {
                    d = fabsf(sqrtf(distsq) - r);
                } else {
                    float d0 = sqrtf((px - e0x) * (px - e0x) + (py - e0y) * (py - e0y));
                    float d1 = sqrtf((px - e1x) * (px - e1x) + (py - e1y) * (py - e1y));
                    d = d0 < d1 ? d0 : d1;
                }
            }
            uint8_t a = sd_cov(d, half);
            if (a) gfx_blend(x, y, color, a);
        }
    }
}

void gfx_pacman(float cx, float cy, float r, float mouth_deg, float facing_deg, uint16_t color)
{
    // Wedge test without atan2f: a point is inside the mouth if it lies in the
    // forward half (dot with facing > 0) AND its angle from facing is < half.
    // cos(angle) = dot / |p|, so compare dot^2 vs cos^2(half) * |p|^2 with a
    // sign check — pure mults, no transcendentals in the inner loop.
    float facing = facing_deg * (float)M_PI / 180.0f;
    float half   = mouth_deg  * (float)M_PI / 180.0f;
    float fdx = cosf(facing), fdy = sinf(facing);
    float cosh = cosf(half);
    float cosh2 = cosh * cosh;
    float rsq = r * r;
    int x0 = (int)floorf(cx - r) - 1, x1 = (int)ceilf(cx + r) + 1;
    int y0 = (int)floorf(cy - r) - 1, y1 = (int)ceilf(cy + r) + 1;
    for (int y = y0; y <= y1; y++) {
        for (int x = x0; x <= x1; x++) {
            int hits = 0;
            for (int sy = 0; sy < 2; sy++) {
                for (int sx = 0; sx < 2; sx++) {
                    float dx = (x + (sx + 0.5f) / 2.0f) - cx;
                    float dy = (y + (sy + 0.5f) / 2.0f) - cy;
                    float dsq = dx * dx + dy * dy;
                    if (dsq > rsq) continue;
                    float dot = dx * fdx + dy * fdy;
                    if (dot > 0 && dot * dot > cosh2 * dsq) continue; // in mouth
                    hits++;
                }
            }
            if (hits) gfx_blend(x, y, color, (uint8_t)(hits * 255 / 4));
        }
    }
}

void gfx_bolt(float cx, float cy, float h, uint16_t color)
{
    // Simple 7-point lightning bolt polygon, scaled to height h.
    float s = h / 10.0f;
    // Polygon points (x,y) relative to center, roughly a zig-zag bolt.
    const float px[] = { 1.5f, -2.0f,  0.0f, -1.5f,  2.0f,  0.0f };
    const float py[] = { -5.0f, 1.0f,  1.0f,  5.0f, -1.0f, -1.0f };
    const int n = 6;
    float minx = 1e9f, maxx = -1e9f, miny = 1e9f, maxy = -1e9f;
    for (int i = 0; i < n; i++) {
        if (px[i] < minx) minx = px[i];
        if (px[i] > maxx) maxx = px[i];
        if (py[i] < miny) miny = py[i];
        if (py[i] > maxy) maxy = py[i];
    }
    int x0 = (int)floorf(cx + minx * s) - 1, x1 = (int)ceilf(cx + maxx * s) + 1;
    int y0 = (int)floorf(cy + miny * s) - 1, y1 = (int)ceilf(cy + maxy * s) + 1;
    for (int y = y0; y <= y1; y++) {
        for (int x = x0; x <= x1; x++) {
            int hits = 0;
            for (int sy = 0; sy < 3; sy++) {
                for (int sx = 0; sx < 3; sx++) {
                    float fx = (x + (sx + 0.5f) / 3.0f - cx) / s;
                    float fy = (y + (sy + 0.5f) / 3.0f - cy) / s;
                    // point-in-polygon
                    bool inside = false;
                    for (int i = 0, j = n - 1; i < n; j = i++) {
                        if (((py[i] > fy) != (py[j] > fy)) &&
                            (fx < (px[j] - px[i]) * (fy - py[i]) / (py[j] - py[i]) + px[i]))
                            inside = !inside;
                    }
                    if (inside) hits++;
                }
            }
            if (hits) gfx_blend(x, y, color, (uint8_t)(hits * 255 / 9));
        }
    }
}

// ---- Sprites ---------------------------------------------------------------
#include "esp_heap_caps.h"

bool gfx_sprite_alloc(gfx_sprite_t *sp, int w, int h, uint16_t color)
{
    sp->w = w; sp->h = h; sp->color = color;
    sp->alpha = heap_caps_calloc(1, (size_t)w * h, MALLOC_CAP_8BIT);
    return sp->alpha != NULL;
}

void gfx_sprite_clear(gfx_sprite_t *sp)
{
    if (sp->alpha) memset(sp->alpha, 0, (size_t)sp->w * sp->h);
}

static inline void sp_set(gfx_sprite_t *sp, int x, int y, uint8_t a)
{
    if ((unsigned)x >= (unsigned)sp->w || (unsigned)y >= (unsigned)sp->h) return;
    if (a > sp->alpha[y * sp->w + x]) sp->alpha[y * sp->w + x] = a; // max-combine
}

void gfx_sprite_pacman(gfx_sprite_t *sp, float cx, float cy, float r,
                       float mouth_deg, float facing_deg)
{
    float facing = facing_deg * (float)M_PI / 180.0f;
    float half   = mouth_deg  * (float)M_PI / 180.0f;
    float fdx = cosf(facing), fdy = sinf(facing);
    float cosh = cosf(half), cosh2 = cosh * cosh;
    float rsq = r * r;
    int x0 = (int)floorf(cx - r) - 1, x1 = (int)ceilf(cx + r) + 1;
    int y0 = (int)floorf(cy - r) - 1, y1 = (int)ceilf(cy + r) + 1;
    for (int y = y0; y <= y1; y++)
        for (int x = x0; x <= x1; x++) {
            int hits = 0;
            for (int sy = 0; sy < 3; sy++)
                for (int sx = 0; sx < 3; sx++) {
                    float dx = (x + (sx + 0.5f) / 3.0f) - cx;
                    float dy = (y + (sy + 0.5f) / 3.0f) - cy;
                    float dsq = dx * dx + dy * dy;
                    if (dsq > rsq) continue;
                    float dot = dx * fdx + dy * fdy;
                    if (dot > 0 && dot * dot > cosh2 * dsq) continue;
                    hits++;
                }
            if (hits) sp_set(sp, x, y, (uint8_t)(hits * 255 / 9));
        }
}

void gfx_sprite_bolt(gfx_sprite_t *sp, float cx, float cy, float h)
{
    float s = h / 10.0f;
    const float px[] = { 1.5f, -2.0f,  0.0f, -1.5f,  2.0f,  0.0f };
    const float py[] = { -5.0f, 1.0f,  1.0f,  5.0f, -1.0f, -1.0f };
    const int n = 6;
    float minx = 1e9f, maxx = -1e9f, miny = 1e9f, maxy = -1e9f;
    for (int i = 0; i < n; i++) {
        if (px[i] < minx) minx = px[i];
        if (px[i] > maxx) maxx = px[i];
        if (py[i] < miny) miny = py[i];
        if (py[i] > maxy) maxy = py[i];
    }
    int x0 = (int)floorf(cx + minx * s) - 1, x1 = (int)ceilf(cx + maxx * s) + 1;
    int y0 = (int)floorf(cy + miny * s) - 1, y1 = (int)ceilf(cy + maxy * s) + 1;
    for (int y = y0; y <= y1; y++)
        for (int x = x0; x <= x1; x++) {
            int hits = 0;
            for (int sy = 0; sy < 3; sy++)
                for (int sx = 0; sx < 3; sx++) {
                    float fx = (x + (sx + 0.5f) / 3.0f - cx) / s;
                    float fy = (y + (sy + 0.5f) / 3.0f - cy) / s;
                    bool inside = false;
                    for (int i = 0, j = n - 1; i < n; j = i++) {
                        if (((py[i] > fy) != (py[j] > fy)) &&
                            (fx < (px[j] - px[i]) * (fy - py[i]) / (py[j] - py[i]) + px[i]))
                            inside = !inside;
                    }
                    if (inside) hits++;
                }
            if (hits) sp_set(sp, x, y, (uint8_t)(hits * 255 / 9));
        }
}

void gfx_sprite_punch_circle(gfx_sprite_t *sp, float cx, float cy, float r)
{
    int x0 = (int)floorf(cx - r) - 1, x1 = (int)ceilf(cx + r) + 1;
    int y0 = (int)floorf(cy - r) - 1, y1 = (int)ceilf(cy + r) + 1;
    for (int y = y0; y <= y1; y++)
        for (int x = x0; x <= x1; x++) {
            if ((unsigned)x >= (unsigned)sp->w || (unsigned)y >= (unsigned)sp->h) continue;
            float dx = x + 0.5f - cx, dy = y + 0.5f - cy;
            if (dx * dx + dy * dy <= r * r) sp->alpha[y * sp->w + x] = 0;
        }
}

void gfx_sprite_circle(gfx_sprite_t *sp, float cx, float cy, float r)
{
    float rsq = r * r;
    int x0 = (int)floorf(cx - r) - 1, x1 = (int)ceilf(cx + r) + 1;
    int y0 = (int)floorf(cy - r) - 1, y1 = (int)ceilf(cy + r) + 1;
    for (int y = y0; y <= y1; y++)
        for (int x = x0; x <= x1; x++) {
            int hits = 0;
            for (int sy = 0; sy < 3; sy++)
                for (int sx = 0; sx < 3; sx++) {
                    float dx = (x + (sx + 0.5f) / 3.0f) - cx;
                    float dy = (y + (sy + 0.5f) / 3.0f) - cy;
                    if (dx * dx + dy * dy <= rsq) hits++;
                }
            if (hits) sp_set(sp, x, y, (uint8_t)(hits * 255 / 9));
        }
}

void gfx_blit(const gfx_sprite_t *sp, int x, int y, uint8_t gain)
{
    if (!sp->alpha) return;
    for (int yy = 0; yy < sp->h; yy++) {
        for (int xx = 0; xx < sp->w; xx++) {
            uint8_t a = sp->alpha[yy * sp->w + xx];
            if (!a) continue;
            if (gain != 255) a = (uint8_t)((a * gain) / 255);
            gfx_blend(x + xx, y + yy, sp->color, a);
        }
    }
}

// ---- Baked JetBrains Mono readout font -------------------------------------
#include "font_jbmono.h"

int gfx_jbmono_index(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c == '.') return 10;
    if (c == '-') return 11;
    if (c == ' ') return 12;
    return -1;
}

void gfx_jbtext(int x, int y, const char *s, uint16_t color)
{
    int cx = x;
    for (; *s; s++) {
        int idx = gfx_jbmono_index(*s);
        if (idx >= 0 && idx < gfx_jbmono_count) {
            const jb_glyph_t *g = &gfx_jbmono[idx];
            for (int yy = 0; yy < g->h; yy++) {
                const uint8_t *row = &g->alpha[yy * g->w];
                for (int xx = 0; xx < g->w; xx++) {
                    uint8_t a = row[xx];
                    if (a) gfx_blend(cx + xx, y + yy, color, a);
                }
            }
        }
        cx += gfx_jbmono_advance;
    }
}

int gfx_jbtext_w(const char *s)
{
    int n = 0;
    for (; *s; s++) n++;
    return n * gfx_jbmono_advance;
}

int gfx_jbtext_h(void)
{
    return gfx_jbmono_cell_h;
}

// ---- Rounded rectangles ----------------------------------------------------
void gfx_round_rect_grad(int x, int y, int w, int h, int r, uint16_t top, uint16_t bottom)
{
    if (r * 2 > w) r = w / 2;
    if (r * 2 > h) r = h / 2;
    int tr, tg, tb, br, bg, bb;
    unpack(top, &tr, &tg, &tb);
    unpack(bottom, &br, &bg, &bb);
    int denom = (h > 1) ? (h - 1) : 1;

    // Signed-distance rounded box: gives clean, symmetric AA on every corner
    // (the old per-corner supersample left a faint hard seam along the bottom).
    float hw = w * 0.5f, hh = h * 0.5f;
    float cx = x + hw, cy = y + hh;
    float ix = hw - r, iy = hh - r;          // inner half-extents (straight part)

    for (int yy = 0; yy < h; yy++) {
        int rr = tr + (br - tr) * yy / denom;
        int rg = tg + (bg - tg) * yy / denom;
        int rb = tb + (bb - tb) * yy / denom;
        uint16_t col = gfx_rgb(rr, rg, rb);
        int gy = y + yy;
        float py = gy + 0.5f - cy;
        float adY = fabsf(py) - iy; if (adY < 0) adY = 0;
        for (int xx = 0; xx < w; xx++) {
            int gx = x + xx;
            float px = gx + 0.5f - cx;
            float adX = fabsf(px) - ix; if (adX < 0) adX = 0;
            float dist = sqrtf(adX * adX + adY * adY) - r;   // <0 inside
            uint8_t cov;
            if (dist <= -0.5f) cov = 255;
            else if (dist >= 0.5f) cov = 0;
            else cov = (uint8_t)((0.5f - dist) * 255.0f);
            if (cov) gfx_blend(gx, gy, col, cov);
        }
    }
}

void gfx_round_rect(int x, int y, int w, int h, int r, uint16_t color)
{
    gfx_round_rect_grad(x, y, w, h, r, color, color);
}

void gfx_round_rect_border(int x, int y, int w, int h, int r, uint16_t color)
{
    if (r * 2 > w) r = w / 2;
    if (r * 2 > h) r = h / 2;
    gfx_hline(x + r, y,         w - 2 * r, color);
    gfx_hline(x + r, y + h - 1, w - 2 * r, color);
    gfx_vline(x,         y + r, h - 2 * r, color);
    gfx_vline(x + w - 1, y + r, h - 2 * r, color);
    // AA corners (thin arc)
    float cxs[4] = { x + r,         x + w - r - 1, x + r,         x + w - r - 1 };
    float cys[4] = { y + r,         y + r,         y + h - r - 1, y + h - r - 1 };
    for (int k = 0; k < 4; k++) {
        for (int yy = -r - 1; yy <= r + 1; yy++)
            for (int xx = -r - 1; xx <= r + 1; xx++) {
                float d = sqrtf(xx * xx + yy * yy);
                float e = fabsf(d - r);
                if (e < 1.2f) {
                    uint8_t a = (uint8_t)((1.2f - e) / 1.2f * 255);
                    // only the outward quarter
                    int gx = (int)cxs[k] + xx, gy = (int)cys[k] + yy;
                    if ((k == 0 && xx <= 0 && yy <= 0) ||
                        (k == 1 && xx >= 0 && yy <= 0) ||
                        (k == 2 && xx <= 0 && yy >= 0) ||
                        (k == 3 && xx >= 0 && yy >= 0))
                        gfx_blend(gx, gy, color, a);
                }
            }
    }
}

// ---- Text ------------------------------------------------------------------
void gfx_text(int x, int y, const char *s, uint8_t size, uint16_t color)
{
    int cx = x;
    for (; *s; s++) {
        unsigned char ch = (unsigned char)*s;
        if (ch == '\n') { cx = x; y += GFX_SMALL_H(size) + size; continue; }
        const unsigned char *g = &gfx_font5x7[ch * 5];
        for (int col = 0; col < 5; col++) {
            uint8_t bits = g[col];
            for (int row = 0; row < 7; row++) {
                if (bits & (1 << row))
                    gfx_fill_rect(cx + col * size, y + row * size, size, size, color);
            }
        }
        cx += 6 * size;
    }
}

int gfx_text_w(const char *s, uint8_t size)
{
    return (int)strlen(s) * 6 * size;
}
