// -----------------------------------------------------------------------------
// Web-UI parity matrix — keep in sync with Firmware/ESP32/web/src/tabs/*
// -----------------------------------------------------------------------------
// CLI tab        <-> Web tab           <-> Status
// Overview       <-> overview/         <-> parity (read-only)
// CH0..CH3       <-> analog/           <-> parity (function, ADC mux/range/rate, DAC, DO, alerts)
// Power          <-> system/           <-> parity subset (VADJ sliders, e-fuse, 15V, MUX/USB hub)
// Signal         <-> signal/SignalPath <-> parity (32-switch grid, reset preset)
// HAT            <-> system/ (HAT card)<-> parity (pin pickers, conn power, IO voltage, detect/reset)
// Settings       <-> system/           <-> parity (board profile, USB-PD, faults, diag, calibration)
// -----------------------------------------------------------------------------

// =============================================================================
// cli_menu.cpp - Interactive full-screen menu (TUI) for the BugBuster CLI.
//
// Architecture (three layers):
//
//   ┌─────────────────────────────────────────────────────────────────┐
//   │ Input layer     ─ ESC/CSI parser + modal stack key dispatch     │
//   ├─────────────────────────────────────────────────────────────────┤
//   │ Model layer     ─ snapshot of g_deviceState + dirty tracking    │
//   ├─────────────────────────────────────────────────────────────────┤
//   │ Render layer    ─ heap-allocated cell back-buffer + diff blit   │
//   └─────────────────────────────────────────────────────────────────┘
//
// Anti-flicker: the back-buffer is compared cell-by-cell with the front
// buffer each tick; only contiguous changed runs are emitted as
// `\x1b[r;cH<SGR><glyphs>` sequences. When nothing changed, present()
// emits zero bytes — eliminating the flicker that came from clearing the
// screen every 100 ms.
//
// Autoscaling: `term_rows()`/`term_cols()` are sampled each tick; when
// the size changes, both buffers are reallocated and a full repaint is
// forced.
//
// Modal stack: pickers / sliders / confirms render over the active tab
// without losing it; ESC pops a modal (instead of exiting the menu);
// double-ESC still exits.
// =============================================================================

#include "cli_menu.h"
#include "cli_term.h"
#include "cli_shared.h"
#include "tasks.h"
#include "bbp.h"
#include "config.h"

#include "ad74416h.h"
#include "ad74416h_regs.h"
#include "board_profile.h"
#include "husb238.h"
#include "pca9535.h"
#include "adgs2414d.h"
#include "hat.h"
#include "selftest.h"
#include "ds4424.h"
#include "wifi_manager.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_mac.h"

// ---------------------------------------------------------------------------
// Geometry / buffer caps
// ---------------------------------------------------------------------------
#define MENU_MIN_ROWS   18
#define MENU_MIN_COLS   60
#define MENU_MAX_ROWS   60
#define MENU_MAX_COLS   180

// Cell attribute flags (packed)
#define ATTR_BOLD     0x01
#define ATTR_REVERSE  0x02
#define ATTR_DIM      0x04

// Style packing helper: TermColor fits in 8 bits (values 30..97).
struct Cell {
    char     glyph;     // single ASCII byte; UTF-8 multi-byte not used in cells
    uint8_t  fg;        // TermColor (TERM_FG_*) — 0 means default (39)
    uint8_t  attr;      // ATTR_* bitmask
};

// ---------------------------------------------------------------------------
// Render state
// ---------------------------------------------------------------------------
static Cell*   s_back  = nullptr;
static Cell*   s_front = nullptr;
static int     s_rows  = 0;     // current allocated rows
static int     s_cols  = 0;     // current allocated cols

// ---------------------------------------------------------------------------
// Lifecycle / housekeeping
// ---------------------------------------------------------------------------
static bool     s_active           = false;
static bool     s_alt_out          = false;
static bool     s_want_exit        = false;
static uint32_t s_last_redraw_ms   = 0;
static uint32_t s_slow_snap_last_ms = 0;
static uint32_t s_last_probe_ms    = 0;
static bool     s_force_redraw     = true;
static bool     s_last_esc_was_lone = false;     // for double-ESC detection
static uint32_t s_last_esc_ms      = 0;

// ---------------------------------------------------------------------------
// Input state machine
// ---------------------------------------------------------------------------
typedef enum {
    ST_NORMAL = 0,
    ST_ESC,
    ST_CSI,
    ST_SS3,
} MenuParseState;

static MenuParseState s_pstate = ST_NORMAL;
static char  s_csi_params[16];
static uint8_t s_csi_len = 0;

// ---------------------------------------------------------------------------
// Tabs & navigation
// ---------------------------------------------------------------------------
typedef enum {
    TAB_OVERVIEW = 0,
    TAB_CH0,
    TAB_CH1,
    TAB_CH2,
    TAB_CH3,
    TAB_POWER,
    TAB_SIGNAL,
    TAB_HAT,
    TAB_SETTINGS,
    NUM_TABS
} MenuTab;

static const char* kTabNames[NUM_TABS] = {
    "Overview", "CH0", "CH1", "CH2", "CH3", "Power", "Signal", "HAT", "Settings"
};

// Channel tab field rows
typedef enum {
    CH_FIELD_FUNCTION = 0,
    CH_FIELD_ADC_RATE,
    CH_FIELD_ADC_MUX,
    CH_FIELD_DAC_VOLTAGE,
    CH_FIELD_DO_TOGGLE,
    CH_FIELD_CLEAR_ALERT,
    NUM_CH_FIELDS
} ChannelField;

// Settings tab rows
typedef enum {
    SET_FIELD_CLEAR_FAULTS = 0,
    SET_FIELD_USBPD,
    SET_FIELD_BOARD_PROFILE,
    SET_FIELD_DIAG_0,
    SET_FIELD_DIAG_1,
    SET_FIELD_DIAG_2,
    SET_FIELD_DIAG_3,
    SET_FIELD_CAL_VADJ1,
    SET_FIELD_CAL_VADJ2,
    SET_FIELD_CAL_SAVE,
    SET_FIELD_CAL_LOAD,
    SET_FIELD_CAL_CLEAR,
    NUM_SET_FIELDS
} SettingsField;

// Power tab rows
typedef enum {
    PWR_FIELD_VADJ1 = 0,
    PWR_FIELD_VADJ2,
    PWR_FIELD_VLOGIC,
    PWR_FIELD_EFUSE1,
    PWR_FIELD_EFUSE2,
    PWR_FIELD_EFUSE3,
    PWR_FIELD_EFUSE4,
    PWR_FIELD_15V,
    PWR_FIELD_MUX,
    PWR_FIELD_USBHUB,
    NUM_PWR_FIELDS
} PowerField;

// Signal tab field count: 32 switch cells + 1 reset-all row
#define SIGNAL_GRID_CELLS   32   // 4 devices x 8 switches
#define NUM_SIGNAL_FIELDS   (SIGNAL_GRID_CELLS + 1)
#define SIGNAL_RESET_FIELD  0    // Reset row is first (field 0)
#define SIGNAL_GRID_BASE    1    // Grid cells start at field 1

// HAT tab rows
typedef enum {
    HAT_FIELD_PIN0 = 0,
    HAT_FIELD_PIN1,
    HAT_FIELD_PIN2,
    HAT_FIELD_PIN3,
    HAT_FIELD_CONN_A,
    HAT_FIELD_CONN_B,
    HAT_FIELD_IO_VOLT,
    HAT_FIELD_DETECT,
    HAT_FIELD_RESET,
    NUM_HAT_FIELDS
} HatTabField;

static MenuTab s_tab = TAB_OVERVIEW;
static uint8_t s_field = 0;

// ---------------------------------------------------------------------------
// Snapshot (model)
// ---------------------------------------------------------------------------
struct ChanSnap {
    ChannelFunction function;
    AdcRange        adcRange;
    AdcRate         adcRate;
    AdcConvMux      adcMux;
    uint32_t        adcRawCode;
    float           adcValue;
    uint16_t        dacCode;
    float           dacValue;
    bool            doState;
    uint16_t        channelAlertStatus;
};

struct DiagSnap {
    uint8_t source;
    float   value;
};

struct MenuSnapshot {
    bool      valid;
    bool      spiOk;
    bool      i2cOk;
    float     dieTemperature;
    uint16_t  liveStatus;
    uint16_t  alertStatus;
    uint16_t  supplyAlertStatus;
    ChanSnap  ch[4];
    DiagSnap  diag[4];
    // Cached non-mutex values (board profile + USB-PD pulled outside mutex)
    char      profileName[32];
    char      profileId[16];
    float     vlogic;
    float     vadj1;
    float     vadj2;
    float     pdVoltage;
    float     pdCurrent;
    bool      pdAttached;
    // Power tab
    bool      efuseEn[4];
    bool      efuseFlt[4];
    bool      rail15v;
    bool      muxEn;
    bool      usbHubEn;
    bool      vadj1Pg;
    bool      vadj2Pg;
    bool      logicPg;
    // Signal tab
    uint8_t   muxState[ADGS_API_MAIN_DEVICES];
    // ADGS fault state (populated in take_snapshot, outside mutex — SPI call)
    uint8_t   adgsErrorFlags;
    bool      adgsFaulted;
    // HAT status (Fix 3)
    bool      hatDetected;
    uint8_t   hatType;
    bool      hatConnected;
    // WiFi status (Fix 5)
    char      ipStr[16];
    int8_t    rssi;
    char      ssid[33];
};

static MenuSnapshot s_snap;

// ---------------------------------------------------------------------------
// Modal stack
// ---------------------------------------------------------------------------
typedef enum {
    M_NONE = 0,
    M_PICKER,
    M_SLIDER,
    M_CONFIRM,
    M_PROGRESS,
    M_HELP,
} ModalKind;

struct PickerItem {
    int32_t     value;
    const char* label;
};

typedef void (*PickerCb)(int32_t value, void* user);
typedef void (*SliderCb)(float value, void* user);
typedef void (*ConfirmCb)(bool yes, void* user);

struct ModalFrame {
    ModalKind   kind;
    const char* title;

    // picker
    const PickerItem* items;
    uint8_t           item_count;
    uint8_t           selected;
    uint8_t           scroll;       // first visible index when list overflows
    PickerCb          picker_cb;

    // slider
    const char* unit;
    float       min;
    float       max;
    float       step;
    float       value;
    bool        apply_pending;
    SliderCb    slider_cb;

    // confirm
    const char* prompt;
    ConfirmCb   confirm_cb;
    bool        confirm_yes;       // current cursor position (defaults to NO)

    // progress
    const char* progress_status;
    int         progress_percent;  // 0..100, or -1 for indeterminate
    bool        progress_done;
    bool        progress_failed;
    bool        progress_cancel;   // set by ESC; visible to tick_cb via frame
    void      (*progress_tick)(struct ModalFrame*, void*);

    void* user;
};

#define MODAL_STACK_DEPTH  3
static ModalFrame s_modal_stack[MODAL_STACK_DEPTH];
static int        s_modal_top = -1;       // -1 == empty

// ---------------------------------------------------------------------------
// Toast (non-blocking transient message)
// ---------------------------------------------------------------------------
static char       s_toast_msg[80];
static uint32_t   s_toast_set_ms      = 0;
static uint32_t   s_toast_duration_ms = 0;
static TermColor  s_toast_color = TERM_FG_B_GREEN;

// ---------------------------------------------------------------------------
// Forward decls
// ---------------------------------------------------------------------------
static void render_full(void);
static bool buffer_alloc(int rows, int cols);
static void buffer_free(void);

// ===========================================================================
// Cell-buffer primitives
// ===========================================================================

static inline Cell* cell_at(int row /*0-idx*/, int col /*0-idx*/) {
    if (!s_back) return nullptr;
    if (row < 0 || row >= s_rows || col < 0 || col >= s_cols) return nullptr;
    return &s_back[row * s_cols + col];
}

static void clear_back(void) {
    if (!s_back) return;
    int n = s_rows * s_cols;
    for (int i = 0; i < n; i++) {
        s_back[i].glyph = ' ';
        s_back[i].fg    = 0;
        s_back[i].attr  = 0;
    }
}

static void invalidate_front(void) {
    if (!s_front) return;
    int n = s_rows * s_cols;
    // Use a sentinel that no real draw will produce: glyph=0, fg=0xFF
    for (int i = 0; i < n; i++) {
        s_front[i].glyph = 0;
        s_front[i].fg    = 0xFF;
        s_front[i].attr  = 0xFF;
    }
}

static bool buffer_alloc(int rows, int cols) {
    if (rows < MENU_MIN_ROWS) rows = MENU_MIN_ROWS;
    if (cols < MENU_MIN_COLS) cols = MENU_MIN_COLS;
    if (rows > MENU_MAX_ROWS) rows = MENU_MAX_ROWS;
    if (cols > MENU_MAX_COLS) cols = MENU_MAX_COLS;

    if (s_back && rows == s_rows && cols == s_cols) return true;

    buffer_free();
    size_t n = (size_t)rows * (size_t)cols;
    s_back  = (Cell*)malloc(n * sizeof(Cell));
    s_front = (Cell*)malloc(n * sizeof(Cell));
    if (!s_back || !s_front) {
        buffer_free();
        return false;
    }
    s_rows = rows;
    s_cols = cols;
    clear_back();
    invalidate_front();
    return true;
}

static void buffer_free(void) {
    if (s_back)  { free(s_back);  s_back  = nullptr; }
    if (s_front) { free(s_front); s_front = nullptr; }
    s_rows = 0;
    s_cols = 0;
}

// Draw a single character cell (0-indexed).
static inline void put_cell(int row, int col, char ch, uint8_t fg, uint8_t attr) {
    Cell* c = cell_at(row, col);
    if (!c) return;
    c->glyph = ch;
    c->fg    = fg;
    c->attr  = attr;
}

// Draw text starting at (row, col). Truncates at right edge.
static void draw_text(int row, int col, const char* s, uint8_t fg, uint8_t attr) {
    if (!s) return;
    int c = col;
    while (*s && c < s_cols) {
        // Skip non-printables (ESC etc) for safety
        if ((uint8_t)*s >= 0x20 && (uint8_t)*s < 0x7F) {
            put_cell(row, c, *s, fg, attr);
        } else {
            put_cell(row, c, ' ', fg, attr);
        }
        s++;
        c++;
    }
}

// printf-style draw, truncated to terminal width.
static void draw_textf(int row, int col, uint8_t fg, uint8_t attr,
                       const char* fmt, ...) {
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n < 0) return;
    draw_text(row, col, buf, fg, attr);
}

// Horizontal line of `len` cells at (row, col) using `glyph`.
static void draw_hline(int row, int col, int len, char glyph,
                       uint8_t fg, uint8_t attr) {
    for (int i = 0; i < len; i++) {
        put_cell(row, col + i, glyph, fg, attr);
    }
}

// Fill a rectangular region with a single character (typically ' ' to clear).
static void draw_fill(int row, int col, int rows, int cols,
                      char glyph, uint8_t fg, uint8_t attr) {
    for (int r = 0; r < rows; r++) {
        for (int c = 0; c < cols; c++) {
            put_cell(row + r, col + c, glyph, fg, attr);
        }
    }
}

// Box outline using ASCII (+, -, |). Inside is left untouched.
static void draw_box(int row, int col, int rows, int cols, uint8_t fg, uint8_t attr) {
    if (rows < 2 || cols < 2) return;
    // top + bottom
    for (int c = 1; c < cols - 1; c++) {
        put_cell(row,            col + c, '-', fg, attr);
        put_cell(row + rows - 1, col + c, '-', fg, attr);
    }
    // sides
    for (int r = 1; r < rows - 1; r++) {
        put_cell(row + r, col,            '|', fg, attr);
        put_cell(row + r, col + cols - 1, '|', fg, attr);
    }
    // corners
    put_cell(row,            col,            '+', fg, attr);
    put_cell(row,            col + cols - 1, '+', fg, attr);
    put_cell(row + rows - 1, col,            '+', fg, attr);
    put_cell(row + rows - 1, col + cols - 1, '+', fg, attr);
}

// ===========================================================================
// Unicode decoration glyph system (Fix 1)
// ===========================================================================
// Glyph codes 0x80..0x9F are "decoration codes" stored in Cell.glyph.
// They are never produced by draw_text (which filters >=0x7F to space).
// At emit time, emit_run expands them to their UTF-8 sequences.
// Use draw_glyph() to place them — bypasses draw_text filtering.

#define G_BOX_TL    0x80   // ┌
#define G_BOX_TR    0x81   // ┐
#define G_BOX_BL    0x82   // └
#define G_BOX_BR    0x83   // ┘
#define G_BOX_H     0x84   // ─
#define G_BOX_V     0x85   // │
#define G_BOX_TEE_L 0x86   // ├
#define G_BOX_TEE_R 0x87   // ┤
#define G_BOX_TEE_T 0x88   // ┬
#define G_BOX_TEE_B 0x89   // ┴
#define G_BOX_CROSS 0x8A   // ┼
#define G_BOX_DBL_H 0x8B   // ═
#define G_BOX_DBL_V 0x8C   // ║
#define G_BAR_8L    0x8D   // ▏
#define G_BAR_8R    0x8E   // ▕
#define G_BLOCK_F   0x8F   // █
#define G_BLOCK_LH  0x90   // ▌
#define G_BLOCK_RH  0x91   // ▐
#define G_BLOCK_UH  0x92   // ▀
#define G_BLOCK_DH  0x93   // ▄
#define G_LIGHT_DOT 0x94   // · (U+00B7)
#define G_ARROW_R   0x95   // ▶
#define G_ARROW_D   0x96   // ▼
#define G_BULLET_F  0x97   // ●
#define G_BULLET_O  0x98   // ○
#define G_RSSI_B1   0x99   // ▁
#define G_RSSI_B2   0x9A   // ▂
#define G_RSSI_B3   0x9B   // ▄
#define G_RSSI_B4   0x9C   // ▆
#define G_RSSI_B5   0x9D   // █

