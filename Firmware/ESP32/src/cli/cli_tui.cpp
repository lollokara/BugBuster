// =============================================================================
// cli_tui.cpp - Read-only full-screen dashboard for the CLI.
// =============================================================================

#include "cli_tui.h"
#include "cli_term.h"
#include "cli_shared.h"
#include "tasks.h"
#include "bbp.h"
#include "config.h"
#include "ad74416h_regs.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include <stdio.h>
#include <string.h>

// ---------------------------------------------------------------------------
// Module state
// ---------------------------------------------------------------------------
static bool     s_active         = false;
static bool     s_alt_out        = false;  // alt-screen currently unwound
static bool     s_want_exit      = false;
static uint32_t s_entered_ms     = 0;
static uint32_t s_last_redraw_ms = 0;

// Snapshot of device state taken under mutex, rendered without the mutex.
struct TuiSnapshot {
    bool     spiOk;
    float    dieTemperature;
    uint16_t liveStatus;
    struct {
        ChannelFunction function;
        AdcRange        adcRange;
        uint32_t        adcRawCode;
        float           adcValue;
        uint16_t        dacCode;
        bool            doState;
    } ch[4];
    bool     valid;
};

static TuiSnapshot s_snap;

// ---------------------------------------------------------------------------
// Name lookup (duplicated from cli_cmds_dev.cpp — kept local to avoid
// cross-module dependency; both copies map the same enum values.)
// ---------------------------------------------------------------------------
static const char* tui_func_name(ChannelFunction f) {
    switch (f) {
        case CH_FUNC_HIGH_IMP:  return "HIGH_IMP";
        case CH_FUNC_VOUT:      return "VOUT";
        case CH_FUNC_IOUT:      return "IOUT";
        case CH_FUNC_VIN:       return "VIN";
        case CH_FUNC_IIN_EXT_PWR:   return "IIN_EXT";
        case CH_FUNC_IIN_LOOP_PWR:  return "IIN_LOOP";
        case CH_FUNC_RES_MEAS:  return "RES";
        case CH_FUNC_DIN_LOGIC: return "DIN_LOG";
        case CH_FUNC_DIN_LOOP:  return "DIN_LOOP";
        default:                return "?";
    }
}

// ---------------------------------------------------------------------------
// Primitives
// ---------------------------------------------------------------------------

static void tui_goto(int row, int col)
{
    char seq[16];
    int n = snprintf(seq, sizeof(seq), "\x1b[%d;%dH", row, col);
    if (n > 0) term_emit(seq, (size_t)n);
}

static void tui_clear_screen(void)
{
    term_emit("\x1b[2J", 4);
}

static void tui_hide_cursor(void)    { term_emit("\x1b[?25l", 6); }
static void tui_show_cursor(void)    { term_emit("\x1b[?25h", 6); }
static void tui_alt_enter(void)      { term_emit("\x1b[?1049h", 8); }
static void tui_alt_leave(void)      { term_emit("\x1b[?1049l", 8); }
static void tui_sgr_reset(void)      { term_emit("\x1b[0m", 4); }

// Take a snapshot of g_deviceState with a short timeout. Returns true on
// success; otherwise the previous snapshot is retained.
static bool take_snapshot(void)
{
    if (xSemaphoreTake(g_stateMutex, pdMS_TO_TICKS(5)) != pdTRUE) {
        return false;
    }
    s_snap.spiOk          = g_deviceState.spiOk;
    s_snap.dieTemperature = g_deviceState.dieTemperature;
    s_snap.liveStatus     = g_deviceState.liveStatus;
    for (int c = 0; c < 4; c++) {
        s_snap.ch[c].function   = g_deviceState.channels[c].function;
        s_snap.ch[c].adcRange   = g_deviceState.channels[c].adcRange;
        s_snap.ch[c].adcRawCode = g_deviceState.channels[c].adcRawCode;
        s_snap.ch[c].adcValue   = g_deviceState.channels[c].adcValue;
        s_snap.ch[c].dacCode    = g_deviceState.channels[c].dacCode;
        s_snap.ch[c].doState    = g_deviceState.channels[c].doState;
    }
    xSemaphoreGive(g_stateMutex);
    s_snap.valid = true;
    return true;
}

// ---------------------------------------------------------------------------
// Rendering
// ---------------------------------------------------------------------------

