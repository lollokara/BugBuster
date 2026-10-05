#include "splash.h"

#include <stdio.h>
#include <string.h>
#include "config.h"
#include "gfx.h"
#include "logo_bb.h"

#define SP_LOGO_X   8
#define SP_LOGO_Y   ((DISP_HEIGHT - LOGO_BB_H) / 2)
#define SP_TEXT_X   (SP_LOGO_X + LOGO_BB_W + 12)
#define SP_RIGHT    (DISP_WIDTH - 8)
#define SP_TITLE_Y  6
#define SP_LINE_Y   28
#define SP_BAR_Y    42
#define SP_BAR_H    8
#define SP_BAR_GAP  3
#define SP_DETAIL_Y 58

#define C_DIM    gfx_rgb(140, 150, 160)
#define C_MUTED  gfx_rgb(70, 78, 88)
#define C_OK     gfx_rgb(60, 200, 120)
#define C_FAIL   gfx_rgb(235, 80, 80)
#define C_SKIP   gfx_rgb(110, 120, 135)
#define C_TIME   gfx_rgb(240, 170, 40)
#define C_TITLE  gfx_rgb(90, 210, 220)

static void draw_logo(void)
{
    for (int y = 0; y < LOGO_BB_H; ++y) {
        for (int x = 0; x < LOGO_BB_W; ++x) {
            gfx_pixel(SP_LOGO_X + x, SP_LOGO_Y + y, LOGO_BB_PIXELS[y * LOGO_BB_W + x]);
        }
    }
}

static void draw_bar(int n, const uint8_t *status)
{
    if (n <= 0) return;
    const int span = SP_RIGHT - SP_TEXT_X;
    const int w = (span - (n - 1) * SP_BAR_GAP) / n;
    for (int i = 0; i < n; ++i) {
        const int x = SP_TEXT_X + i * (w + SP_BAR_GAP);
        switch (status[i]) {
        case SB_ITEM_OK:      gfx_fill_rect(x, SP_BAR_Y, w, SP_BAR_H, C_OK);   break;
        case SB_ITEM_FAILED:  gfx_fill_rect(x, SP_BAR_Y, w, SP_BAR_H, C_FAIL); break;
        case SB_ITEM_SKIPPED: gfx_fill_rect(x, SP_BAR_Y, w, SP_BAR_H, C_SKIP); break;
        case SB_ITEM_TIMEOUT:
            // Hatched: unresolved at a deadline, visibly different from "skipped".
            gfx_rect(x, SP_BAR_Y, w, SP_BAR_H, C_TIME);
            for (int hx = x + 2; hx < x + w - 1; hx += 4) gfx_vline(hx, SP_BAR_Y + 1, SP_BAR_H - 2, C_TIME);
            break;
        default:              gfx_rect(x, SP_BAR_Y, w, SP_BAR_H, C_MUTED);     break;
        }
    }
}

static void draw_frame(const char *line, int n, const uint8_t *status, const char *detail,
                       uint16_t detail_color)
{
    gfx_clear(GFX_BLACK);
    draw_logo();
    gfx_text(SP_TEXT_X, SP_TITLE_Y, "BugBuster", 2, C_TITLE);
    gfx_text(SP_TEXT_X, SP_LINE_Y, line, 1, C_DIM);
    draw_bar(n, status);
    if (detail && detail[0]) gfx_text(SP_TEXT_X, SP_DETAIL_Y, detail, 1, detail_color);
}

void splash_draw(const sb_c6_view_t *v)
{
    char line[40];
    char detail[40] = "";
    uint16_t detail_color = C_DIM;

    if (v->mode == SB_UI_WAKE) {
        if (v->waiting_mainboard)  snprintf(line, sizeof(line), "Waking - waiting for mainboard");
        else if (v->finished)      snprintf(line, sizeof(line), v->issues ? "Awake - with issues" : "Awake");
        else                       snprintf(line, sizeof(line), "Waking: %s", v->label[v->current]);
    } else {
        if (v->finished)           snprintf(line, sizeof(line), v->issues ? "Ready - with issues" : "Ready");
        else                       snprintf(line, sizeof(line), "Starting: %s", v->label[v->current]);
    }

    // The first thing that went wrong is named; otherwise show honest progress.
    bool named = false;
    for (int i = 0; i < v->n && !named; ++i) {
        const uint8_t s = v->status[i];
        if (s == SB_ITEM_OK || s == SB_ITEM_PENDING) continue;
        snprintf(detail, sizeof(detail), "%s: %s", v->label[i], sb_c6_item_text((sb_item_t)s));
        detail_color = (s == SB_ITEM_FAILED) ? C_FAIL : (s == SB_ITEM_TIMEOUT) ? C_TIME : C_DIM;
        named = true;
    }
    if (v->s3_silent) {
        snprintf(detail, sizeof(detail), "Mainboard sent no boot report");
        detail_color = C_TIME;
    } else if (!named) {
        snprintf(detail, sizeof(detail), "%u/%u ready", (unsigned)v->ok_count, (unsigned)v->n);
    }
    draw_frame(line, v->n, v->status, detail, detail_color);
}

void splash_draw_logo_frame(void)
{
    uint8_t none[SB_C6_WAKE_ITEMS];
    memset(none, 0, sizeof(none));
    draw_frame("Waking...", SB_C6_WAKE_ITEMS, none, "", C_DIM);
}