static const char* const kGlyphTable[32] = {
    "\xe2\x94\x8c",   // 0x80 ┌
    "\xe2\x94\x90",   // 0x81 ┐
    "\xe2\x94\x94",   // 0x82 └
    "\xe2\x94\x98",   // 0x83 ┘
    "\xe2\x94\x80",   // 0x84 ─
    "\xe2\x94\x82",   // 0x85 │
    "\xe2\x94\x9c",   // 0x86 ├
    "\xe2\x94\xa4",   // 0x87 ┤
    "\xe2\x94\xac",   // 0x88 ┬
    "\xe2\x94\xb4",   // 0x89 ┴
    "\xe2\x94\xbc",   // 0x8A ┼
    "\xe2\x95\x90",   // 0x8B ═
    "\xe2\x95\x91",   // 0x8C ║
    "\xe2\x96\x8f",   // 0x8D ▏
    "\xe2\x96\x95",   // 0x8E ▕
    "\xe2\x96\x88",   // 0x8F █
    "\xe2\x96\x8c",   // 0x90 ▌
    "\xe2\x96\x90",   // 0x91 ▐
    "\xe2\x96\x80",   // 0x92 ▀
    "\xe2\x96\x84",   // 0x93 ▄
    "\xc2\xb7",       // 0x94 · (U+00B7, 2-byte UTF-8)
    "\xe2\x96\xb6",   // 0x95 ▶
    "\xe2\x96\xbc",   // 0x96 ▼
    "\xe2\x97\x8f",   // 0x97 ●
    "\xe2\x97\x8b",   // 0x98 ○
    "\xe2\x96\x81",   // 0x99 ▁
    "\xe2\x96\x82",   // 0x9A ▂
    "\xe2\x96\x84",   // 0x9B ▄
    "\xe2\x96\x86",   // 0x9C ▆
    "\xe2\x96\x88",   // 0x9D █
    "",               // 0x9E reserved
    "",               // 0x9F reserved
};

// Place a single decoration glyph (code 0x80..0x9F) bypassing draw_text.
static inline void draw_glyph(int row, int col, uint8_t code, uint8_t fg, uint8_t attr) {
    put_cell(row, col, (char)code, fg, attr);
}

// Unicode box using G_BOX_* glyphs. Inside is left untouched.
static void draw_box_unicode(int row, int col, int rows, int cols,
                             uint8_t fg, uint8_t attr) {
    if (rows < 2 || cols < 2) return;
    draw_glyph(row,            col,            G_BOX_TL, fg, attr);
    draw_glyph(row,            col + cols - 1, G_BOX_TR, fg, attr);
    draw_glyph(row + rows - 1, col,            G_BOX_BL, fg, attr);
    draw_glyph(row + rows - 1, col + cols - 1, G_BOX_BR, fg, attr);
    for (int c = 1; c < cols - 1; c++) {
        draw_glyph(row,            col + c, G_BOX_H, fg, attr);
        draw_glyph(row + rows - 1, col + c, G_BOX_H, fg, attr);
    }
    for (int r = 1; r < rows - 1; r++) {
        draw_glyph(row + r, col,            G_BOX_V, fg, attr);
        draw_glyph(row + r, col + cols - 1, G_BOX_V, fg, attr);
    }
}

// Unicode panel: box + title embedded at top-left as " title ".
static void draw_panel(int row, int col, int rows, int cols,
                       const char* title, uint8_t fg, uint8_t attr) {
    draw_box_unicode(row, col, rows, cols, fg, attr);
    if (title && title[0]) {
        // Write " title " over the top border (overwriting the ─ glyphs)
        char tbuf[64];
        snprintf(tbuf, sizeof(tbuf), " %s ", title);
        int tlen = (int)strlen(tbuf);
        if (tlen > cols - 4) tlen = cols - 4;
        // Use draw_text for ASCII title text, positioned after the TL corner
        for (int i = 0; i < tlen && col + 1 + i < col + cols - 1; i++) {
            put_cell(row, col + 1 + i, tbuf[i], fg, attr);
        }
    }
}

// Tektronix-style dotted leader: "Label ·········· value" right-aligned in `width` cols.
static void draw_dotted_leader(int row, int col, int width,
                               const char* label, const char* value,
                               uint8_t label_fg, uint8_t value_fg, uint8_t value_attr) {
    if (!label) label = "";
    if (!value) value = "";
    int llen = (int)strlen(label);
    int vlen = (int)strlen(value);
    // dot field: width - llen - 1 - 1 - vlen (space before dots, space after)
    int dot_start = col + llen + 1;
    int dot_end   = col + width - vlen - 1;  // one space before value
    // draw label
    draw_text(row, col, label, label_fg, 0);
    // draw dots
    for (int c = dot_start; c < dot_end && c < col + width; c++) {
        draw_glyph(row, c, G_LIGHT_DOT, (uint8_t)TERM_FG_B_BLACK, 0);
    }
    // draw value right-aligned
    int vcol = col + width - vlen;
    if (vcol > dot_start) {
        draw_text(row, vcol, value, value_fg, value_attr);
    }
}

// ===========================================================================
// Diff-based present
// ===========================================================================

// Emit one run of cells with cursor positioning + SGR.
static void emit_run(int row, int col_start, const Cell* cells, int len) {
    char buf[80];
    // 1-indexed terminal coords
    int n = snprintf(buf, sizeof(buf), "\x1b[%d;%dH", row + 1, col_start + 1);
    if (n > 0) term_emit(buf, (size_t)n);

    // Track current SGR so we don't re-emit between identical cells.
    uint8_t cur_fg   = 0xFE;
    uint8_t cur_attr = 0xFE;

    char run[MENU_MAX_COLS + 1];
    int  run_len = 0;

    for (int i = 0; i < len; i++) {
        const Cell* c = &cells[i];
        if (c->fg != cur_fg || c->attr != cur_attr) {
            // flush text run
            if (run_len > 0) {
                term_emit(run, (size_t)run_len);
                run_len = 0;
            }
            // emit SGR reset + new attrs
            int p = snprintf(buf, sizeof(buf), "\x1b[0");
            if (c->attr & ATTR_BOLD)    { p += snprintf(buf + p, sizeof(buf) - p, ";1"); }
            if (c->attr & ATTR_DIM)     { p += snprintf(buf + p, sizeof(buf) - p, ";2"); }
            if (c->attr & ATTR_REVERSE) { p += snprintf(buf + p, sizeof(buf) - p, ";7"); }
            uint8_t fg = c->fg ? c->fg : (uint8_t)TERM_FG_DEFAULT;
            p += snprintf(buf + p, sizeof(buf) - p, ";%um", (unsigned)fg);
            term_emit(buf, (size_t)p);
            cur_fg   = c->fg;
            cur_attr = c->attr;
        }
        uint8_t g = (uint8_t)c->glyph;
        if (g >= 0x80 && g <= 0x9F) {
            // Decoration glyph: flush run, emit UTF-8 sequence directly.
            if (run_len > 0) {
                term_emit(run, (size_t)run_len);
                run_len = 0;
            }
            const char* seq = kGlyphTable[g - 0x80];
            if (seq && seq[0]) term_emit(seq, strlen(seq));
        } else {
            if (run_len < (int)sizeof(run)) {
                run[run_len++] = c->glyph;
            }
        }
    }
    if (run_len > 0) {
        term_emit(run, (size_t)run_len);
    }
}

// Walk back vs front line-by-line, emit changed runs, swap.
static void present(void) {
    if (!s_back || !s_front) return;

    for (int r = 0; r < s_rows; r++) {
        Cell* back_row  = &s_back [r * s_cols];
        Cell* front_row = &s_front[r * s_cols];

        int c = 0;
        while (c < s_cols) {
            // skip identical cells
            while (c < s_cols &&
                   back_row[c].glyph == front_row[c].glyph &&
                   back_row[c].fg    == front_row[c].fg    &&
                   back_row[c].attr  == front_row[c].attr) {
                c++;
            }
            if (c >= s_cols) break;

            int run_start = c;
            while (c < s_cols &&
                   (back_row[c].glyph != front_row[c].glyph ||
                    back_row[c].fg    != front_row[c].fg    ||
                    back_row[c].attr  != front_row[c].attr)) {
                c++;
            }
            int run_len = c - run_start;
            emit_run(r, run_start, &back_row[run_start], run_len);
            // copy run into front
            for (int i = 0; i < run_len; i++) {
                front_row[run_start + i] = back_row[run_start + i];
            }
        }
    }

    // Final SGR reset so any subsequent direct emits start clean.
    static const char kReset[] = "\x1b[0m";
    term_emit(kReset, sizeof(kReset) - 1);
}

// ===========================================================================
// Snapshot
// ===========================================================================

// Slow snapshot: SPI reads, WiFi, HAT — called at ~500 ms cadence.
static void take_snapshot_slow(void) {
    // ADGS fault state — SPI read, done outside mutex to avoid contention
    s_snap.adgsErrorFlags = adgs_read_error_flags();
    s_snap.adgsFaulted    = adgs_is_faulted();

    // HAT status — no mutex needed, hat_get_state() is its own guard
    {
        const HatState* hs = hat_get_state();
        s_snap.hatDetected  = hat_detected();
        s_snap.hatType      = hs ? (uint8_t)hs->type : (uint8_t)HAT_TYPE_NONE;
        s_snap.hatConnected = hs ? hs->connected : false;
    }

    // WiFi status — wifi_manager accessors are lock-free reads
    {
        bool connected = wifi_is_connected();
        if (connected) {
            const char* ip   = wifi_get_sta_ip();
            const char* ssid = wifi_get_sta_ssid();
            strncpy(s_snap.ipStr, ip   ? ip   : "-", sizeof(s_snap.ipStr)  - 1);
            strncpy(s_snap.ssid,  ssid ? ssid : "",  sizeof(s_snap.ssid)   - 1);
            s_snap.ipStr[sizeof(s_snap.ipStr) - 1] = 0;
            s_snap.ssid [sizeof(s_snap.ssid)  - 1] = 0;
            s_snap.rssi = (int8_t)wifi_get_rssi();
        } else {
            strncpy(s_snap.ipStr, "-", sizeof(s_snap.ipStr) - 1);
            s_snap.ipStr[sizeof(s_snap.ipStr) - 1] = 0;
            s_snap.ssid[0] = 0;
            s_snap.rssi = -128;
        }
    }

    // Board profile (own NVS-backed cache, no mutex needed)
    const BoardProfile* bp = board_profile_get_active();
    if (bp) {
        strncpy(s_snap.profileId,   bp->id,   sizeof(s_snap.profileId)   - 1);
        strncpy(s_snap.profileName, bp->name, sizeof(s_snap.profileName) - 1);
        s_snap.profileId  [sizeof(s_snap.profileId)   - 1] = 0;
        s_snap.profileName[sizeof(s_snap.profileName) - 1] = 0;
    } else {
        s_snap.profileId[0]   = 0;
        snprintf(s_snap.profileName, sizeof(s_snap.profileName), "(none)");
    }
}

static bool take_snapshot(void) {
    if (xSemaphoreTake(g_stateMutex, pdMS_TO_TICKS(5)) != pdTRUE) return false;
    s_snap.spiOk             = g_deviceState.spiOk;
    s_snap.i2cOk             = g_deviceState.i2cOk;
    s_snap.dieTemperature    = g_deviceState.dieTemperature;
    s_snap.liveStatus        = g_deviceState.liveStatus;
    s_snap.alertStatus       = g_deviceState.alertStatus;
    s_snap.supplyAlertStatus = g_deviceState.supplyAlertStatus;
    for (int c = 0; c < 4; c++) {
        const ChannelState& src = g_deviceState.channels[c];
        ChanSnap& dst = s_snap.ch[c];
        dst.function           = src.function;
        dst.adcRange           = src.adcRange;
        dst.adcRate            = src.adcRate;
        dst.adcMux             = src.adcMux;
        dst.adcRawCode         = src.adcRawCode;
        dst.adcValue           = src.adcValue;
        dst.dacCode            = src.dacCode;
        dst.dacValue           = src.dacValue;
        dst.doState            = src.doState;
        dst.channelAlertStatus = src.channelAlertStatus;
    }
    for (int d = 0; d < 4; d++) {
        s_snap.diag[d].source = g_deviceState.diag[d].source;
        s_snap.diag[d].value  = g_deviceState.diag[d].value;
    }
    // IDAC voltages (cached values updated by poll task; safe to read)
    s_snap.vlogic = g_deviceState.idac.state[0].target_v;
    s_snap.vadj1  = g_deviceState.idac.state[1].target_v;
    s_snap.vadj2  = g_deviceState.idac.state[2].target_v;
    // USB-PD
    s_snap.pdVoltage  = g_deviceState.usbpd.voltage_v;
    s_snap.pdCurrent  = g_deviceState.usbpd.current_a;
    s_snap.pdAttached = g_deviceState.usbpd.attached;
    // Power tab: PCA9535 state
    {
        const PCA9535State& io = g_deviceState.ioexp;
        for (int i = 0; i < 4; i++) {
            s_snap.efuseEn[i]  = io.efuse_en[i];
            s_snap.efuseFlt[i] = io.efuse_flt[i];
        }
        s_snap.rail15v  = io.en_15v;
        s_snap.muxEn    = io.en_mux;
        s_snap.usbHubEn = io.en_usb_hub;
        s_snap.vadj1Pg  = io.vadj1_pg;
        s_snap.vadj2Pg  = io.vadj2_pg;
        s_snap.logicPg  = io.logic_pg;
    }
    // Signal tab: MUX state (API main devices only)
    adgs_get_api_states(s_snap.muxState);
    xSemaphoreGive(g_stateMutex);

    s_snap.valid = true;
    return true;
}

// ===========================================================================
// Helper labels
// ===========================================================================

static const char* func_label(ChannelFunction f) {
    switch (f) {
        case CH_FUNC_HIGH_IMP:           return "HIGH-Z";
        case CH_FUNC_VOUT:               return "VOUT";
        case CH_FUNC_IOUT:               return "IOUT";
        case CH_FUNC_VIN:                return "VIN";
        case CH_FUNC_IIN_EXT_PWR:        return "IIN_EXT";
        case CH_FUNC_IIN_LOOP_PWR:       return "IIN_LOOP";
        case CH_FUNC_RES_MEAS:           return "RES";
        case CH_FUNC_DIN_LOGIC:          return "DIN_LOG";
        case CH_FUNC_DIN_LOOP:           return "DIN_LOOP";
        case CH_FUNC_IOUT_HART:          return "IOUT_HART";
        case CH_FUNC_IIN_EXT_PWR_HART:   return "IIN_EXT_HART";
        case CH_FUNC_IIN_LOOP_PWR_HART:  return "IIN_LOOP_HART";
    }
    return "?";
}

static const PickerItem kFunctionItems[] = {
    { CH_FUNC_HIGH_IMP,           "HIGH-Z         (safe / disconnected)" },
    { CH_FUNC_VOUT,               "VOUT           (voltage output)" },
    { CH_FUNC_IOUT,               "IOUT           (current output, ext pwr)" },
    { CH_FUNC_VIN,                "VIN            (voltage input / ADC)" },
    { CH_FUNC_IIN_EXT_PWR,        "IIN_EXT_PWR    (current input, ext pwr)" },
    { CH_FUNC_IIN_LOOP_PWR,       "IIN_LOOP_PWR   (current input, loop pwr)" },
    { CH_FUNC_RES_MEAS,           "RES_MEAS       (RTD / resistance)" },
    { CH_FUNC_DIN_LOGIC,          "DIN_LOGIC      (digital input)" },
    { CH_FUNC_DIN_LOOP,           "DIN_LOOP       (digital input, loop pwr)" },
    { CH_FUNC_IOUT_HART,          "IOUT_HART      (current out + HART)" },
    { CH_FUNC_IIN_EXT_PWR_HART,   "IIN_EXT_HART   (current in + HART)" },
    { CH_FUNC_IIN_LOOP_PWR_HART,  "IIN_LOOP_HART  (loop in + HART)" },
};
static const uint8_t kFunctionItemCount =
    sizeof(kFunctionItems) / sizeof(kFunctionItems[0]);

static const PickerItem kUsbPdItems[] = {
    { HUSB238_V_5V,  " 5 V" },
    { HUSB238_V_9V,  " 9 V" },
    { HUSB238_V_12V, "12 V" },
    { HUSB238_V_15V, "15 V" },
    { HUSB238_V_18V, "18 V" },
    { HUSB238_V_20V, "20 V" },
};
static const uint8_t kUsbPdItemCount =
    sizeof(kUsbPdItems) / sizeof(kUsbPdItems[0]);

static const PickerItem kAdcRateItems[] = {
    { ADC_RATE_10SPS_H,    "10 SPS (high reject)" },
    { ADC_RATE_20SPS,      "20 SPS" },
    { ADC_RATE_20SPS_H,    "20 SPS (high reject)" },
    { ADC_RATE_200SPS_H1,  "200 SPS (high reject v1)" },
    { ADC_RATE_200SPS_H,   "200 SPS (high reject)" },
    { ADC_RATE_1_2KSPS,    "1.2 kSPS" },
    { ADC_RATE_1_2KSPS_H,  "1.2 kSPS (high reject)" },
    { ADC_RATE_4_8KSPS,    "4.8 kSPS" },
    { ADC_RATE_9_6KSPS,    "9.6 kSPS" },
};
static const uint8_t kAdcRateItemCount =
    sizeof(kAdcRateItems) / sizeof(kAdcRateItems[0]);

static const PickerItem kAdcMuxItems[] = {
    { ADC_MUX_LF_TO_AGND,      "LF -> AGND" },
    { ADC_MUX_HF_TO_LF,        "HF -> LF (differential)" },
    { ADC_MUX_VSENSEN_TO_AGND, "VSENSE- -> AGND" },
    { ADC_MUX_LF_TO_VSENSEN,   "LF -> VSENSE- (4-wire sense)" },
    { ADC_MUX_AGND_TO_AGND,    "AGND -> AGND (self-test)" },
};
static const uint8_t kAdcMuxItemCount =
    sizeof(kAdcMuxItems) / sizeof(kAdcMuxItems[0]);