// Map the ADC range of a channel to a [0..1] fill fraction for the bar graph.
// Returns -1 when the bar does not make sense for the active function.
static float range_to_fraction(ChannelFunction f, AdcRange r, float v)
{
    // Some functions aren't voltage/current measurements — no bar.
    switch (f) {
        case CH_FUNC_HIGH_IMP:
        case CH_FUNC_DIN_LOGIC:
        case CH_FUNC_DIN_LOOP:
            return -1.0f;
        default: break;
    }

    float lo = 0.0f, hi = 12.0f;
    switch (r) {
        case ADC_RNG_0_12V:             lo = 0.0f;    hi = 12.0f;   break;
        case ADC_RNG_NEG12_12V:         lo = -12.0f;  hi = 12.0f;   break;
        case ADC_RNG_NEG2_5_2_5V:       lo = -2.5f;   hi = 2.5f;    break;
        case ADC_RNG_NEG0_3125_0V:      lo = -0.3125f;hi = 0.0f;    break;
        case ADC_RNG_0_0_3125V:         lo = 0.0f;    hi = 0.3125f; break;
        case ADC_RNG_0_0_625V:          lo = 0.0f;    hi = 0.625f;  break;
        case ADC_RNG_NEG0_3125_0_3125V: lo = -0.3125f;hi = 0.3125f; break;
        case ADC_RNG_NEG104MV_104MV:    lo = -0.104f; hi = 0.104f;  break;
        default: break;
    }
    if (hi <= lo) return -1.0f;
    float frac = (v - lo) / (hi - lo);
    if (frac < 0.0f) frac = 0.0f;
    if (frac > 1.0f) frac = 1.0f;
    return frac;
}

static void render_header(int /*cols*/)
{
    uint32_t up_ms = millis_now() - s_entered_ms;
    unsigned int up_s = up_ms / 1000;
    unsigned int hh = up_s / 3600;
    unsigned int mm = (up_s % 3600) / 60;
    unsigned int ss = up_s % 60;

    tui_goto(1, 1);
    term_erase_to_eol();
    term_bold_cprint(TERM_FG_B_CYAN, " BugBuster ");
    term_print("│ ");
    if (s_snap.spiOk) term_cprint(TERM_FG_B_GREEN, "SPI:OK");
    else              term_cprint(TERM_FG_B_RED,   "SPI:FAIL");
    term_printf(" │ T:%.1fC", s_snap.dieTemperature);
    term_printf(" │ LIVE:0x%04X", s_snap.liveStatus);
    term_printf(" │ up %02u:%02u:%02u", hh, mm, ss);

    // Separator under header
    tui_goto(2, 1);
    term_erase_to_eol();
    term_cprint(TERM_FG_B_BLACK,
        "────────────────────────────────────────────────────────────────────────");
}

// Render a 4-row stanza for one channel starting at `row`:
//   row+0  CH n · FUNC          DAC nnn   DIN n   DO ON/OFF   ALRT 0x....
//   row+1    ±VV.VVVV V    [████████░░░░░░░░░░░░░░░░░░] 50%
//   row+2  (blank separator)
static void render_channel_card(int row, int ch, int cols)
{
    // Compute bar width from terminal cols: reserve ~18 cols for the number +
    // label, ~8 for the percentage, the rest for the bar.
    int bar_slots = cols - 38;
    if (bar_slots < 8)  bar_slots = 8;
    if (bar_slots > 40) bar_slots = 40;

    const char* fname = tui_func_name(s_snap.ch[ch].function);
    float   v         = s_snap.ch[ch].adcValue;
    uint16_t dac      = s_snap.ch[ch].dacCode;
    bool    doOn      = s_snap.ch[ch].doState;

    // Header line of the card
    tui_goto(row, 1);
    term_erase_to_eol();
    term_bold_cprint(TERM_FG_B_CYAN, " CH ");
    term_cprintf(TERM_FG_B_CYAN, "%d", ch);
    term_print("  ");
    term_cprintf(TERM_FG_B_YELLOW, "%-10s", fname);
    term_printf("    DAC ");
    term_cprintf(TERM_FG_B_WHITE, "%5u", dac);
    term_print("   DO ");
    if (doOn) term_cprint(TERM_FG_B_GREEN, "ON ");
    else      term_cprint(TERM_FG_B_BLACK, "OFF");

    // Big-value + bar line
    tui_goto(row + 1, 1);
    term_erase_to_eol();
    term_print("   ");
    TermColor vc = TERM_FG_B_GREEN;
    if (v < -11.5f || v > 11.5f) vc = TERM_FG_B_RED;
    else if (v < 0.0f)            vc = TERM_FG_B_YELLOW;
    term_attr(TERM_ATTR_BOLD);
    term_cprintf(vc, "%+9.4f V", v);
    term_reset_sgr();
    term_print("   ");

    float frac = range_to_fraction(s_snap.ch[ch].function,
                                   s_snap.ch[ch].adcRange, v);
    if (frac >= 0.0f) {
        int filled = (int)(frac * bar_slots + 0.5f);
        if (filled < 0) filled = 0;
        if (filled > bar_slots) filled = bar_slots;
        term_print("[");
        for (int i = 0; i < filled;    i++) term_cprint(TERM_FG_B_GREEN,  "█");
        for (int i = filled; i < bar_slots; i++) term_cprint(TERM_FG_B_BLACK, "░");
        term_printf("] %3d%%", (int)(frac * 100.0f + 0.5f));
    } else {
        term_cprint(TERM_FG_B_BLACK, "  (no range)");
    }
}

