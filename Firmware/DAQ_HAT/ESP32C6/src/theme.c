#include "theme.h"
#include "gfx.h"

theme_t g_theme;
static bool s_dark = false;

// Default "light" palette — white background, dark text, vivid accents.
static void load_default(void)
{
    g_theme.bg       = gfx_rgb(255, 255, 255);
    g_theme.bg1      = gfx_rgb(244, 246, 250);
    g_theme.header   = gfx_rgb(236, 240, 246);
    g_theme.card     = gfx_rgb(248, 250, 252);
    g_theme.card_hi  = gfx_rgb(238, 242, 248);
    g_theme.border   = gfx_rgb(196, 205, 218);
    g_theme.text     = gfx_rgb(17, 24, 39);
    g_theme.dim      = gfx_rgb(55, 65, 81);
    g_theme.muted    = gfx_rgb(107, 114, 128);
    g_theme.blue     = gfx_rgb(37, 99, 235);
    g_theme.green    = gfx_rgb(5, 150, 105);
    g_theme.amber    = gfx_rgb(217, 119, 6);
    g_theme.rose     = gfx_rgb(220, 38, 38);
    g_theme.cyan     = gfx_rgb(8, 145, 178);
    g_theme.purple   = gfx_rgb(124, 58, 237);
    g_theme.sel      = gfx_rgb(37, 99, 235);
    g_theme.sel_text = gfx_rgb(255, 255, 255);
}

// "Neon dark" palette — true black background, white text, saturated neon
// accents.
static void load_dark(void)
{
    g_theme.bg       = gfx_rgb(0, 0, 0);
    g_theme.bg1      = gfx_rgb(0, 0, 0);
    g_theme.header   = gfx_rgb(0, 0, 0);
    g_theme.card     = gfx_rgb(12, 14, 20);
    g_theme.card_hi  = gfx_rgb(20, 24, 34);
    g_theme.border   = gfx_rgb(40, 70, 88);
    g_theme.text     = gfx_rgb(255, 255, 255);
    g_theme.dim      = gfx_rgb(200, 210, 220);
    g_theme.muted    = gfx_rgb(120, 130, 145);
    g_theme.blue     = gfx_rgb(77, 210, 255);
    g_theme.green    = gfx_rgb(57, 255, 158);
    g_theme.amber    = gfx_rgb(255, 210, 58);
    g_theme.rose     = gfx_rgb(255, 77, 109);
    g_theme.cyan     = gfx_rgb(24, 240, 255);
    g_theme.purple   = gfx_rgb(194, 100, 255);
    g_theme.sel      = gfx_rgb(24, 240, 255);
    g_theme.sel_text = gfx_rgb(0, 0, 0);
}

void theme_init(void)
{
    s_dark = false;
    load_default();
}

void theme_set_dark(bool dark)
{
    s_dark = dark;
    if (dark) load_dark();
    else      load_default();
}

bool theme_is_dark(void) { return s_dark; }