static const PickerItem kDiagSourceItems[] = {
    {  0, "AGND" },
    {  1, "Temperature" },
    {  2, "DVCC" },
    {  3, "AVCC" },
    {  4, "LDO 1V8" },
    {  5, "AVDD_HI" },
    {  6, "AVDD_LO" },
    {  7, "AVSS" },
    {  8, "REFOUT" },
    {  9, "ALDO 1V8" },
    { 10, "DLDO 1V8" },
    { 11, "REF1" },
    { 12, "REF2" },
    { 13, "VSENSE" },
};
static const uint8_t kDiagSourceItemCount =
    sizeof(kDiagSourceItems) / sizeof(kDiagSourceItems[0]);

// HAT pin function picker items
static const PickerItem kHatFuncItems[] = {
    { HAT_FUNC_DISCONNECTED, "Disconnected" },
    { HAT_FUNC_GPIO1,        "GPIO1" },
    { HAT_FUNC_GPIO2,        "GPIO2" },
    { HAT_FUNC_GPIO3,        "GPIO3" },
    { HAT_FUNC_GPIO4,        "GPIO4" },
};
static const uint8_t kHatFuncItemCount =
    sizeof(kHatFuncItems) / sizeof(kHatFuncItems[0]);

// Signal tab 2D navigation state
static uint8_t s_signal_dev = 0;
static uint8_t s_signal_sw  = 0;

// Power tab: cached voltage ranges (populated once on tab activation)
static float s_pwr_vadj1_min = 0.0f, s_pwr_vadj1_max = 5.0f;
static float s_pwr_vadj2_min = 0.0f, s_pwr_vadj2_max = 5.0f;
static float s_pwr_vlogic_min = 0.0f, s_pwr_vlogic_max = 5.0f;
static bool  s_pwr_range_cached = false;

static const char* diag_source_label(uint8_t source) {
    for (uint8_t i = 0; i < kDiagSourceItemCount; i++) {
        if ((uint8_t)kDiagSourceItems[i].value == source) {
            return kDiagSourceItems[i].label;
        }
    }
    return "?";
}

// Toast helpers --------------------------------------------------------------

static void show_toast(const char* msg, TermColor color, uint32_t duration_ms = 2500) {
    strncpy(s_toast_msg, msg ? msg : "", sizeof(s_toast_msg) - 1);
    s_toast_msg[sizeof(s_toast_msg) - 1] = 0;
    s_toast_color = color;
    s_toast_set_ms      = millis_now();
    s_toast_duration_ms = duration_ms;
    s_force_redraw = true;
}

// ===========================================================================
// Modal stack management
// ===========================================================================

static ModalFrame* modal_top(void) {
    if (s_modal_top < 0) return nullptr;
    return &s_modal_stack[s_modal_top];
}

static void modal_pop(void) {
    if (s_modal_top < 0) return;
    s_modal_stack[s_modal_top].kind = M_NONE;
    s_modal_top--;
    s_force_redraw = true;
}

static ModalFrame* modal_push(ModalKind kind) {
    if (s_modal_top + 1 >= MODAL_STACK_DEPTH) return nullptr;
    s_modal_top++;
    ModalFrame* f = &s_modal_stack[s_modal_top];
    memset(f, 0, sizeof(*f));
    f->kind = kind;
    s_force_redraw = true;
    return f;
}

static void open_picker(const char* title,
                        const PickerItem* items, uint8_t count,
                        uint8_t initial_selected,
                        PickerCb cb, void* user) {
    ModalFrame* f = modal_push(M_PICKER);
    if (!f) return;
    f->title      = title;
    f->items      = items;
    f->item_count = count;
    f->selected   = (initial_selected < count) ? initial_selected : 0;
    f->scroll     = 0;
    f->picker_cb  = cb;
    f->user       = user;
}

static void open_slider(const char* title, const char* unit,
                        float min, float max, float step, float current,
                        SliderCb cb, void* user) {
    ModalFrame* f = modal_push(M_SLIDER);
    if (!f) return;
    f->title      = title;
    f->unit       = unit ? unit : "";
    f->min        = min;
    f->max        = max;
    f->step       = step;
    f->value      = current;
    if (f->value < min) f->value = min;
    if (f->value > max) f->value = max;
    f->apply_pending = false;
    f->slider_cb  = cb;
    f->user       = user;
}

static void open_confirm(const char* prompt, ConfirmCb cb, void* user) {
    ModalFrame* f = modal_push(M_CONFIRM);
    if (!f) return;
    f->title       = "Confirm";
    f->prompt      = prompt;
    f->confirm_cb  = cb;
    f->confirm_yes = false;        // default to NO
    f->user        = user;
}

// ===========================================================================
// Action callbacks (invoked when modals commit)
// ===========================================================================

static void cb_apply_function(int32_t value, void* user) {
    int ch = (int)(intptr_t)user;
    ChannelFunction f = (ChannelFunction)value;
    tasks_apply_channel_function((uint8_t)ch, f);

    // Fix 2: auto-set ADC mux to match the selected function's natural view.
    // VOUT / VIN: LF->AGND reads the actual output/input voltage.
    // IOUT / IOUT_HART: HF->LF (differential) reads loop current — already
    //   the default after tasks_apply_channel_function(); no override needed.
    if (f == CH_FUNC_VOUT || f == CH_FUNC_VIN) {
        Command c;
        memset(&c, 0, sizeof(c));
        c.type         = CMD_ADC_CONFIG;
        c.channel      = (uint8_t)ch;
        c.adcCfg.mux   = ADC_MUX_LF_TO_AGND;
        c.adcCfg.range = s_snap.ch[ch].adcRange;   // keep user's range
        c.adcCfg.rate  = s_snap.ch[ch].adcRate;    // keep user's rate
        sendCommand(c);
        char msg[80];
        snprintf(msg, sizeof(msg),
                 "CH%d -> %s applied (ADC mux -> LF->AGND for output monitoring)",
                 ch, func_label(f));
        show_toast(msg, TERM_FG_B_GREEN);
    } else {
        char msg[64];
        snprintf(msg, sizeof(msg), "CH%d -> %s applied", ch, func_label(f));
        show_toast(msg, TERM_FG_B_GREEN);
    }
}

static void cb_apply_dac_voltage(float v, void* user) {
    int ch = (int)(intptr_t)user;
    bool ok = tasks_apply_dac_voltage((uint8_t)ch, v, false /* unipolar */);
    char msg[64];
    if (ok) {
        snprintf(msg, sizeof(msg), "CH%d DAC -> %.3f V applied", ch, v);
        show_toast(msg, TERM_FG_B_GREEN);
    } else {
        snprintf(msg, sizeof(msg),
                 "CH%d DAC apply FAILED (not in VOUT mode?)", ch);
        show_toast(msg, TERM_FG_B_RED, 3500);
    }
}

static void cb_clear_all_faults(bool yes, void*) {
    if (!yes) return;
    Command c;
    memset(&c, 0, sizeof(c));
    c.type = CMD_CLEAR_ALERTS;
    sendCommand(c);
    show_toast("Clear-all-faults dispatched", TERM_FG_B_GREEN);
}

static void cb_clear_channel_alert(bool yes, void* user) {
    if (!yes) return;
    int ch = (int)(intptr_t)user;
    Command c;
    memset(&c, 0, sizeof(c));
    c.type    = CMD_CLEAR_CHANNEL_ALERT;
    c.channel = (uint8_t)ch;
    sendCommand(c);
    char msg[48];
    snprintf(msg, sizeof(msg), "CH%d clear-alert dispatched", ch);
    show_toast(msg, TERM_FG_B_GREEN);
}

static void cb_apply_usbpd(int32_t value, void*) {
    Husb238Voltage v = (Husb238Voltage)value;
    bool ok = husb238_select_pdo(v) && husb238_go_command(0x01 /* REQUEST */);
    char msg[48];
    snprintf(msg, sizeof(msg), "USB-PD %s",
             ok ? "renegotiation requested" : "request FAILED");
    show_toast(msg, ok ? TERM_FG_B_GREEN : TERM_FG_B_RED);
}

static void cb_apply_board_profile(int32_t value, void*) {
    uint8_t idx = (uint8_t)value;
    const BoardProfile* p = board_profile_at(idx);
    if (!p) {
        show_toast("Profile index invalid", TERM_FG_B_RED);
        return;
    }
    bool ok = board_profile_select(p->id);
    char msg[64];
    snprintf(msg, sizeof(msg), "Profile %s %s", p->id, ok ? "selected" : "FAILED");
    show_toast(msg, ok ? TERM_FG_B_GREEN : TERM_FG_B_RED);
}

static void cb_apply_adc_rate(int32_t value, void* user) {
    int ch = (int)(intptr_t)user;
    Command c;
    memset(&c, 0, sizeof(c));
    c.type         = CMD_ADC_CONFIG;
    c.channel      = (uint8_t)ch;
    c.adcCfg.mux   = s_snap.ch[ch].adcMux;
    c.adcCfg.range = s_snap.ch[ch].adcRange;
    c.adcCfg.rate  = (AdcRate)value;
    sendCommand(c);
    char msg[64];
    const char* lbl = "?";
    for (uint8_t i = 0; i < kAdcRateItemCount; i++) {
        if (kAdcRateItems[i].value == value) { lbl = kAdcRateItems[i].label; break; }
    }
    snprintf(msg, sizeof(msg), "CH%d ADC rate -> %s", ch, lbl);
    show_toast(msg, TERM_FG_B_GREEN);
}

static void cb_apply_adc_mux(int32_t value, void* user) {
    int ch = (int)(intptr_t)user;
    Command c;
    memset(&c, 0, sizeof(c));
    c.type         = CMD_ADC_CONFIG;
    c.channel      = (uint8_t)ch;
    c.adcCfg.mux   = (AdcConvMux)value;
    c.adcCfg.range = s_snap.ch[ch].adcRange;
    c.adcCfg.rate  = s_snap.ch[ch].adcRate;
    sendCommand(c);
    char msg[64];
    snprintf(msg, sizeof(msg), "CH%d ADC mux dispatched", ch);
    for (uint8_t i = 0; i < kAdcMuxItemCount; i++) {
        if (kAdcMuxItems[i].value == value) {
            snprintf(msg, sizeof(msg), "CH%d ADC mux -> %s", ch, kAdcMuxItems[i].label);
            break;
        }
    }
    show_toast(msg, TERM_FG_B_GREEN);
}

static void cb_apply_diag_slot(int32_t value, void* user) {
    int slot = (int)(intptr_t)user;
    Command c;
    memset(&c, 0, sizeof(c));
    c.type           = CMD_DIAG_CONFIG;
    c.diagCfg.slot   = (uint8_t)slot;
    c.diagCfg.source = (uint8_t)value;
    sendCommand(c);
    char msg[64];
    snprintf(msg, sizeof(msg), "Diag slot %d -> %s dispatched",
             slot, diag_source_label((uint8_t)value));
    show_toast(msg, TERM_FG_B_GREEN);
}

// Power tab callbacks --------------------------------------------------------

static void cb_apply_vadj1(float v, void*) {
    bool ok = ds4424_set_voltage(1, v);
    char msg[64];
    snprintf(msg, sizeof(msg), "VADJ1 -> %.3f V %s", v, ok ? "applied" : "FAILED");
    show_toast(msg, ok ? TERM_FG_B_GREEN : TERM_FG_B_RED);
}

static void cb_apply_vadj2(float v, void*) {
    bool ok = ds4424_set_voltage(2, v);
    char msg[64];
    snprintf(msg, sizeof(msg), "VADJ2 -> %.3f V %s", v, ok ? "applied" : "FAILED");
    show_toast(msg, ok ? TERM_FG_B_GREEN : TERM_FG_B_RED);
}

static void cb_apply_vlogic(float v, void*) {
    bool ok = ds4424_set_voltage(0, v);
    char msg[64];
    snprintf(msg, sizeof(msg), "VLOGIC -> %.3f V %s", v, ok ? "applied" : "FAILED");
    show_toast(msg, ok ? TERM_FG_B_GREEN : TERM_FG_B_RED);
}

static void cb_toggle_efuse(bool yes, void* user) {
    if (!yes) return;
    int idx = (int)(intptr_t)user;  // 0-based
    static const PcaControl kEfuseCtrl[4] = {
        PCA_CTRL_EFUSE1_EN, PCA_CTRL_EFUSE2_EN,
        PCA_CTRL_EFUSE3_EN, PCA_CTRL_EFUSE4_EN
    };
    bool new_state = !s_snap.efuseEn[idx];
    bool ok = pca9535_set_control(kEfuseCtrl[idx], new_state);
    char msg[64];
    snprintf(msg, sizeof(msg), "E-Fuse %d -> %s %s",
             idx + 1, new_state ? "ON" : "OFF", ok ? "" : "(FAILED)");
    show_toast(msg, ok ? TERM_FG_B_GREEN : TERM_FG_B_RED);
}

static void cb_toggle_15v(bool yes, void*) {
    if (!yes) return;
    bool new_state = !s_snap.rail15v;
    bool ok = pca9535_set_control(PCA_CTRL_15V_EN, new_state);
    char msg[48];
    snprintf(msg, sizeof(msg), "15V rail -> %s %s",
             new_state ? "ON" : "OFF", ok ? "" : "(FAILED)");
    show_toast(msg, ok ? TERM_FG_B_GREEN : TERM_FG_B_RED);
}

static void cb_toggle_mux(bool yes, void*) {
    if (!yes) return;
    bool new_state = !s_snap.muxEn;
    bool ok = pca9535_set_control(PCA_CTRL_MUX_EN, new_state);
    char msg[48];
    snprintf(msg, sizeof(msg), "MUX pwr -> %s %s",
             new_state ? "ON" : "OFF", ok ? "" : "(FAILED)");
    show_toast(msg, ok ? TERM_FG_B_GREEN : TERM_FG_B_RED);
}

static void cb_toggle_usbhub(bool yes, void*) {
    if (!yes) return;
    bool new_state = !s_snap.usbHubEn;
    bool ok = pca9535_set_control(PCA_CTRL_USB_HUB_EN, new_state);
    char msg[48];
    snprintf(msg, sizeof(msg), "USB Hub -> %s %s",
             new_state ? "ON" : "OFF", ok ? "" : "(FAILED)");
    show_toast(msg, ok ? TERM_FG_B_GREEN : TERM_FG_B_RED);
}

// Signal tab callbacks -------------------------------------------------------

static void cb_signal_reset_all(bool yes, void*) {
    if (!yes) return;
    adgs_reset_all();
    show_toast("All switches opened", TERM_FG_B_GREEN);
}

// HAT tab callbacks ----------------------------------------------------------

static void cb_hat_set_pin(int32_t value, void* user) {
    int pin = (int)(intptr_t)user;
    HatPinFunction func = (HatPinFunction)value;
    bool ok = hat_set_pin((uint8_t)pin, func);
    char msg[64];
    snprintf(msg, sizeof(msg), "EXP_EXT%d -> %s %s",
             pin + 1, hat_func_name(func), ok ? "" : "(FAILED)");
    show_toast(msg, ok ? TERM_FG_B_GREEN : TERM_FG_B_RED);
}

static void cb_hat_conn_a(bool yes, void*) {
    if (!yes) return;
    const HatState* hs = hat_get_state();
    bool new_on = hs ? !hs->connector[HAT_CONNECTOR_A].enabled : true;
    bool ok = hat_set_power(HAT_CONNECTOR_A, new_on);
    char msg[48];
    snprintf(msg, sizeof(msg), "Connector A -> %s %s",
             new_on ? "ON" : "OFF", ok ? "" : "(FAILED)");
    show_toast(msg, ok ? TERM_FG_B_GREEN : TERM_FG_B_RED);
}

static void cb_hat_conn_b(bool yes, void*) {
    if (!yes) return;
    const HatState* hs = hat_get_state();
    bool new_on = hs ? !hs->connector[HAT_CONNECTOR_B].enabled : true;
    bool ok = hat_set_power(HAT_CONNECTOR_B, new_on);
    char msg[48];
    snprintf(msg, sizeof(msg), "Connector B -> %s %s",
             new_on ? "ON" : "OFF", ok ? "" : "(FAILED)");
    show_toast(msg, ok ? TERM_FG_B_GREEN : TERM_FG_B_RED);
}

static void cb_hat_io_volt(float v, void*) {
    uint16_t mv = (uint16_t)(v * 1000.0f + 0.5f);
    bool ok = hat_set_io_voltage(mv);
    char msg[64];
    snprintf(msg, sizeof(msg), "HAT I/O volt -> %u mV %s", mv, ok ? "" : "(FAILED)");
    show_toast(msg, ok ? TERM_FG_B_GREEN : TERM_FG_B_RED);
}

static void cb_hat_reset(bool yes, void*) {
    if (!yes) return;
    bool ok = hat_reset();
    show_toast(ok ? "HAT reset OK" : "HAT reset FAILED",
               ok ? TERM_FG_B_GREEN : TERM_FG_B_RED);
}

// Calibration progress modal tick callback ----------------------------------

static void cal_progress_tick(ModalFrame* f, void*) {
    if (f->progress_cancel) {
        f->progress_status = "Cancelled (calibration may still be running)";
        f->progress_done   = true;
        f->progress_failed = true;
        return;
    }
    const SelftestCalResult* r = selftest_get_cal_result();
    if (!r) return;
    switch (r->status) {
        case CAL_STATUS_IDLE:
            f->progress_status  = "Idle";
            f->progress_percent = 0;
            f->progress_done    = true;
            break;
        case CAL_STATUS_RUNNING:
            f->progress_status  = "Calibrating...";
            // points_collected out of DS4424_CAL_MAX_POINTS
            f->progress_percent = (r->points_collected * 100) / 16;
            if (f->progress_percent > 99) f->progress_percent = 99;
            break;
        case CAL_STATUS_SUCCESS:
            f->progress_status  = "Calibration successful";
            f->progress_percent = 100;
            f->progress_done    = true;
            f->progress_failed  = false;
            break;
        case CAL_STATUS_FAILED:
            f->progress_status  = "Calibration FAILED";
            f->progress_percent = 100;
            f->progress_done    = true;
            f->progress_failed  = true;
            break;
    }
}

