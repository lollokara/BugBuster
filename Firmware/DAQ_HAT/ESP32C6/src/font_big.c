#include "gfx.h"
#include <string.h>

// Big readout digits rendered as anti-aliased vector strokes for a smooth,
// modern, monospaced look reminiscent of the desktop app's JetBrains Mono
// numerals. Geometry is defined in a nominal BIGFONT_W x BIGFONT_H cell and
// scaled by `scale`.
//
// Coordinate frame inside the cell (origin top-left, y down):
//   left stroke x = 3, right = 13, center x = 8
//   top loop center (8,9) r5, bottom loop center (8,19) r5  (touch at y=14)

static float s_x, s_y, s_scale, s_th;
static uint16_t s_col;

#define SX(nx) (s_x + (nx) * s_scale)
#define SY(ny) (s_y + (ny) * s_scale)

static void L(float x0, float y0, float x1, float y1)
{
    gfx_thick_line(SX(x0), SY(y0), SX(x1), SY(y1), s_th, s_col);
}
static void A(float cx, float cy, float r, float a0, float a1)
{
    gfx_arc(SX(cx), SY(cy), r * s_scale, a0, a1, s_th, s_col);
}

static int advance(char c, float scale)
{
    switch (c) {
        case '.': return (int)(9 * scale);
        case ' ': return (int)(9 * scale);
        default:  return (int)(BIGFONT_W * scale);
    }
}

static void draw_digit(char c)
{
    switch (c) {
    case '0':
        L(3, 9, 3, 19); L(13, 9, 13, 19);
        A(8, 9, 5, 180, 360); A(8, 19, 5, 0, 180);
        break;
    case '1':
        L(8, 4, 8, 24); L(4.5f, 7, 8, 4); L(4, 24, 12, 24);
        break;
    case '2':
        A(8, 9, 5, 160, 375);
        L(12.6f, 10.6f, 3.6f, 23);
        L(3, 24, 13, 24);
        break;
    case '3':
        A(8, 9.5f, 4.5f, 170, 430);
        A(8, 18.5f, 4.5f, 290, 550);
        break;
    case '4':
        L(10.5f, 4, 3, 17.5f); L(3, 17.5f, 13, 17.5f); L(10.5f, 4, 10.5f, 24);
        break;
    case '5':
        L(4, 4, 12, 4); L(4, 4, 4, 13); L(4, 13, 6.5f, 13);
        A(8, 18, 5, 250, 540);
        break;
    case '6':
        L(12, 5, 5, 14);
        A(8, 18.5f, 5, 0, 360);
        break;
    case '7':
        L(3, 4, 13, 4); L(13, 4, 6, 24);
        break;
    case '8':
        A(8, 9, 5, 0, 360); A(8, 19, 5, 0, 360);
        break;
    case '9':
        A(8, 9.5f, 5, 0, 360); L(11.5f, 12, 7, 24);
        break;
    case '-':
        L(3, 14, 13, 14);
        break;
    case '.':
        gfx_fill_circle(SX(5), SY(22), 1.9f * s_scale, s_col);
        break;
    default: /* space */ break;
    }
}

void gfx_bigtext(int x, int y, const char *s, float scale, uint16_t color)
{
    s_x = x; s_y = y; s_scale = scale; s_col = color;
    s_th = 3.0f * scale;
    if (s_th < 1.4f) s_th = 1.4f;
    for (; *s; s++) {
        s_x = (float)x;
        draw_digit(*s);
        x += advance(*s, scale);
        s_x = (float)x;
    }
}

int gfx_bigtext_w(const char *s, float scale)
{
    int w = 0;
    for (; *s; s++) w += advance(*s, scale);
    return w;
}