static void render_channels(int rows, int cols)
{
    // Each card occupies 3 rows (header + value/bar + spacer).
    int start_row = 3;
    for (int ch = 0; ch < 4; ch++) {
        if (start_row + 2 > rows - 2) break;
        render_channel_card(start_row, ch, cols);
        start_row += 3;
    }
}

static void render_footer(int rows, int /*cols*/)
{
    int row = rows - 1;
    if (row < 10) row = 10;
    tui_goto(row, 1);
    term_erase_to_eol();
    term_cprint(TERM_FG_B_BLACK,
        "────────────────────────────────────────────────────────────────────────");

    tui_goto(row + 1, 1);
    term_erase_to_eol();
    term_bold_cprint(TERM_FG_B_YELLOW, " q");
    term_print("/");
    term_bold_cprint(TERM_FG_B_YELLOW, "Ctrl-C");
    term_print(" exit   ");
    term_bold_cprint(TERM_FG_B_YELLOW, "r");
    term_print(" force redraw   ");
    term_cprint(TERM_FG_B_BLACK, "(read-only snapshot @ 2 Hz)");
}

static void render_full(void)
{
    int rows = term_rows();
    int cols = term_cols();
    if (rows < 14) rows = 14;   // sanity min (card × 4 + header + footer)
    if (cols < 40) cols = 40;

    take_snapshot();
    tui_clear_screen();
    render_header(cols);
    render_channels(rows, cols);
    render_footer(rows, cols);
    // Park cursor out of sight
    tui_goto(rows, 1);
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

extern "C" void cli_tui_init(void)
{
    s_active         = false;
    s_alt_out        = false;
    s_want_exit      = false;
    s_snap.valid     = false;
    s_entered_ms     = 0;
    s_last_redraw_ms = 0;
}

extern "C" bool cli_tui_active(void)        { return s_active; }
extern "C" bool cli_tui_want_exit(void)     { return s_want_exit; }
extern "C" void cli_tui_clear_want_exit(void) { s_want_exit = false; }

extern "C" void cli_tui_enter(void)
{
    if (s_active) return;
    if (bbpCdcClaimed()) {
        // Cannot emit anything — refuse quietly.
        term_println("TUI unavailable: CDC #0 is held by BBP.");
        return;
    }
    tui_alt_enter();
    tui_hide_cursor();
    s_active         = true;
    s_alt_out        = false;
    s_want_exit      = false;
    s_entered_ms     = millis_now();
    s_last_redraw_ms = 0;  // force immediate first render on next tick
    render_full();
    s_last_redraw_ms = millis_now();
}

extern "C" void cli_tui_leave(void)
{
    if (!s_active && !s_alt_out) return;
    tui_sgr_reset();
    tui_alt_leave();
    tui_show_cursor();
    s_active    = false;
    s_alt_out   = false;
}

extern "C" void cli_tui_preempt(void)
{
    if (!s_active) return;
    // Optimistically unwind the alt-screen so that if the 0xBB turns into a
    // BBP handshake, the host terminal is already clean. If it does NOT
    // complete a handshake, cli_tui_tick will re-enter on the next tick.
    tui_sgr_reset();
    tui_alt_leave();
    tui_show_cursor();
    s_alt_out = true;
}

extern "C" void cli_tui_feed(uint8_t b)
{
    if (!s_active) return;
    if (b == 0x03 || b == 'q' || b == 'Q') {
        cli_tui_leave();
        s_want_exit = true;
        return;
    }
    if (b == 'r' || b == 'R') {
        s_last_redraw_ms = 0;  // force redraw on next tick
        return;
    }
    // Any other keypress is ignored (Phase 4 is read-only).
}

extern "C" void cli_tui_tick(void)
{
    if (!s_active) return;

    // If BBP has now claimed the port, we can't emit anything. Drop quietly.
    if (bbpCdcClaimed()) {
        s_active  = false;
        s_alt_out = false;
        s_want_exit = true;
        return;
    }

    // Recover from a preempted unwind that didn't turn into a handshake.
    if (s_alt_out) {
        tui_alt_enter();
        tui_hide_cursor();
        s_alt_out = false;
        s_last_redraw_ms = 0;
    }

    uint32_t now = millis_now();
    if (now - s_last_redraw_ms < 500) return;
    s_last_redraw_ms = now;
    render_full();
}

extern "C" void cli_cmd_tui(const char* args)
{
    (void)args;
    cli_tui_enter();
}