static void open_progress(const char* title,
                          void (*tick)(ModalFrame*, void*), void* user) {
    ModalFrame* f = modal_push(M_PROGRESS);
    if (!f) return;
    f->title            = title;
    f->progress_status  = "Starting...";
    f->progress_percent = 0;
    f->progress_done    = false;
    f->progress_failed  = false;
    f->progress_cancel  = false;
    f->progress_tick    = tick;
    f->user             = user;
}

// Settings calibration callbacks --------------------------------------------

static void cb_cal_save(bool yes, void*) {
    if (!yes) return;
    bool ok = ds4424_cal_save();
    show_toast(ok ? "Cal saved to NVS" : "Cal save FAILED",
               ok ? TERM_FG_B_GREEN : TERM_FG_B_RED);
}

static void cb_cal_load(bool yes, void*) {
    if (!yes) return;
    bool ok = ds4424_cal_load();
    show_toast(ok ? "Cal loaded from NVS" : "Cal load FAILED",
               ok ? TERM_FG_B_GREEN : TERM_FG_B_RED);
}

static void cb_cal_clear(bool yes, void*) {
    if (!yes) return;
    for (uint8_t ch = 0; ch < 3; ch++) ds4424_cal_clear(ch);
    show_toast("Cal data cleared for all channels", TERM_FG_B_GREEN);
}

// ===========================================================================
// Activations: triggered by Enter on a focused field.
// ===========================================================================

static void activate_channel_field(int ch, ChannelField field) {
    switch (field) {
        case CH_FIELD_FUNCTION: {
            // find current selection index
            uint8_t sel = 0;
            for (uint8_t i = 0; i < kFunctionItemCount; i++) {
                if (kFunctionItems[i].value == (int32_t)s_snap.ch[ch].function) {
                    sel = i;
                    break;
                }
            }
            static char title[40];
            snprintf(title, sizeof(title), "CH%d Function", ch);
            open_picker(title, kFunctionItems, kFunctionItemCount, sel,
                        cb_apply_function, (void*)(intptr_t)ch);
            break;
        }
        case CH_FIELD_ADC_RATE: {
            uint8_t sel = 0;
            for (uint8_t i = 0; i < kAdcRateItemCount; i++) {
                if (kAdcRateItems[i].value == (int32_t)s_snap.ch[ch].adcRate) {
                    sel = i;
                    break;
                }
            }
            static char title_rate[40];
            snprintf(title_rate, sizeof(title_rate), "CH%d ADC Sample Rate", ch);
            open_picker(title_rate, kAdcRateItems, kAdcRateItemCount, sel,
                        cb_apply_adc_rate, (void*)(intptr_t)ch);
            break;
        }
        case CH_FIELD_ADC_MUX: {
            uint8_t sel = 0;
            for (uint8_t i = 0; i < kAdcMuxItemCount; i++) {
                if (kAdcMuxItems[i].value == (int32_t)s_snap.ch[ch].adcMux) {
                    sel = i;
                    break;
                }
            }
            static char title_mux[40];
            snprintf(title_mux, sizeof(title_mux), "CH%d ADC Input Mux", ch);
            open_picker(title_mux, kAdcMuxItems, kAdcMuxItemCount, sel,
                        cb_apply_adc_mux, (void*)(intptr_t)ch);
            break;
        }
        case CH_FIELD_DAC_VOLTAGE: {
            static char title[40];
            snprintf(title, sizeof(title), "CH%d DAC Voltage Setpoint", ch);
            open_slider(title, "V",
                        0.0f, 12.0f, 0.010f,
                        s_snap.ch[ch].dacValue,
                        cb_apply_dac_voltage, (void*)(intptr_t)ch);
            break;
        }
        case CH_FIELD_DO_TOGGLE: {
            // Immediate toggle (safe, idempotent).
            Command c;
            memset(&c, 0, sizeof(c));
            c.type    = CMD_DO_SET;
            c.channel = (uint8_t)ch;
            c.boolVal = !s_snap.ch[ch].doState;
            sendCommand(c);
            char msg[48];
            snprintf(msg, sizeof(msg), "CH%d DO -> %s", ch,
                     c.boolVal ? "ON" : "OFF");
            show_toast(msg, TERM_FG_B_GREEN);
            break;
        }
        case CH_FIELD_CLEAR_ALERT: {
            static char prompt[64];
            snprintf(prompt, sizeof(prompt),
                     "Clear CH%d channel-alert register?", ch);
            open_confirm(prompt, cb_clear_channel_alert,
                         (void*)(intptr_t)ch);
            break;
        }
        default: break;
    }
}

static void activate_settings_field(SettingsField field) {
    switch (field) {
        case SET_FIELD_CLEAR_FAULTS:
            open_confirm("Clear ALL AD74416H alert registers?",
                         cb_clear_all_faults, nullptr);
            break;
        case SET_FIELD_USBPD: {
            // initial selection: closest to current contract
            static const int kUsbPdVoltsLut[] = {5,9,12,15,18,20};
            uint8_t sel = 0;
            float v = s_snap.pdVoltage;
            float best = 1e9f;
            for (uint8_t i = 0; i < kUsbPdItemCount; i++) {
                float candidate = (float)kUsbPdVoltsLut[i];
                float d = fabsf(candidate - v);
                if (d < best) { best = d; sel = i; }
            }
            open_picker("USB-PD Voltage",
                        kUsbPdItems, kUsbPdItemCount, sel,
                        cb_apply_usbpd, nullptr);
            break;
        }
        case SET_FIELD_BOARD_PROFILE: {
            // Build a transient picker list — store it statically for the
            // duration of the modal (kBoardItems holds value=index).
            static PickerItem s_board_items[8];
            static char       s_board_labels[8][40];
            uint8_t count = board_profile_count();
            if (count > 8) count = 8;
            uint8_t sel = 0;
            const BoardProfile* active = board_profile_get_active();
            for (uint8_t i = 0; i < count; i++) {
                const BoardProfile* p = board_profile_at(i);
                if (!p) continue;
                snprintf(s_board_labels[i], sizeof(s_board_labels[0]),
                         "%-12s  %s", p->id, p->name);
                s_board_items[i].value = i;
                s_board_items[i].label = s_board_labels[i];
                if (active && strcmp(active->id, p->id) == 0) sel = i;
            }
            open_picker("Board Profile",
                        s_board_items, count, sel,
                        cb_apply_board_profile, nullptr);
            break;
        }
        case SET_FIELD_DIAG_0:
        case SET_FIELD_DIAG_1:
        case SET_FIELD_DIAG_2:
        case SET_FIELD_DIAG_3: {
            int slot = (int)field - (int)SET_FIELD_DIAG_0;
            uint8_t sel = 0;
            uint8_t cur_src = s_snap.diag[slot].source;
            for (uint8_t i = 0; i < kDiagSourceItemCount; i++) {
                if ((uint8_t)kDiagSourceItems[i].value == cur_src) {
                    sel = i;
                    break;
                }
            }
            static char title_diag[40];
            snprintf(title_diag, sizeof(title_diag), "Diag Slot %d Source", slot);
            open_picker(title_diag, kDiagSourceItems, kDiagSourceItemCount, sel,
                        cb_apply_diag_slot, (void*)(intptr_t)slot);
            break;
        }
        case SET_FIELD_CAL_VADJ1:
            if (selftest_start_auto_calibrate(1)) {
                static char cal_title[40];
                snprintf(cal_title, sizeof(cal_title), "Calibrating VADJ1...");
                open_progress(cal_title, cal_progress_tick, nullptr);
            } else {
                show_toast("Cal start FAILED (busy or interlock)", TERM_FG_B_RED);
            }
            break;
        case SET_FIELD_CAL_VADJ2:
            if (selftest_start_auto_calibrate(2)) {
                static char cal_title2[40];
                snprintf(cal_title2, sizeof(cal_title2), "Calibrating VADJ2...");
                open_progress(cal_title2, cal_progress_tick, nullptr);
            } else {
                show_toast("Cal start FAILED (busy or interlock)", TERM_FG_B_RED);
            }
            break;
        case SET_FIELD_CAL_SAVE:
            open_confirm("Save calibration to NVS?", cb_cal_save, nullptr);
            break;
        case SET_FIELD_CAL_LOAD:
            open_confirm("Load calibration from NVS?", cb_cal_load, nullptr);
            break;
        case SET_FIELD_CAL_CLEAR:
            open_confirm("Clear ALL calibration data (ch 0-2)?", cb_cal_clear, nullptr);
            break;
        default: break;
    }
}

static void activate_power_field(PowerField field) {
    if (!s_pwr_range_cached) {
        ds4424_get_range(1, &s_pwr_vadj1_min,  &s_pwr_vadj1_max);
        ds4424_get_range(2, &s_pwr_vadj2_min,  &s_pwr_vadj2_max);
        ds4424_get_range(0, &s_pwr_vlogic_min, &s_pwr_vlogic_max);
        s_pwr_range_cached = true;
    }
    switch (field) {
        case PWR_FIELD_VADJ1:
            open_slider("VADJ1 Voltage", "V",
                        s_pwr_vadj1_min, s_pwr_vadj1_max, 0.050f,
                        s_snap.vadj1, cb_apply_vadj1, nullptr);
            break;
        case PWR_FIELD_VADJ2:
            open_slider("VADJ2 Voltage", "V",
                        s_pwr_vadj2_min, s_pwr_vadj2_max, 0.050f,
                        s_snap.vadj2, cb_apply_vadj2, nullptr);
            break;
        case PWR_FIELD_VLOGIC:
            open_slider("Level-Shifter Voltage", "V",
                        s_pwr_vlogic_min, s_pwr_vlogic_max, 0.050f,
                        s_snap.vlogic, cb_apply_vlogic, nullptr);
            break;
        case PWR_FIELD_EFUSE1:
        case PWR_FIELD_EFUSE2:
        case PWR_FIELD_EFUSE3:
        case PWR_FIELD_EFUSE4: {
            int idx = (int)field - (int)PWR_FIELD_EFUSE1;
            static char pwr_prompt[64];
            snprintf(pwr_prompt, sizeof(pwr_prompt),
                     "Toggle E-Fuse %d (%s -> %s)?",
                     idx + 1,
                     s_snap.efuseEn[idx] ? "ON" : "OFF",
                     s_snap.efuseEn[idx] ? "OFF" : "ON");
            open_confirm(pwr_prompt, cb_toggle_efuse, (void*)(intptr_t)idx);
            break;
        }
        case PWR_FIELD_15V:
            open_confirm(s_snap.rail15v ? "Disable 15V rail?" : "Enable 15V rail?",
                         cb_toggle_15v, nullptr);
            break;
        case PWR_FIELD_MUX:
            open_confirm(s_snap.muxEn ? "Disable MUX power?" : "Enable MUX power?",
                         cb_toggle_mux, nullptr);
            break;
        case PWR_FIELD_USBHUB:
            open_confirm(s_snap.usbHubEn ? "Disable USB Hub?" : "Enable USB Hub?",
                         cb_toggle_usbhub, nullptr);
            break;
        default: break;
    }
}

static void activate_signal_field(int field) {
    if (field == SIGNAL_RESET_FIELD) {
        // Reset-all row (field 0)
        open_confirm("Open all 32 switches (reset)?", cb_signal_reset_all, nullptr);
    } else {
        int gf  = field - SIGNAL_GRID_BASE;
        int dev = gf / 8;
        int sw  = gf % 8;
        bool cur = (s_snap.muxState[dev] >> sw) & 1;
        bool ok = adgs_set_api_switch_safe((uint8_t)dev, (uint8_t)sw, !cur);
        char msg[64];
        snprintf(msg, sizeof(msg), "MUX%d SW%d -> %s %s",
                 dev, sw, !cur ? "CLOSED" : "OPEN", ok ? "" : "(FAILED)");
        show_toast(msg, ok ? TERM_FG_B_GREEN : TERM_FG_B_RED);
    }
}

static void activate_hat_field(HatTabField field) {
    switch (field) {
        case HAT_FIELD_PIN0:
        case HAT_FIELD_PIN1:
        case HAT_FIELD_PIN2:
        case HAT_FIELD_PIN3: {
            int pin = (int)field - (int)HAT_FIELD_PIN0;
            const HatState* hs = hat_get_state();
            HatPinFunction cur = hs ? hs->pin_config[pin] : HAT_FUNC_DISCONNECTED;
            uint8_t sel = 0;
            for (uint8_t i = 0; i < kHatFuncItemCount; i++) {
                if (kHatFuncItems[i].value == (int32_t)cur) { sel = i; break; }
            }
            static char hat_pin_title[40];
            snprintf(hat_pin_title, sizeof(hat_pin_title), "EXP_EXT%d Function", pin + 1);
            open_picker(hat_pin_title, kHatFuncItems, kHatFuncItemCount, sel,
                        cb_hat_set_pin, (void*)(intptr_t)pin);
            break;
        }
        case HAT_FIELD_CONN_A:
            open_confirm("Toggle Connector A power?", cb_hat_conn_a, nullptr);
            break;
        case HAT_FIELD_CONN_B:
            open_confirm("Toggle Connector B power?", cb_hat_conn_b, nullptr);
            break;
        case HAT_FIELD_IO_VOLT: {
            const HatState* hs = hat_get_state();
            float cur_v = hs ? (hs->io_voltage_mv / 1000.0f) : 3.3f;
            open_slider("HAT I/O Voltage", "V",
                        1.2f, 5.5f, 0.1f, cur_v,
                        cb_hat_io_volt, nullptr);
            break;
        }
        case HAT_FIELD_DETECT: {
            HatType t = hat_detect();
            char msg[64];
            snprintf(msg, sizeof(msg), "HAT type: %s", hat_type_name(t));
            show_toast(msg, TERM_FG_B_CYAN);
            break;
        }
        case HAT_FIELD_RESET:
            open_confirm("Reset HAT to defaults?", cb_hat_reset, nullptr);
            break;
        default: break;
    }
}

// ===========================================================================
// Tab rendering
// ===========================================================================

// --- Header bar (Fix 2) -----------------------------------------------------
// Row 0: double-line divider (═══ …)
// Row 1: BUGBUSTER branding | FW version + uptime | MAC + RSSI

static void render_header_bar(void) {
    // Row 0: full-width double horizontal rule
    for (int c = 0; c < s_cols; c++) {
        draw_glyph(0, c, G_BOX_DBL_H, (uint8_t)TERM_FG_B_BLACK, 0);
    }

    // Row 1: three sections
    draw_fill(1, 0, 1, s_cols, ' ', 0, 0);

    // Left: " BUGBUSTER · ESP32-S3"
    draw_text(1, 1, "BUGBUSTER", (uint8_t)TERM_FG_B_CYAN, ATTR_BOLD);
    draw_text(1, 10, " \xc2\xb7 ESP32-S3", (uint8_t)TERM_FG_B_BLACK, ATTR_DIM);

    // Center: "FW 3.0.0 · UP HH:MM:SS"
    {
        uint64_t uptime_s = (uint64_t)(esp_timer_get_time() / 1000000ULL);
        unsigned hh = (unsigned)(uptime_s / 3600);
        unsigned mm = (unsigned)((uptime_s % 3600) / 60);
        unsigned ss = (unsigned)(uptime_s % 60);
        char center[48];
        snprintf(center, sizeof(center), "FW %d.%d.%d \xc2\xb7 UP %02u:%02u:%02u",
                 BBP_FW_VERSION_MAJOR, BBP_FW_VERSION_MINOR, BBP_FW_VERSION_PATCH,
                 hh, mm, ss);
        int clen = (int)strlen(center);
        int ccol = (s_cols - clen) / 2;
        if (ccol < 22) ccol = 22;
        draw_text(1, ccol, center, (uint8_t)TERM_FG_B_BLACK, 0);
    }

    // Right: MAC (last 2 octets abbreviated) + RSSI bars
    {
        // Get MAC
        uint8_t mac[6] = {0};
        esp_read_mac(mac, ESP_MAC_WIFI_STA);
        // RSSI bars
        int8_t rssi = s_snap.rssi;
        bool wifi_ok = (rssi != -128);
        uint8_t rssi_bar = 0;
        if (wifi_ok) {
            if      (rssi >= -55) rssi_bar = 5;
            else if (rssi >= -65) rssi_bar = 4;
            else if (rssi >= -75) rssi_bar = 3;
            else if (rssi >= -85) rssi_bar = 2;
            else                  rssi_bar = 1;
        }
        // Compose right section
        char right[32];
        snprintf(right, sizeof(right), "MAC %02X:%02X:..",
                 mac[0], mac[1]);
        int rlen = (int)strlen(right) + (wifi_ok ? rssi_bar + 1 : 0);
        int rcol = s_cols - rlen - 2;
        if (rcol < 2) rcol = 2;
        draw_text(1, rcol, right, (uint8_t)TERM_FG_B_BLACK, ATTR_DIM);
        if (wifi_ok) {
            // Draw signal bars using stacked block glyphs
            static const uint8_t kRssiBars[5] = {
                G_RSSI_B1, G_RSSI_B2, G_RSSI_B3, G_RSSI_B4, G_RSSI_B5
            };
            int bcol = rcol + (int)strlen(right) + 1;
            for (uint8_t b = 0; b < rssi_bar && b < 5; b++) {
                draw_glyph(1, bcol + b, kRssiBars[b],
                           (uint8_t)TERM_FG_B_GREEN, 0);
            }
        }
    }
}

static void render_tab_bar(void) {
    // Row 2: tab strip (shifted +2 vs original row 0).
    draw_fill(2, 0, 1, s_cols, ' ', 0, 0);
    int col = 1;
    for (int i = 0; i < NUM_TABS; i++) {
        if (col + 14 >= s_cols) break;
        bool sel = (i == (int)s_tab);

        // Determine badge for this tab
        bool badge = false;
        if (i >= TAB_CH0 && i <= TAB_CH3) {
            int ch = i - TAB_CH0;
            badge = (s_snap.ch[ch].channelAlertStatus != 0);
        } else if (i == TAB_SIGNAL) {
            badge = s_snap.adgsFaulted;
        } else if (i == TAB_SETTINGS) {
            badge = (s_snap.alertStatus != 0);
        }

        char name_buf[20];
        if (badge) {
            snprintf(name_buf, sizeof(name_buf), "%s[!]", kTabNames[i]);
        } else {
            snprintf(name_buf, sizeof(name_buf), "%s   ", kTabNames[i]);
        }
        // Pad to fixed width so tab positions don't shift
        int name_w = (int)strlen(kTabNames[i]) + 3;  // always same width

        char buf[24];
        if (sel) {
            snprintf(buf, sizeof(buf), "[ %s ]", name_buf);
        } else {
            snprintf(buf, sizeof(buf), "  %s  ", name_buf);
        }

        if (sel) {
            // Draw base tab in cyan
            draw_text(2, col, buf, TERM_FG_B_CYAN, ATTR_BOLD);
            // Overlay badge in red if present
            if (badge) {
                int badge_col = col + 3 + (int)strlen(kTabNames[i]);
                draw_text(2, badge_col, "[!]", TERM_FG_B_RED, ATTR_BOLD);
            }
        } else {
            draw_text(2, col, buf, TERM_FG_B_BLACK, 0);
            if (badge) {
                int badge_col = col + 2 + (int)strlen(kTabNames[i]);
                draw_text(2, badge_col, "[!]", TERM_FG_B_RED, ATTR_BOLD);
            }
        }
        col += name_w + 4;  // consistent spacing: "  " + name + "   " + "  " + 1 gap
        (void)name_w;
        col = col - name_w - 4 + (int)strlen(buf) + 1;
    }
    // Row 3: separator (shifted from row 1).
    draw_hline(3, 0, s_cols, '-', TERM_FG_B_BLACK, 0);
}

static void render_status_strip(void) {
    // Bottom-of-screen: footer separator + key hints + toast.
    int sep_row    = s_rows - 3;
    int hint_row   = s_rows - 2;
    int toast_row  = s_rows - 1;
    if (sep_row < 6) return;  // need at least header+tab rows above

    draw_hline(sep_row, 0, s_cols, '-', TERM_FG_B_BLACK, 0);

    draw_fill(hint_row, 0, 1, s_cols, ' ', 0, 0);
    const char* hint =
        " <Left/Right> tabs   <Up/Down> field   <Enter> open   <q>/<ESC ESC> quit   <r> redraw   <?> help";
    draw_text(hint_row, 1, hint, TERM_FG_B_BLACK, 0);

    draw_fill(toast_row, 0, 1, s_cols, ' ', 0, 0);
    if (s_toast_msg[0] && (uint32_t)(millis_now() - s_toast_set_ms) < s_toast_duration_ms) {
        draw_text(toast_row, 1, " > ", s_toast_color, ATTR_BOLD);
        draw_text(toast_row, 4, s_toast_msg, s_toast_color, ATTR_BOLD);
    } else {
        // Live system status strip — rendered field-by-field with per-field colour.
        // Priority order (never dropped): SPI, I2C, Alert, PD.
        // Dropped right-to-left when line would overflow: Heap, Q:N, Profile, Temp.
        int max_col = s_cols - 2;
        int col = 1;

        // Helper lambda-style macro: draw one field if it fits, advance col.
        // We use a local struct approach to keep it readable without C++ lambdas.
        char fbuf[48];
        uint8_t ffg;

        // --- SPI ---
        snprintf(fbuf, sizeof(fbuf), " SPI:%s", s_snap.spiOk ? "OK" : "FAIL");
        ffg = s_snap.spiOk ? (uint8_t)TERM_FG_B_BLACK : (uint8_t)TERM_FG_B_RED;
        if (col + (int)strlen(fbuf) <= max_col) {
            draw_text(toast_row, col, fbuf, ffg, 0);
            col += (int)strlen(fbuf);
        }

        // --- I2C ---
        snprintf(fbuf, sizeof(fbuf), "  I2C:%s", s_snap.i2cOk ? "OK" : "FAIL");
        ffg = s_snap.i2cOk ? (uint8_t)TERM_FG_B_BLACK : (uint8_t)TERM_FG_B_RED;
        if (col + (int)strlen(fbuf) <= max_col) {
            draw_text(toast_row, col, fbuf, ffg, 0);
            col += (int)strlen(fbuf);
        }

        // --- Alert ---
        snprintf(fbuf, sizeof(fbuf), "  Alert:0x%04X", s_snap.alertStatus);
        ffg = s_snap.alertStatus ? (uint8_t)TERM_FG_B_YELLOW : (uint8_t)TERM_FG_B_BLACK;
        if (col + (int)strlen(fbuf) <= max_col) {
            draw_text(toast_row, col, fbuf, ffg, 0);
            col += (int)strlen(fbuf);
        }

        // --- PD ---
        snprintf(fbuf, sizeof(fbuf), "  PD:%.1fV/%.2fA", s_snap.pdVoltage, s_snap.pdCurrent);
        ffg = (uint8_t)TERM_FG_B_BLACK;
        if (col + (int)strlen(fbuf) <= max_col) {
            draw_text(toast_row, col, fbuf, ffg, 0);
            col += (int)strlen(fbuf);
        }

        // --- Heap (sampled live, not snapshotted) ---
        uint32_t heap_k = esp_get_free_heap_size() / 1024;
        snprintf(fbuf, sizeof(fbuf), "  Heap:%uk", (unsigned)heap_k);
        ffg = (heap_k < 20) ? (uint8_t)TERM_FG_B_RED
            : (heap_k < 50) ? (uint8_t)TERM_FG_B_YELLOW
            :                  (uint8_t)TERM_FG_B_BLACK;
        if (col + (int)strlen(fbuf) <= max_col) {
            draw_text(toast_row, col, fbuf, ffg, 0);
            col += (int)strlen(fbuf);
        }

        // --- Command queue depth ---
        UBaseType_t qd = uxQueueMessagesWaiting(g_cmdQueue);
        snprintf(fbuf, sizeof(fbuf), "  Q:%u", (unsigned)qd);
        ffg = (qd > 4) ? (uint8_t)TERM_FG_B_RED
            : (qd > 0) ? (uint8_t)TERM_FG_B_YELLOW
            :             (uint8_t)TERM_FG_B_BLACK;
        if (col + (int)strlen(fbuf) <= max_col) {
            draw_text(toast_row, col, fbuf, ffg, 0);
            col += (int)strlen(fbuf);
        }

        // --- Profile ---
        snprintf(fbuf, sizeof(fbuf), "  Profile:%s", s_snap.profileName);
        ffg = (uint8_t)TERM_FG_B_BLACK;
        if (col + (int)strlen(fbuf) <= max_col) {
            draw_text(toast_row, col, fbuf, ffg, 0);
            col += (int)strlen(fbuf);
        }

        // --- Temp ---
        snprintf(fbuf, sizeof(fbuf), "  Temp:%.1fC", s_snap.dieTemperature);
        ffg = (uint8_t)TERM_FG_B_BLACK;
        if (col + (int)strlen(fbuf) <= max_col) {
            draw_text(toast_row, col, fbuf, ffg, 0);
        }
    }
}

// --- Overview tab -----------------------------------------------------------

static void render_overview(void) {
    // Panel wrapping (Fix 3): content area starts at row 4 (below tab bar row 2 + separator row 3)
    int panel_row  = 4;
    int panel_col  = 1;
    int panel_rows = s_rows - 4 - panel_row;   // leaves hint+status strip below
    int panel_cols = s_cols - 2;
    if (panel_rows < 4) panel_rows = 4;
    draw_panel(panel_row, panel_col, panel_rows, panel_cols,
               kTabNames[TAB_OVERVIEW], (uint8_t)TERM_FG_B_BLACK, 0);

    int row = panel_row + 1;   // first content row inside panel
    int lc  = panel_col + 2;   // left content column

    // Left column: system status using dotted leaders (Fix 4)
    {
        char vbuf[32];
        // SPI
        uint8_t spi_fg = s_snap.spiOk ? (uint8_t)TERM_FG_B_GREEN : (uint8_t)TERM_FG_B_RED;
        snprintf(vbuf, sizeof(vbuf), "%s", s_snap.spiOk ? "OK" : "FAIL");
        draw_dotted_leader(row, lc, 38, "SPI", vbuf, (uint8_t)TERM_FG_DEFAULT, spi_fg, 0);

        // Die temp
        snprintf(vbuf, sizeof(vbuf), "%.1f C", s_snap.dieTemperature);
        draw_dotted_leader(row+1, lc, 38, "Die temp", vbuf, (uint8_t)TERM_FG_DEFAULT, (uint8_t)TERM_FG_DEFAULT, 0);

        // Live status
        snprintf(vbuf, sizeof(vbuf), "0x%04X", s_snap.liveStatus);
        draw_dotted_leader(row+2, lc, 38, "Live status", vbuf, (uint8_t)TERM_FG_DEFAULT, (uint8_t)TERM_FG_DEFAULT, 0);

        // Alert status
        uint8_t al_fg = s_snap.alertStatus ? (uint8_t)TERM_FG_B_YELLOW : (uint8_t)TERM_FG_DEFAULT;
        snprintf(vbuf, sizeof(vbuf), "0x%04X", s_snap.alertStatus);
        draw_dotted_leader(row+3, lc, 38, "Alert status", vbuf, (uint8_t)TERM_FG_DEFAULT, al_fg, 0);

        // Supply alert
        uint8_t sa_fg = s_snap.supplyAlertStatus ? (uint8_t)TERM_FG_B_YELLOW : (uint8_t)TERM_FG_DEFAULT;
        snprintf(vbuf, sizeof(vbuf), "0x%04X", s_snap.supplyAlertStatus);
        draw_dotted_leader(row+4, lc, 38, "Supply alert", vbuf, (uint8_t)TERM_FG_DEFAULT, sa_fg, 0);
    }

    // Right column: supply rails + PD (Fix 4, dotted leaders)
    int rc = lc + 40;   // right column start
    int rw = panel_cols - 42;  // right column width
    if (rw < 20) rw = 20;
    {
        char vbuf[48];
        // Profile
        draw_dotted_leader(row, rc, rw, "Profile", s_snap.profileName,
                           (uint8_t)TERM_FG_DEFAULT, (uint8_t)TERM_FG_DEFAULT, 0);

        // VLOGIC
        {
            uint8_t fg = s_snap.logicPg ? (uint8_t)TERM_FG_B_GREEN : (uint8_t)TERM_FG_B_YELLOW;
            snprintf(vbuf, sizeof(vbuf), "%.3f V  PG:%s",
                     s_snap.vlogic, s_snap.logicPg ? "OK" : "NO");
            draw_dotted_leader(row+1, rc, rw, "VLOGIC", vbuf,
                               (uint8_t)TERM_FG_DEFAULT, fg, 0);
        }
        // VADJ1
        {
            uint8_t fg = s_snap.vadj1Pg ? (uint8_t)TERM_FG_B_GREEN : (uint8_t)TERM_FG_B_YELLOW;
            snprintf(vbuf, sizeof(vbuf), "%.3f V  PG:%s",
                     s_snap.vadj1, s_snap.vadj1Pg ? "OK" : "NO");
            draw_dotted_leader(row+2, rc, rw, "VADJ1", vbuf,
                               (uint8_t)TERM_FG_DEFAULT, fg, 0);
        }
        // VADJ2
        {
            uint8_t fg = s_snap.vadj2Pg ? (uint8_t)TERM_FG_B_GREEN : (uint8_t)TERM_FG_B_YELLOW;
            snprintf(vbuf, sizeof(vbuf), "%.3f V  PG:%s",
                     s_snap.vadj2, s_snap.vadj2Pg ? "OK" : "NO");
            draw_dotted_leader(row+3, rc, rw, "VADJ2", vbuf,
                               (uint8_t)TERM_FG_DEFAULT, fg, 0);
        }
        // USB-PD
        {
            uint8_t fg = s_snap.pdAttached ? (uint8_t)TERM_FG_B_GREEN : (uint8_t)TERM_FG_B_BLACK;
            snprintf(vbuf, sizeof(vbuf), "%s  %.1fV/%.2fA",
                     s_snap.pdAttached ? "attached" : "detached",
                     s_snap.pdVoltage, s_snap.pdCurrent);
            draw_dotted_leader(row+4, rc, rw, "USB-PD", vbuf,
                               (uint8_t)TERM_FG_DEFAULT, fg, 0);
        }
    }

    row += 5;

    // E-fuse status row
    draw_text(row, lc, "E-fuses", (uint8_t)TERM_FG_DEFAULT, 0);
    {
        int col = lc + 9;
        for (int i = 0; i < 4; i++) {
            uint8_t fg;
            if (s_snap.efuseFlt[i]) {
                fg = (uint8_t)TERM_FG_B_RED;
            } else if (s_snap.efuseEn[i]) {
                fg = (uint8_t)TERM_FG_B_GREEN;
            } else {
                fg = (uint8_t)TERM_FG_B_YELLOW;
            }
            char lbl[5];
            snprintf(lbl, sizeof(lbl), "[F%d]", i + 1);
            draw_text(row, col, lbl, fg, 0);
            col += 5;
        }
    }

    // HAT status row
    row++;
    {
        uint8_t fg;
        if (s_snap.hatDetected && s_snap.hatConnected) {
            fg = (uint8_t)TERM_FG_B_GREEN;
        } else if (s_snap.hatDetected) {
            fg = (uint8_t)TERM_FG_B_YELLOW;
        } else {
            fg = (uint8_t)TERM_FG_B_BLACK;
        }
        char vbuf[48];
        snprintf(vbuf, sizeof(vbuf), "%-10s  %s  %s",
                 hat_type_name((HatType)s_snap.hatType),
                 s_snap.hatDetected  ? "detected"  : "missing",
                 s_snap.hatConnected ? "connected" : "disconnected");
        draw_dotted_leader(row, lc, 55, "HAT", vbuf,
                           (uint8_t)TERM_FG_DEFAULT, fg, 0);
    }

    // WiFi row
    row++;
    {
        bool connected = (s_snap.rssi != -128);
        if (connected) {
            uint8_t fg;
            if (s_snap.rssi > -60) {
                fg = (uint8_t)TERM_FG_B_GREEN;
            } else if (s_snap.rssi >= -75) {
                fg = (uint8_t)TERM_FG_B_YELLOW;
            } else {
                fg = (uint8_t)TERM_FG_B_RED;
            }
            char vbuf[64];
            snprintf(vbuf, sizeof(vbuf), "%-20s  %-15s  %d dBm",
                     s_snap.ssid, s_snap.ipStr, (int)s_snap.rssi);
            draw_dotted_leader(row, lc, 60, "WiFi", vbuf,
                               (uint8_t)TERM_FG_DEFAULT, fg, 0);
        } else {
            draw_text(row, lc, "WiFi", (uint8_t)TERM_FG_DEFAULT, 0);
            draw_text(row, lc + 5, "(disconnected)", (uint8_t)TERM_FG_B_BLACK, ATTR_DIM);
        }
    }

    row += 2;

    // Channel summary table
    draw_textf(row, lc, TERM_FG_B_WHITE, ATTR_BOLD,
               " CH | Function       |   ADC value   |  DAC code  |  DO ");
    draw_hline(row + 1, lc, 60, '-', TERM_FG_B_BLACK, 0);
    for (int c = 0; c < 4; c++) {
        const ChanSnap& s = s_snap.ch[c];
        draw_textf(row + 2 + c, lc, TERM_FG_DEFAULT, 0,
                   " %d  | %-13s | %+10.4f V | %5u      |  %s",
                   c, func_label(s.function), s.adcValue, s.dacCode,
                   s.doState ? "ON " : "OFF");
    }

    row += 7;  // past channel table

    // System panel (heap + tasks) — draw below channel table.
    {
        int sp_col  = lc;
        int sp_w    = 42;   // inner width (panel cols = sp_w + 2 for borders)
        int sp_rows = 5;    // 3 data rows + 2 border rows
        if (row + sp_rows >= s_rows - 4) {
            // Not enough rows — skip panel entirely
            return;
        }
        draw_panel(row, sp_col - 1, sp_rows, sp_w + 2, "System", (uint8_t)TERM_FG_B_BLACK, 0);

        int sr = row + 1;  // first row inside panel
        int sc = sp_col + 1;

        // Task count
        {
            char vbuf[16];
            snprintf(vbuf, sizeof(vbuf), "%u", (unsigned)uxTaskGetNumberOfTasks());
            draw_dotted_leader(sr + 0, sc, 28, "Tasks", vbuf,
                               (uint8_t)TERM_FG_DEFAULT, (uint8_t)TERM_FG_DEFAULT, 0);
        }

        // Heap free
        {
            char vbuf[20];
            uint32_t heap_kb = esp_get_free_heap_size() / 1024;
            uint8_t heap_fg = (heap_kb < 20) ? (uint8_t)TERM_FG_B_RED
                            : (heap_kb < 50) ? (uint8_t)TERM_FG_B_YELLOW
                            :                  (uint8_t)TERM_FG_B_GREEN;
            snprintf(vbuf, sizeof(vbuf), "%u kB free", (unsigned)heap_kb);
            draw_dotted_leader(sr + 1, sc, 28, "Heap", vbuf,
                               (uint8_t)TERM_FG_DEFAULT, heap_fg, 0);
        }

        // Heap minimum
        {
            char vbuf[20];
            uint32_t min_kb = esp_get_minimum_free_heap_size() / 1024;
            snprintf(vbuf, sizeof(vbuf), "%u kB", (unsigned)min_kb);
            draw_dotted_leader(sr + 2, sc, 28, "Heap min", vbuf,
                               (uint8_t)TERM_FG_DEFAULT, (uint8_t)TERM_FG_B_BLACK, 0);
        }
    }
}

// --- Channel tab ------------------------------------------------------------

static void render_channel_tab(int ch) {
    const ChanSnap& s = s_snap.ch[ch];

    // Panel (Fix 3)
    int panel_row  = 4;
    int panel_col  = 1;
    int panel_rows = s_rows - 4 - panel_row;
    int panel_cols = s_cols - 2;
    if (panel_rows < 4) panel_rows = 4;
    draw_panel(panel_row, panel_col, panel_rows, panel_cols,
               kTabNames[TAB_CH0 + ch], (uint8_t)TERM_FG_B_BLACK, 0);

    int row = panel_row + 1;
    int lc  = panel_col + 2;

    // Live readout block (Fix 4: dotted leaders)
    {
        char vbuf[48];
        snprintf(vbuf, sizeof(vbuf), "%+10.4f V   (raw 0x%06lX)",
                 s.adcValue, (unsigned long)s.adcRawCode);
        draw_dotted_leader(row, lc, 50, "Live ADC", vbuf,
                           (uint8_t)TERM_FG_DEFAULT, (uint8_t)TERM_FG_DEFAULT, 0);

        snprintf(vbuf, sizeof(vbuf), "code %5u  ~ %.3f V", s.dacCode, s.dacValue);
        draw_dotted_leader(row+1, lc, 50, "DAC active", vbuf,
                           (uint8_t)TERM_FG_DEFAULT, (uint8_t)TERM_FG_DEFAULT, 0);

        uint8_t do_fg = s.doState ? (uint8_t)TERM_FG_B_GREEN : (uint8_t)TERM_FG_DEFAULT;
        draw_dotted_leader(row+2, lc, 50, "DO state", s.doState ? "ON" : "OFF",
                           (uint8_t)TERM_FG_DEFAULT, do_fg, 0);

        uint8_t al_fg = s.channelAlertStatus ? (uint8_t)TERM_FG_B_YELLOW : (uint8_t)TERM_FG_DEFAULT;
        snprintf(vbuf, sizeof(vbuf), "0x%04X", s.channelAlertStatus);
        draw_dotted_leader(row+3, lc, 50, "Channel alert", vbuf,
                           (uint8_t)TERM_FG_DEFAULT, al_fg, 0);
    }
    row += 5;

    // Field rows (selectable)
    struct FieldDef {
        const char* label;
        char        buf[64];
    } fields[NUM_CH_FIELDS];

    snprintf(fields[CH_FIELD_FUNCTION].buf,
             sizeof(fields[0].buf),
             "%s", func_label(s.function));
    fields[CH_FIELD_FUNCTION].label = "Function";

    // Find ADC rate label
    {
        const char* rate_lbl = "?";
        for (uint8_t i = 0; i < kAdcRateItemCount; i++) {
            if (kAdcRateItems[i].value == (int32_t)s.adcRate) {
                rate_lbl = kAdcRateItems[i].label;
                break;
            }
        }
        snprintf(fields[CH_FIELD_ADC_RATE].buf, sizeof(fields[0].buf), "%s", rate_lbl);
    }
    fields[CH_FIELD_ADC_RATE].label = "ADC Rate";

    // Find ADC mux label
    {
        const char* mux_lbl = "?";
        for (uint8_t i = 0; i < kAdcMuxItemCount; i++) {
            if (kAdcMuxItems[i].value == (int32_t)s.adcMux) {
                mux_lbl = kAdcMuxItems[i].label;
                break;
            }
        }
        snprintf(fields[CH_FIELD_ADC_MUX].buf, sizeof(fields[0].buf), "%s", mux_lbl);
    }
    fields[CH_FIELD_ADC_MUX].label = "ADC Mux";

    snprintf(fields[CH_FIELD_DAC_VOLTAGE].buf,
             sizeof(fields[0].buf),
             "%.3f V  (target — opens slider)", s.dacValue);
    fields[CH_FIELD_DAC_VOLTAGE].label = "DAC Voltage";

    snprintf(fields[CH_FIELD_DO_TOGGLE].buf,
             sizeof(fields[0].buf),
             "%s  (Enter to toggle)", s.doState ? "ON" : "OFF");
    fields[CH_FIELD_DO_TOGGLE].label = "DO State";

    snprintf(fields[CH_FIELD_CLEAR_ALERT].buf,
             sizeof(fields[0].buf),
             "Clear channel alert  (0x%04X)", s.channelAlertStatus);
    fields[CH_FIELD_CLEAR_ALERT].label = "Faults";

    for (int i = 0; i < NUM_CH_FIELDS; i++) {
        bool selected = (s_field == i);
        uint8_t fg   = selected ? TERM_FG_B_YELLOW : TERM_FG_DEFAULT;
        uint8_t attr = selected ? ATTR_BOLD : 0;
        const char* arrow = selected ? "> " : "  ";
        draw_textf(row + i, lc, fg, attr,
                   "%s%-15s  %s", arrow,
                   fields[i].label, fields[i].buf);
    }

    row += NUM_CH_FIELDS + 1;
    draw_textf(row, lc, TERM_FG_B_BLACK, 0,
               "(Function picker replaces the old destructive enum cycle.)");
}

// --- Settings tab -----------------------------------------------------------

static void render_settings_tab(void) {
    // Panel (Fix 3)
    int panel_row  = 4;
    int panel_col  = 1;
    int panel_rows = s_rows - 4 - panel_row;
    int panel_cols = s_cols - 2;
    if (panel_rows < 4) panel_rows = 4;
    draw_panel(panel_row, panel_col, panel_rows, panel_cols,
               kTabNames[TAB_SETTINGS], (uint8_t)TERM_FG_B_BLACK, 0);

    int row = panel_row + 1;
    int lc  = panel_col + 2;

    struct {
        const char* label;
        char        value[64];
    } items[NUM_SET_FIELDS];

    snprintf(items[SET_FIELD_CLEAR_FAULTS].value,
             sizeof(items[0].value),
             "Clear all alert registers   (current alert 0x%04X)",
             s_snap.alertStatus);
    // Calibration rows
    {
        const SelftestCalResult* cr = selftest_get_cal_result();
        bool busy = selftest_is_busy();
        snprintf(items[SET_FIELD_CAL_VADJ1].value, sizeof(items[0].value),
                 "%s", busy ? "BUSY" : "Start auto-calibration");
        items[SET_FIELD_CAL_VADJ1].label = "Cal VADJ1";
        snprintf(items[SET_FIELD_CAL_VADJ2].value, sizeof(items[0].value),
                 "%s", busy ? "BUSY" : "Start auto-calibration");
        items[SET_FIELD_CAL_VADJ2].label = "Cal VADJ2";
        const char* cal_err = (cr && cr->status == CAL_STATUS_SUCCESS)
                              ? "last: OK" : (cr ? "last: --" : "--");
        (void)cal_err;
        snprintf(items[SET_FIELD_CAL_SAVE].value, sizeof(items[0].value),
                 "Save all cal data to NVS");
        items[SET_FIELD_CAL_SAVE].label = "Cal Save";
        snprintf(items[SET_FIELD_CAL_LOAD].value, sizeof(items[0].value),
                 "Load all cal data from NVS");
        items[SET_FIELD_CAL_LOAD].label = "Cal Load";
        snprintf(items[SET_FIELD_CAL_CLEAR].value, sizeof(items[0].value),
                 "Clear cal data for ch 0-2");
        items[SET_FIELD_CAL_CLEAR].label = "Cal Clear";
    }
    items[SET_FIELD_CLEAR_FAULTS].label = "Faults";

    snprintf(items[SET_FIELD_USBPD].value,
             sizeof(items[0].value),
             "%.1f V / %.2f A   %s",
             s_snap.pdVoltage, s_snap.pdCurrent,
             s_snap.pdAttached ? "(attached)" : "(detached)");
    items[SET_FIELD_USBPD].label = "USB-PD";

    snprintf(items[SET_FIELD_BOARD_PROFILE].value,
             sizeof(items[0].value),
             "%s   (id %s)", s_snap.profileName, s_snap.profileId);
    items[SET_FIELD_BOARD_PROFILE].label = "Board Profile";

    for (int slot = 0; slot < 4; slot++) {
        int fi = SET_FIELD_DIAG_0 + slot;
        const DiagSnap& d = s_snap.diag[slot];
        snprintf(items[fi].value, sizeof(items[0].value),
                 "%-12s  = %.4f", diag_source_label(d.source), d.value);
        static const char* kDiagLabels[4] = {
            "Diag slot 0", "Diag slot 1", "Diag slot 2", "Diag slot 3"
        };
        items[fi].label = kDiagLabels[slot];
    }

    for (int i = 0; i < NUM_SET_FIELDS; i++) {
        bool selected = (s_field == i);
        uint8_t fg   = selected ? TERM_FG_B_YELLOW : TERM_FG_DEFAULT;
        uint8_t attr = selected ? ATTR_BOLD : 0;
        const char* arrow = selected ? "> " : "  ";
        draw_textf(row + i, lc, fg, attr,
                   "%s%-16s  %s", arrow, items[i].label, items[i].value);
    }
}

// --- Power tab --------------------------------------------------------------

static void render_power_tab(void) {
    // Panel (Fix 3)
    int panel_row  = 4;
    int panel_col  = 1;
    int panel_rows = s_rows - 4 - panel_row;
    int panel_cols = s_cols - 2;
    if (panel_rows < 4) panel_rows = 4;
    draw_panel(panel_row, panel_col, panel_rows, panel_cols,
               kTabNames[TAB_POWER], (uint8_t)TERM_FG_B_BLACK, 0);

    int row = panel_row + 1;
    int lc  = panel_col + 2;

    struct PwrRow {
        const char* label;
        char        value[64];
    } items[NUM_PWR_FIELDS];

    snprintf(items[PWR_FIELD_VADJ1].value, sizeof(items[0].value),
             "%.3f V  PG:%s", s_snap.vadj1, s_snap.vadj1Pg ? "OK" : "NO");
    items[PWR_FIELD_VADJ1].label = "VADJ1";

    snprintf(items[PWR_FIELD_VADJ2].value, sizeof(items[0].value),
             "%.3f V  PG:%s", s_snap.vadj2, s_snap.vadj2Pg ? "OK" : "NO");
    items[PWR_FIELD_VADJ2].label = "VADJ2";

    snprintf(items[PWR_FIELD_VLOGIC].value, sizeof(items[0].value),
             "%.3f V  PG:%s", s_snap.vlogic, s_snap.logicPg ? "OK" : "NO");
    items[PWR_FIELD_VLOGIC].label = "Level-Shifter";

    for (int i = 0; i < 4; i++) {
        int fi = PWR_FIELD_EFUSE1 + i;
        snprintf(items[fi].value, sizeof(items[0].value),
                 "%s  flt:%s",
                 s_snap.efuseEn[i]  ? "ON " : "OFF",
                 s_snap.efuseFlt[i] ? "YES" : "no");
        static const char* kEfLabels[4] = {
            "E-Fuse 1", "E-Fuse 2", "E-Fuse 3", "E-Fuse 4"
        };
        items[fi].label = kEfLabels[i];
    }

    snprintf(items[PWR_FIELD_15V].value, sizeof(items[0].value),
             "%s", s_snap.rail15v ? "ON" : "OFF");
    items[PWR_FIELD_15V].label = "15V Rail";

    snprintf(items[PWR_FIELD_MUX].value, sizeof(items[0].value),
             "%s", s_snap.muxEn ? "ON" : "OFF");
    items[PWR_FIELD_MUX].label = "MUX Power";

    snprintf(items[PWR_FIELD_USBHUB].value, sizeof(items[0].value),
             "%s", s_snap.usbHubEn ? "ON" : "OFF");
    items[PWR_FIELD_USBHUB].label = "USB Hub";

    for (int i = 0; i < NUM_PWR_FIELDS; i++) {
        bool selected = (s_field == i);
        uint8_t fg   = selected ? TERM_FG_B_YELLOW : TERM_FG_DEFAULT;
        uint8_t attr = selected ? ATTR_BOLD : 0;
        const char* arrow = selected ? "> " : "  ";
        draw_textf(row + i, lc, fg, attr,
                   "%s%-18s  %s", arrow, items[i].label, items[i].value);
    }
}

// --- Signal Path tab --------------------------------------------------------

static void render_signal_tab(void) {
    // Panel (Fix 3)
    int panel_row  = 4;
    int panel_col  = 1;
    int panel_rows = s_rows - 4 - panel_row;
    int panel_cols = s_cols - 2;
    if (panel_rows < 4) panel_rows = 4;
    draw_panel(panel_row, panel_col, panel_rows, panel_cols,
               kTabNames[TAB_SIGNAL], (uint8_t)TERM_FG_B_BLACK, 0);

    int row = panel_row + 1;
    int lc  = panel_col + 2;

    // Status row (uses snapshot values — no SPI read on hot path)
    draw_textf(row, lc, s_snap.adgsFaulted ? TERM_FG_B_RED : TERM_FG_DEFAULT, 0,
               "error_flags=0x%02X  faulted=%s",
               s_snap.adgsErrorFlags, s_snap.adgsFaulted ? "YES" : "NO");
    row++;

    // Reset-all row (SIGNAL_RESET_FIELD = 0, first selectable row)
    {
        bool sel     = (s_field == SIGNAL_RESET_FIELD);
        uint8_t fg   = sel ? TERM_FG_B_YELLOW : TERM_FG_DEFAULT;
        uint8_t attr = sel ? ATTR_BOLD : 0;
        draw_textf(row, lc, fg, attr,
                   "%sReset all (open)", sel ? "> " : "  ");
        row++;
    }

    // Column header
    draw_textf(row, lc, TERM_FG_B_BLACK, 0,
               "Dev  SW0 SW1 SW2 SW3 SW4 SW5 SW6 SW7");
    row++;

    // 4x8 grid (fields SIGNAL_GRID_BASE .. SIGNAL_GRID_BASE+31)
    for (int dev = 0; dev < ADGS_API_MAIN_DEVICES; dev++) {
        draw_textf(row + dev, lc, TERM_FG_DEFAULT, 0, " %d  ", dev);
        for (int sw = 0; sw < 8; sw++) {
            bool closed = (s_snap.muxState[dev] >> sw) & 1;
            bool sel    = (s_field == SIGNAL_GRID_BASE + dev * 8 + sw);
            uint8_t fg   = sel ? TERM_FG_B_YELLOW : (closed ? TERM_FG_B_GREEN : TERM_FG_DEFAULT);
            uint8_t attr = sel ? ATTR_BOLD : 0;
            draw_textf(row + dev, lc + 5 + sw * 4, fg, attr, "%s", closed ? "[X]" : "[ ]");
        }
    }
}

// --- HAT tab ----------------------------------------------------------------

static void render_hat_tab(void) {
    // Panel (Fix 3)
    int panel_row  = 4;
    int panel_col  = 1;
    int panel_rows = s_rows - 4 - panel_row;
    int panel_cols = s_cols - 2;
    if (panel_rows < 4) panel_rows = 4;
    draw_panel(panel_row, panel_col, panel_rows, panel_cols,
               kTabNames[TAB_HAT], (uint8_t)TERM_FG_B_BLACK, 0);

    int row = panel_row + 1;
    int lc  = panel_col + 2;

    // Read-only status block (not selectable)
    const HatState* hs = hat_get_state();
    bool det = hat_detected();
    draw_textf(row,   lc, TERM_FG_DEFAULT, 0,
               "Detected: %s   Type: %s   Connected: %s",
               det ? "YES" : "NO",
               hs ? hat_type_name((HatType)hs->type) : "?",
               (hs && hs->connected) ? "YES" : "NO");
    if (hs && hs->connected) {
        draw_textf(row + 1, lc, TERM_FG_DEFAULT, 0,
                   "FW: %u.%u   DAP: %s   Target: %s",
                   hs->fw_version_major, hs->fw_version_minor,
                   hs->dap_connected   ? "YES" : "NO",
                   hs->target_detected ? "YES" : "NO");
    }
    row += 3;

    // Selectable rows
    struct HatRow {
        const char* label;
        char        value[64];
    } items[NUM_HAT_FIELDS];

    for (int i = 0; i < 4; i++) {
        int fi = HAT_FIELD_PIN0 + i;
        HatPinFunction func = (hs && i < HAT_NUM_EXT_PINS)
                              ? hs->pin_config[i] : HAT_FUNC_DISCONNECTED;
        snprintf(items[fi].value, sizeof(items[0].value), "%s", hat_func_name(func));
        static const char* kPinLabels[4] = {
            "EXP_EXT1", "EXP_EXT2", "EXP_EXT3", "EXP_EXT4"
        };
        items[fi].label = kPinLabels[i];
    }

    {
        bool on_a = hs && hs->connector[HAT_CONNECTOR_A].enabled;
        snprintf(items[HAT_FIELD_CONN_A].value, sizeof(items[0].value),
                 "%s  %.1f mA  flt:%s",
                 on_a ? "ON " : "OFF",
                 hs ? hs->connector[HAT_CONNECTOR_A].current_ma : 0.0f,
                 (hs && hs->connector[HAT_CONNECTOR_A].fault) ? "YES" : "no");
        items[HAT_FIELD_CONN_A].label = "Connector A";
    }
    {
        bool on_b = hs && hs->connector[HAT_CONNECTOR_B].enabled;
        snprintf(items[HAT_FIELD_CONN_B].value, sizeof(items[0].value),
                 "%s  %.1f mA  flt:%s",
                 on_b ? "ON " : "OFF",
                 hs ? hs->connector[HAT_CONNECTOR_B].current_ma : 0.0f,
                 (hs && hs->connector[HAT_CONNECTOR_B].fault) ? "YES" : "no");
        items[HAT_FIELD_CONN_B].label = "Connector B";
    }

    snprintf(items[HAT_FIELD_IO_VOLT].value, sizeof(items[0].value),
             "%u mV", hs ? hs->io_voltage_mv : 0);
    items[HAT_FIELD_IO_VOLT].label = "I/O Voltage";

    snprintf(items[HAT_FIELD_DETECT].value, sizeof(items[0].value),
             "Run detect now");
    items[HAT_FIELD_DETECT].label = "Detect HAT";

    snprintf(items[HAT_FIELD_RESET].value, sizeof(items[0].value),
             "Reset all pins to disconnected");
    items[HAT_FIELD_RESET].label = "Reset HAT";

    for (int i = 0; i < NUM_HAT_FIELDS; i++) {
        bool selected = (s_field == i);
        uint8_t fg   = selected ? TERM_FG_B_YELLOW : TERM_FG_DEFAULT;
        uint8_t attr = selected ? ATTR_BOLD : 0;
        const char* arrow = selected ? "> " : "  ";
        draw_textf(row + i, lc, fg, attr,
                   "%s%-14s  %s", arrow, items[i].label, items[i].value);
    }
}

// ===========================================================================
// Modal rendering
// ===========================================================================

static void render_picker(ModalFrame* f) {
    int box_w = 0;
    // size box to content
    for (uint8_t i = 0; i < f->item_count; i++) {
        int len = (int)strlen(f->items[i].label);
        if (len > box_w) box_w = len;
    }
    box_w += 8;
    if (box_w < 30) box_w = 30;
    if (box_w > s_cols - 4) box_w = s_cols - 4;

    int max_visible = s_rows - 10;
    if (max_visible < 4) max_visible = 4;
    int visible = (f->item_count < max_visible) ? f->item_count : max_visible;
    int box_h = visible + 5;
    if (box_h > s_rows - 4) box_h = s_rows - 4;

    int row = (s_rows - box_h) / 2;
    int col = (s_cols - box_w) / 2;

    // Backdrop fill
    draw_fill(row, col, box_h, box_w, ' ', 0, 0);
    draw_box (row, col, box_h, box_w, TERM_FG_B_CYAN, ATTR_BOLD);

    // Title bar
    draw_textf(row, col + 2, TERM_FG_B_WHITE, ATTR_BOLD,
               " %s ", f->title ? f->title : "Picker");

    // Compute scroll window so selected stays in view
    int scroll = f->scroll;
    if (f->selected < scroll) scroll = f->selected;
    if (f->selected >= scroll + visible) scroll = f->selected - visible + 1;
    if (scroll < 0) scroll = 0;
    f->scroll = (uint8_t)scroll;

    for (int i = 0; i < visible; i++) {
        int idx = scroll + i;
        if (idx >= f->item_count) break;
        bool sel = (idx == f->selected);
        uint8_t fg   = sel ? TERM_FG_B_YELLOW : TERM_FG_DEFAULT;
        uint8_t attr = sel ? (ATTR_BOLD | ATTR_REVERSE) : 0;
        draw_fill(row + 2 + i, col + 2, 1, box_w - 4, ' ', fg, attr);
        draw_textf(row + 2 + i, col + 3, fg, attr,
                   "%s %s", sel ? ">" : " ", f->items[idx].label);
    }

    // Footer hint
    draw_textf(row + box_h - 2, col + 2, TERM_FG_B_BLACK, 0,
               " Up/Down  select   Enter  apply   ESC  cancel ");
}

static void render_slider(ModalFrame* f) {
    int box_w = s_cols - 10;
    if (box_w > 70) box_w = 70;
    if (box_w < 40) box_w = 40;
    int box_h = 9;

    int row = (s_rows - box_h) / 2;
    int col = (s_cols - box_w) / 2;

    draw_fill(row, col, box_h, box_w, ' ', 0, 0);
    draw_box (row, col, box_h, box_w, TERM_FG_B_CYAN, ATTR_BOLD);
    draw_textf(row, col + 2, TERM_FG_B_WHITE, ATTR_BOLD,
               " %s ", f->title ? f->title : "Slider");

    // Numeric readout
    draw_textf(row + 2, col + 3, TERM_FG_DEFAULT, 0,
               "Value: %.3f %s   (min %.3f, max %.3f, step %.3f)",
               f->value, f->unit, f->min, f->max, f->step);

    // Bar
    int bar_w = box_w - 6;
    if (bar_w < 10) bar_w = 10;
    float frac = (f->max > f->min) ? (f->value - f->min) / (f->max - f->min) : 0.0f;
    if (frac < 0.0f) frac = 0.0f;
    if (frac > 1.0f) frac = 1.0f;
    int filled = (int)(frac * bar_w + 0.5f);
    draw_text(row + 4, col + 3, "[", TERM_FG_B_BLACK, 0);
    for (int i = 0; i < bar_w; i++) {
        char ch = (i < filled) ? '#' : '.';
        uint8_t fg = (i < filled) ? TERM_FG_B_GREEN : TERM_FG_B_BLACK;
        put_cell(row + 4, col + 4 + i, ch, fg, 0);
    }
    draw_text(row + 4, col + 4 + bar_w, "]", TERM_FG_B_BLACK, 0);

    // Hint / apply prompt
    if (f->apply_pending) {
        draw_textf(row + 6, col + 3, TERM_FG_B_YELLOW, ATTR_BOLD,
                   "Apply %.3f %s to hardware? (Enter=YES, ESC=cancel)",
                   f->value, f->unit);
    } else {
        draw_textf(row + 6, col + 3, TERM_FG_B_BLACK, 0,
                   "Left/Right: +/- step   PgUp/PgDn: x100   Home/End: extremes");
        draw_textf(row + 7, col + 3, TERM_FG_B_BLACK, 0,
                   "Enter: review apply       ESC: cancel");
    }
}

static void render_confirm(ModalFrame* f) {
    int box_w = (int)strlen(f->prompt ? f->prompt : "") + 10;
    if (box_w < 40) box_w = 40;
    if (box_w > s_cols - 10) box_w = s_cols - 10;
    int box_h = 7;
    int row = (s_rows - box_h) / 2;
    int col = (s_cols - box_w) / 2;

    draw_fill(row, col, box_h, box_w, ' ', 0, 0);
    draw_box (row, col, box_h, box_w, TERM_FG_B_RED, ATTR_BOLD);
    draw_textf(row, col + 2, TERM_FG_B_WHITE, ATTR_BOLD,
               " %s ", f->title ? f->title : "Confirm");

    draw_textf(row + 2, col + 3, TERM_FG_DEFAULT, 0,
               "%s", f->prompt ? f->prompt : "");

    // YES / NO buttons
    const char* yes = "[ YES ]";
    const char* no  = "[ NO  ]";
    int y_col = col + 4;
    int n_col = col + box_w - 4 - (int)strlen(no);
    {
        uint8_t fg   = f->confirm_yes ? TERM_FG_B_RED  : TERM_FG_B_BLACK;
        uint8_t attr = f->confirm_yes ? (ATTR_BOLD|ATTR_REVERSE) : 0;
        draw_text(row + 4, y_col, yes, fg, attr);
    }
    {
        uint8_t fg   = !f->confirm_yes ? TERM_FG_B_GREEN : TERM_FG_B_BLACK;
        uint8_t attr = !f->confirm_yes ? (ATTR_BOLD|ATTR_REVERSE) : 0;
        draw_text(row + 4, n_col, no, fg, attr);
    }

    draw_textf(row + box_h - 2, col + 3, TERM_FG_B_BLACK, 0,
               " Left/Right: select   Enter: confirm   y/n: shortcut   ESC: cancel ");
}

static void render_progress(ModalFrame* f) {
    // Tick the callback to update status before rendering
    if (f->progress_tick && !f->progress_done) {
        f->progress_tick(f, f->user);
    }

    int box_w = s_cols - 10;
    if (box_w > 70) box_w = 70;
    if (box_w < 40) box_w = 40;
    int box_h = 9;
    int row = (s_rows - box_h) / 2;
    int col = (s_cols - box_w) / 2;

    draw_fill(row, col, box_h, box_w, ' ', 0, 0);
    uint8_t border_fg = f->progress_failed ? (uint8_t)TERM_FG_B_RED : (uint8_t)TERM_FG_B_CYAN;
    draw_box(row, col, box_h, box_w, border_fg, ATTR_BOLD);
    draw_textf(row, col + 2, TERM_FG_B_WHITE, ATTR_BOLD,
               " %s ", f->title ? f->title : "Progress");

    // Status string
    draw_textf(row + 2, col + 3, TERM_FG_DEFAULT, 0,
               "%s", f->progress_status ? f->progress_status : "");

    // Progress bar
    int bar_w = box_w - 6;
    if (bar_w < 10) bar_w = 10;
    draw_text(row + 4, col + 3, "[", TERM_FG_B_BLACK, 0);
    if (f->progress_percent >= 0) {
        float frac = f->progress_percent / 100.0f;
        if (frac < 0.0f) frac = 0.0f;
        if (frac > 1.0f) frac = 1.0f;
        int filled = (int)(frac * bar_w + 0.5f);
        for (int i = 0; i < bar_w; i++) {
            char ch = (i < filled) ? '#' : '.';
            uint8_t fg = (i < filled)
                         ? (f->progress_failed ? (uint8_t)TERM_FG_B_RED : (uint8_t)TERM_FG_B_GREEN)
                         : (uint8_t)TERM_FG_B_BLACK;
            put_cell(row + 4, col + 4 + i, ch, fg, 0);
        }
    } else {
        // Indeterminate: spinning fill
        static uint8_t s_spin = 0;
        s_spin = (s_spin + 1) % (uint8_t)bar_w;
        for (int i = 0; i < bar_w; i++) {
            char ch = (i == (int)s_spin) ? '#' : '.';
            put_cell(row + 4, col + 4 + i, ch, TERM_FG_B_CYAN, 0);
        }
    }
    draw_text(row + 4, col + 4 + bar_w, "]", TERM_FG_B_BLACK, 0);

    // Footer
    if (f->progress_done) {
        draw_textf(row + 6, col + 3,
                   f->progress_failed ? (uint8_t)TERM_FG_B_RED : (uint8_t)TERM_FG_B_GREEN,
                   ATTR_BOLD,
                   "%s  Press any key to dismiss",
                   f->progress_failed ? "FAILED." : "Done.");
    } else {
        draw_textf(row + 6, col + 3, TERM_FG_B_BLACK, 0,
                   "ESC: request cancel");
    }
}

static void render_help(void) {
    static const char* const kHelpLines[] = {
        "Tabs          : Left / Right arrow",
        "Field         : Up / Down arrow (Signal tab: 2D arrows)",
        "Open / act.   : Enter or Space",
        "Confirm modal : y = YES   n = NO",
        "Slider adjust : Left/Right +/-step   PgUp/PgDn x100   Home/End extremes",
        "Slider apply  : Enter (review)  then Enter again (write)",
        "Cancel modal  : ESC",
        "Quit menu     : q   or   ESC ESC",
        "Force redraw  : r",
    };
    static const int kLineCount = (int)(sizeof(kHelpLines) / sizeof(kHelpLines[0]));

    int box_w = 58;
    if (box_w > s_cols - 4) box_w = s_cols - 4;
    int box_h = kLineCount + 5;
    if (box_h > s_rows - 2) box_h = s_rows - 2;

    int row = (s_rows - box_h) / 2;
    int col = (s_cols - box_w) / 2;

    draw_fill(row, col, box_h, box_w, ' ', 0, 0);
    draw_box (row, col, box_h, box_w, TERM_FG_B_CYAN, ATTR_BOLD);
    draw_textf(row, col + 2, TERM_FG_B_WHITE, ATTR_BOLD, " Help ");

    int visible = box_h - 4;
    if (visible > kLineCount) visible = kLineCount;
    for (int i = 0; i < visible; i++) {
        draw_text(row + 2 + i, col + 3, kHelpLines[i], TERM_FG_DEFAULT, 0);
    }
    draw_textf(row + box_h - 2, col + 3, TERM_FG_B_BLACK, 0,
               " Press any key to dismiss ");
}

static void open_help(void) {
    ModalFrame* f = modal_push(M_HELP);
    if (!f) return;
    f->title = "Help";
}

static void render_modal(void) {
    ModalFrame* f = modal_top();
    if (!f) return;
    switch (f->kind) {
        case M_PICKER:   render_picker(f);   break;
        case M_SLIDER:   render_slider(f);   break;
        case M_CONFIRM:  render_confirm(f);  break;
        case M_PROGRESS: render_progress(f); break;
        case M_HELP:     render_help();      break;
        default: break;
    }
}

// ===========================================================================
// Modal input
// ===========================================================================

static void modal_picker_feed(ModalFrame* f, uint8_t b, char csi_final) {
    if (csi_final == 'A') {            // up
        if (f->selected > 0) f->selected--;
    } else if (csi_final == 'B') {     // down
        if (f->selected + 1 < f->item_count) f->selected++;
    } else if (b == '\r' || b == '\n') {
        if (f->picker_cb) {
            f->picker_cb(f->items[f->selected].value, f->user);
        }
        modal_pop();
    }
}

static void modal_slider_feed(ModalFrame* f, uint8_t b, char csi_final) {
    if (f->apply_pending) {
        // In apply-confirmation sub-state: Enter applies, ESC cancels confirm.
        if (b == '\r' || b == '\n') {
            if (f->slider_cb) f->slider_cb(f->value, f->user);
            modal_pop();
        }
        // ESC cancellation of apply_pending is handled by on_escape() -> modal_pop().
        return;
    }
    float v = f->value;
    if      (csi_final == 'C') v += f->step;
    else if (csi_final == 'D') v -= f->step;
    else if (csi_final == 'H') v  = f->min;
    else if (csi_final == 'F') v  = f->max;
    else if (csi_final == '~') {
        // PgUp / PgDn (delivered as ESC[5~ / ESC[6~). The CSI-final dispatch
        // packs the param into the modal_feed path via raw byte; handled below.
    } else if (b == '\r' || b == '\n') {
        f->apply_pending = true;
        return;
    }
    if (v < f->min) v = f->min;
    if (v > f->max) v = f->max;
    f->value = v;
}

static void modal_confirm_feed(ModalFrame* f, uint8_t b, char csi_final) {
    if (csi_final == 'C' || csi_final == 'D') {
        f->confirm_yes = !f->confirm_yes;
    } else if (b == 'y' || b == 'Y') {
        if (f->confirm_cb) f->confirm_cb(true, f->user);
        modal_pop();
    } else if (b == 'n' || b == 'N') {
        if (f->confirm_cb) f->confirm_cb(false, f->user);
        modal_pop();
    } else if (b == '\r' || b == '\n') {
        if (f->confirm_cb) f->confirm_cb(f->confirm_yes, f->user);
        modal_pop();
    }
}

static void modal_progress_feed(ModalFrame* f, uint8_t b, char csi_final) {
    (void)csi_final;
    if (f->progress_done) {
        // Any key dismisses
        modal_pop();
        return;
    }
    if (b == 0x1B || b == 'q' || b == 'Q') {
        // ESC or q requests cancel
        f->progress_cancel = true;
    }
}

// Returns true if the modal stack consumed the byte.
static bool modal_feed_byte(uint8_t b) {
    ModalFrame* f = modal_top();
    if (!f) return false;
    switch (f->kind) {
        case M_PICKER:   modal_picker_feed  (f, b, 0); break;
        case M_SLIDER:   modal_slider_feed  (f, b, 0); break;
        case M_CONFIRM:  modal_confirm_feed (f, b, 0); break;
        case M_PROGRESS: modal_progress_feed(f, b, 0); break;
        case M_HELP:     modal_pop(); break;   // any key dismisses help
        default: break;
    }
    s_force_redraw = true;
    return true;
}

// CSI sequence dispatched into the modal.
static bool modal_feed_csi(char final_byte, const char* params) {
    ModalFrame* f = modal_top();
    if (!f) return false;
    switch (f->kind) {
        case M_HELP:    modal_pop(); break;     // any CSI key dismisses help
        case M_PICKER:  modal_picker_feed (f, 0, final_byte); break;
        case M_SLIDER:  {
            // Handle PgUp/PgDn (params="5"/"6", final='~')
            if (final_byte == '~' && params) {
                if (params[0] == '5') {
                    f->value -= f->step * 100.0f;
                    if (f->value < f->min) f->value = f->min;
                } else if (params[0] == '6') {
                    f->value += f->step * 100.0f;
                    if (f->value > f->max) f->value = f->max;
                }
            } else {
                modal_slider_feed(f, 0, final_byte);
            }
            break;
        }
        case M_CONFIRM:  modal_confirm_feed (f, 0, final_byte); break;
        case M_PROGRESS: modal_progress_feed(f, 0, final_byte); break;
        default: break;
    }
    s_force_redraw = true;
    return true;
}

// ===========================================================================
// Top-level input handlers
// ===========================================================================

static void on_arrow(char dir) {
    // Modal swallows arrows when present.
    if (modal_top()) {
        modal_feed_csi(dir, nullptr);
        return;
    }

    // Signal tab: 2D grid navigation.
    // Field 0 (SIGNAL_RESET_FIELD) = Reset-all row (no Up/Down grid nav).
    // Fields SIGNAL_GRID_BASE..SIGNAL_GRID_BASE+31 = 4x8 switch grid.
    // Left at sw=0 → prev tab; Right at sw=7 && last dev → next tab.
    // Up from grid dev=0 → back to Reset row; Down from Reset row → grid dev=0 sw=0.
    if (s_tab == TAB_SIGNAL && (dir == 'A' || dir == 'B' || dir == 'C' || dir == 'D')) {
        if (s_field == SIGNAL_RESET_FIELD) {
            // On the Reset row
            if (dir == 'A') {
                // Up: no-op (already at top)
            } else if (dir == 'B') {
                // Down: jump into grid at dev=0 sw=0
                s_signal_dev = 0;
                s_signal_sw  = 0;
                s_field      = (uint8_t)(SIGNAL_GRID_BASE);
            } else if (dir == 'D') {
                // Left: prev tab
                if (s_tab > 0) s_tab = (MenuTab)((int)s_tab - 1);
                s_field = 0;
            } else if (dir == 'C') {
                // Right: next tab
                if ((int)s_tab + 1 < NUM_TABS) s_tab = (MenuTab)((int)s_tab + 1);
                s_field = 0;
            }
        } else {
            // In the grid
            int gf  = (int)s_field - SIGNAL_GRID_BASE;
            int dev = gf / 8;
            int sw  = gf % 8;
            if (dir == 'A') {       // Up
                if (dev > 0) {
                    dev--;
                } else {
                    // Back to Reset row
                    s_field = (uint8_t)SIGNAL_RESET_FIELD;
                    s_force_redraw = true;
                    return;
                }
            } else if (dir == 'B') { // Down
                if (dev + 1 < ADGS_API_MAIN_DEVICES) dev++;
                // else stay on last row (no wrap to Reset row going down)
            } else if (dir == 'D') { // Left
                if (sw == 0) {
                    // Edge: prev tab
                    if (s_tab > 0) s_tab = (MenuTab)((int)s_tab - 1);
                    s_field = 0;
                    s_force_redraw = true;
                    return;
                }
                sw--;
            } else if (dir == 'C') { // Right
                bool last_col = (sw == 7);
                bool last_dev = (dev == ADGS_API_MAIN_DEVICES - 1);
                if (last_col && last_dev) {
                    // Edge: next tab
                    if ((int)s_tab + 1 < NUM_TABS) s_tab = (MenuTab)((int)s_tab + 1);
                    s_field = 0;
                    s_force_redraw = true;
                    return;
                }
                if (sw < 7) {
                    sw++;
                } else {
                    // sw==7 but not last dev: wrap to next dev sw=0
                    dev++;
                    sw = 0;
                }
            }
            s_field      = (uint8_t)(SIGNAL_GRID_BASE + dev * 8 + sw);
            s_signal_dev = (uint8_t)dev;
            s_signal_sw  = (uint8_t)sw;
        }
        s_force_redraw = true;
        return;
    }

    if (dir == 'A') {            // Up — previous field
        if (s_field > 0) s_field--;
    } else if (dir == 'B') {     // Down — next field
        uint8_t max = 0;
        if (s_tab >= TAB_CH0 && s_tab <= TAB_CH3) max = NUM_CH_FIELDS;
        else if (s_tab == TAB_POWER)               max = NUM_PWR_FIELDS;
        else if (s_tab == TAB_SIGNAL)              max = NUM_SIGNAL_FIELDS;
        else if (s_tab == TAB_HAT)                 max = NUM_HAT_FIELDS;
        else if (s_tab == TAB_SETTINGS)            max = NUM_SET_FIELDS;
        if (max > 0 && s_field + 1 < max) s_field++;
    } else if (dir == 'C') {     // Right — next tab
        if ((int)s_tab + 1 < NUM_TABS) s_tab = (MenuTab)((int)s_tab + 1);
        s_field = 0;
    } else if (dir == 'D') {     // Left — prev tab
        if (s_tab > 0) s_tab = (MenuTab)((int)s_tab - 1);
        s_field = 0;
    }
    s_force_redraw = true;
}

static void on_enter(void) {
    if (modal_top()) {
        modal_feed_byte('\r');
        return;
    }
    if (s_tab >= TAB_CH0 && s_tab <= TAB_CH3) {
        activate_channel_field(s_tab - TAB_CH0, (ChannelField)s_field);
    } else if (s_tab == TAB_POWER) {
        activate_power_field((PowerField)s_field);
    } else if (s_tab == TAB_SIGNAL) {
        activate_signal_field((int)s_field);
    } else if (s_tab == TAB_HAT) {
        activate_hat_field((HatTabField)s_field);
    } else if (s_tab == TAB_SETTINGS) {
        activate_settings_field((SettingsField)s_field);
    }
}

static void on_escape(void) {
    // Pop modal first; double-ESC at top-level exits.
    if (modal_top()) {
        ModalFrame* f = modal_top();
        if (f->kind == M_PROGRESS && !f->progress_done) {
            // Request cancel rather than immediate pop
            f->progress_cancel = true;
            s_force_redraw = true;
            return;
        }
        modal_pop();
        return;
    }
    uint32_t now = millis_now();
    if (s_last_esc_was_lone && (now - s_last_esc_ms) < 600) {
        s_want_exit = true;
        cli_menu_leave();
        return;
    }
    s_last_esc_was_lone = true;
    s_last_esc_ms = now;
    show_toast("Press ESC again or 'q' to quit", TERM_FG_B_BLACK, 1200);
}

// ===========================================================================
// CSI / ESC parser
// ===========================================================================

static void csi_dispatch(char final_byte) {
    s_csi_params[s_csi_len] = 0;
    if (final_byte == 'A' || final_byte == 'B' ||
        final_byte == 'C' || final_byte == 'D') {
        on_arrow(final_byte);
    } else if (final_byte == 'H' || final_byte == 'F') {
        // Home / End for slider
        if (modal_top()) modal_feed_csi(final_byte, s_csi_params);
    } else if (final_byte == '~') {
        // PgUp/PgDn etc — pass to modal if any
        if (modal_top()) modal_feed_csi(final_byte, s_csi_params);
    } else if (final_byte == 'R') {
        // Cursor Position Report — parse "rows;cols" into cli_term_on_size
        int rr = 0, cc = 0;
        if (sscanf(s_csi_params, "%d;%d", &rr, &cc) == 2 && rr > 0 && cc > 0) {
            cli_term_on_size(rr, cc);
        }
    }
}

static void feed_normal(uint8_t b) {
    if (b == 0x1B) { s_pstate = ST_ESC; return; }
    if (b == 'q' || b == 'Q') {
        s_want_exit = true;
        cli_menu_leave();
        return;
    }
    if (b == 'r' || b == 'R') {
        if (!modal_top()) { s_force_redraw = true; invalidate_front(); return; }
    }
    if (b == 'y' || b == 'Y' || b == 'n' || b == 'N') {
        if (modal_top()) { modal_feed_byte(b); return; }
    }
    if (b == '\r' || b == '\n' || b == ' ') {
        on_enter();
        return;
    }
    if (b == '?') {
        if (!modal_top()) { open_help(); }
        return;
    }
    s_last_esc_was_lone = false;  // any non-ESC clears the double-ESC arming
}

static void feed_esc(uint8_t b) {
    if (b == '[') {
        s_pstate = ST_CSI;
        s_csi_len = 0;
        s_csi_params[0] = 0;
        return;
    }
    if (b == 'O') { s_pstate = ST_SS3; return; }
    if (b == 0x1B) {
        // Two ESCs in a row - treat as quit signal
        s_want_exit = true;
        cli_menu_leave();
        s_pstate = ST_NORMAL;
        return;
    }
    // Lone ESC followed by something else — treat as ESC keypress.
    on_escape();
    s_pstate = ST_NORMAL;
    // Reprocess the byte as normal.
    feed_normal(b);
}

static void feed_csi(uint8_t b) {
    if (b >= 0x30 && b <= 0x3F) {
        if (s_csi_len + 1 < (int)sizeof(s_csi_params)) {
            s_csi_params[s_csi_len++] = (char)b;
        }
        return;
    }
    if (b >= 0x40 && b <= 0x7E) {
        csi_dispatch((char)b);
        s_pstate = ST_NORMAL;
        return;
    }
}

static void feed_ss3(uint8_t b) {
    // SS3-prefixed function keys (xterm sends ESC O A for some arrows).
    if (b == 'A' || b == 'B' || b == 'C' || b == 'D') {
        on_arrow((char)b);
    } else if (b == 'H') {
        if (modal_top()) modal_feed_csi('H', nullptr);
    } else if (b == 'F') {
        if (modal_top()) modal_feed_csi('F', nullptr);
    }
    s_pstate = ST_NORMAL;
}

// ===========================================================================
// Contextual field hint (row s_rows - 4)
// ===========================================================================

static void render_field_hint(int row) {
    const char* hint = nullptr;

    if (s_tab >= TAB_CH0 && s_tab <= TAB_CH3) {
        switch (s_field) {
            case CH_FIELD_FUNCTION:
                hint = "Hint: changing function reconfigures ADC + clears stale alerts."; break;
            case CH_FIELD_ADC_RATE:
                hint = "Hint: higher rates trade noise rejection for bandwidth."; break;
            case CH_FIELD_ADC_MUX:
                hint = "Hint: AGND->AGND mux is for self-test/offset characterisation."; break;
            case CH_FIELD_DAC_VOLTAGE:
                hint = "Hint: requires VOUT mode; switch via Function picker if currently HIGH-Z/IIN."; break;
            case CH_FIELD_DO_TOGGLE:
                hint = "Hint: DO state is independent of channel function."; break;
            case CH_FIELD_CLEAR_ALERT:
                hint = "Hint: clears only this channel's alert register, not global alerts."; break;
            default: break;
        }
    } else if (s_tab == TAB_POWER) {
        switch (s_field) {
            case PWR_FIELD_VADJ1:
            case PWR_FIELD_VADJ2:
            case PWR_FIELD_VLOGIC:
                hint = "Hint: VADJ rails feed connector pins; lock status comes from the active board profile."; break;
            case PWR_FIELD_EFUSE1:
            case PWR_FIELD_EFUSE2:
            case PWR_FIELD_EFUSE3:
            case PWR_FIELD_EFUSE4:
                hint = "Hint: e-fuse trips on overcurrent; toggle to reset after physical issue is resolved."; break;
            case PWR_FIELD_15V:
            case PWR_FIELD_MUX:
            case PWR_FIELD_USBHUB:
                hint = "Hint: enabling these affects the entire signal path; verify load before toggling."; break;
            default: break;
        }
    } else if (s_tab == TAB_SIGNAL) {
        if (s_field == SIGNAL_RESET_FIELD) {
            hint = "Hint: opens all switches simultaneously; brief power glitch possible.";
        } else {
            hint = "Hint: switches use safe make-before-break sequencing; readback verifies actual state.";
        }
    } else if (s_tab == TAB_HAT) {
        switch (s_field) {
            case HAT_FIELD_PIN0:
            case HAT_FIELD_PIN1:
            case HAT_FIELD_PIN2:
            case HAT_FIELD_PIN3:
                hint = "Hint: HAT pins 1-4 are reserved for SWD; only GPIO functions selectable."; break;
            case HAT_FIELD_CONN_A:
            case HAT_FIELD_CONN_B:
                hint = "Hint: power must be off before changing pin functions or VADJ."; break;
            case HAT_FIELD_IO_VOLT:
                hint = "Hint: HVPAK level translation; range 1.2V to 5.5V."; break;
            case HAT_FIELD_DETECT:
                hint = "Hint: re-reads HAT detect pin and queries UART for type identification."; break;
            case HAT_FIELD_RESET:
                hint = "Hint: returns HAT to safe defaults (all pins disconnected, power off)."; break;
            default: break;
        }
    } else if (s_tab == TAB_SETTINGS) {
        switch (s_field) {
            case SET_FIELD_CLEAR_FAULTS:
                hint = "Hint: clears global + per-channel + supply alert registers; transient faults may reappear."; break;
            case SET_FIELD_USBPD:
                hint = "Hint: requires PD-capable supply; renegotiation may interrupt power briefly."; break;
            case SET_FIELD_BOARD_PROFILE:
                hint = "Hint: persisted to NVS; sets default VLOGIC/VADJ rail nominal voltages."; break;
            case SET_FIELD_DIAG_0:
            case SET_FIELD_DIAG_1:
            case SET_FIELD_DIAG_2:
            case SET_FIELD_DIAG_3:
                hint = "Hint: AD74416H has 4 multiplexed diagnostic channels; assign sources here."; break;
            case SET_FIELD_CAL_VADJ1:
                hint = "Hint: auto-cal sweeps DAC and reads back via ADC; takes ~5 seconds."; break;
            case SET_FIELD_CAL_VADJ2:
                hint = "Hint: auto-cal sweeps DAC and reads back via ADC; takes ~5 seconds."; break;
            case SET_FIELD_CAL_SAVE:
                hint = "Hint: writes calibration table to NVS; persists across reboot."; break;
            case SET_FIELD_CAL_LOAD:
                hint = "Hint: restores last saved calibration; current values will be overwritten."; break;
            case SET_FIELD_CAL_CLEAR:
                hint = "Hint: removes all calibration; voltage outputs revert to theoretical conversion."; break;
            default: break;
        }
    }

    if (hint) {
        draw_text(row, 1, hint, TERM_FG_B_BLACK, ATTR_DIM);
    }
}

// ===========================================================================
// Render orchestration
// ===========================================================================

static void render_full(void) {
    if (!take_snapshot()) return;
    if (!s_back || !s_front) return;

    clear_back();
    render_header_bar();
    render_tab_bar();

    if (s_tab == TAB_OVERVIEW) {
        render_overview();
    } else if (s_tab >= TAB_CH0 && s_tab <= TAB_CH3) {
        render_channel_tab(s_tab - TAB_CH0);
    } else if (s_tab == TAB_POWER) {
        render_power_tab();
    } else if (s_tab == TAB_SIGNAL) {
        render_signal_tab();
    } else if (s_tab == TAB_HAT) {
        render_hat_tab();
    } else if (s_tab == TAB_SETTINGS) {
        render_settings_tab();
    }

    render_status_strip();
    render_field_hint(s_rows - 4);
    render_modal();

    if (s_force_redraw) {
        invalidate_front();
        s_force_redraw = false;
    }
    present();
}

// ===========================================================================
// Lifecycle / public API
// ===========================================================================

extern "C" void cli_menu_init(void) {
    s_active           = false;
    s_alt_out          = false;
    s_want_exit        = false;
    s_snap.valid       = false;
    s_last_redraw_ms   = 0;
    s_last_probe_ms    = 0;
    s_force_redraw     = true;
    s_pstate           = ST_NORMAL;
    s_tab              = TAB_OVERVIEW;
    s_field            = 0;
    s_modal_top        = -1;
    for (int i = 0; i < MODAL_STACK_DEPTH; i++) s_modal_stack[i].kind = M_NONE;
    s_toast_msg[0]      = 0;
    s_toast_set_ms      = 0;
    s_toast_duration_ms = 0;
}

extern "C" bool cli_menu_active(void)        { return s_active; }
extern "C" bool cli_menu_want_exit(void)     { return s_want_exit; }
extern "C" void cli_menu_clear_want_exit(void) { s_want_exit = false; }

static void enter_alt_screen(void) {
    static const char kSeq[] = "\x1b[?1049h" "\x1b[?25l" "\x1b[2J";
    term_emit(kSeq, sizeof(kSeq) - 1);
}

static void leave_alt_screen(void) {
    static const char kSeq[] = "\x1b[0m" "\x1b[?25h" "\x1b[?1049l";
    term_emit(kSeq, sizeof(kSeq) - 1);
}

extern "C" void cli_menu_enter(void) {
    if (s_active) return;
    if (bbpCdcClaimed()) {
        term_println("Menu unavailable: CDC #0 is held by BBP.");
        return;
    }
    int rows = term_rows();
    int cols = term_cols();
    if (!buffer_alloc(rows, cols)) {
        term_println("Menu: failed to allocate cell buffer.");
        return;
    }
    enter_alt_screen();
    s_active            = true;
    s_alt_out           = false;
    s_want_exit         = false;
    s_last_redraw_ms    = 0;
    s_slow_snap_last_ms = 0;
    s_last_probe_ms     = millis_now();
    s_force_redraw      = true;
    s_pstate            = ST_NORMAL;
    s_last_esc_was_lone = false;
    s_modal_top         = -1;
    s_toast_msg[0]      = 0;
    take_snapshot_slow();
    invalidate_front();
    render_full();
    s_last_redraw_ms    = millis_now();
    s_slow_snap_last_ms = millis_now();
}

extern "C" void cli_menu_leave(void) {
    if (!s_active && !s_alt_out) {
        buffer_free();
        return;
    }
    leave_alt_screen();
    s_active   = false;
    s_alt_out  = false;
    buffer_free();
}

extern "C" void cli_menu_preempt(void) {
    if (!s_active) return;
    leave_alt_screen();
    s_alt_out = true;
}

extern "C" void cli_menu_feed(uint8_t b) {
    if (!s_active) return;

    // ESC arming logic: any byte other than another ESC clears the lone-ESC
    // flag *unless* a modal is up (modal handles its own ESC semantics).
    switch (s_pstate) {
        case ST_NORMAL: feed_normal(b); break;
        case ST_ESC:    feed_esc(b);    break;
        case ST_CSI:    feed_csi(b);    break;
        case ST_SS3:    feed_ss3(b);    break;
    }
}

extern "C" void cli_menu_tick(void) {
    if (!s_active) return;
    if (bbpCdcClaimed()) {
        s_active   = false;
        s_alt_out  = false;
        s_want_exit = true;
        buffer_free();
        return;
    }
    if (s_alt_out) {
        enter_alt_screen();
        s_alt_out = false;
        s_force_redraw = true;
        invalidate_front();
    }

    // Periodic terminal-size probe (every ~2 s).
    uint32_t now = millis_now();
    if (now - s_last_probe_ms > 2000) {
        s_last_probe_ms = now;
        term_probe_size();
    }

    // React to size change.
    int rows = term_rows();
    int cols = term_cols();
    if (rows != s_rows || cols != s_cols) {
        if (buffer_alloc(rows, cols)) {
            s_force_redraw = true;
            invalidate_front();
        } else {
            // Allocation failed — gracefully exit menu so CLI prompt returns.
            leave_alt_screen();
            s_active   = false;
            s_alt_out  = false;
            s_want_exit = true;
            buffer_free();
            return;
        }
    }

    // Lone ESC timeout: if user pressed ESC and didn't follow up with anything
    // within ~120 ms, treat it as an ESC keypress (handled by feed_esc when
    // the next byte arrives, but we also flush here so a lone ESC from the
    // modal shows feedback promptly).
    // (No-op: handled in feed_esc on next byte.)

    // Slow snapshot: SPI + WiFi + HAT reads at ~500 ms cadence
    if (now - s_slow_snap_last_ms >= 500) {
        s_slow_snap_last_ms = now;
        take_snapshot_slow();
    }

    if (now - s_last_redraw_ms < 100 && !s_force_redraw) return;
    s_last_redraw_ms = now;
    render_full();
}

extern "C" void cli_cmd_menu(const char* args) {
    (void)args;
    cli_menu_enter();
}
