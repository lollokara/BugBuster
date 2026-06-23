#include "menu.h"
#include "config.h"
#include "gfx.h"
#include "theme.h"
#include "settings.h"
#include "buttons.h"
#include "display.h"
#include "ui.h"
#include "ddp.h"
#include "c6_config.h"
#include "daq_config_registry.h"

#include <math.h>
#include <string.h>
#include <stdio.h>
#include "esp_timer.h"
#include "esp_system.h"
#include "driver/temperature_sensor.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// ===========================================================================
// Data model
// ===========================================================================
typedef enum {
    IT_SUBMENU,    // enters another menu
    IT_TOGGLE,     // OK flips a bool
    IT_CYCLE,      // OK cycles through options
    IT_BARGRAPH,   // OK opens a bargraph value editor
    IT_INFO,       // read-only label + value (diagnostics)
} item_type_t;

typedef struct menu menu_t;

typedef struct {
    const char *label;
    item_type_t type;
    const menu_t *sub;                       // IT_SUBMENU

    void (*value)(char *buf, int n);         // right-side text
    void (*ok)(void);                        // IT_TOGGLE / IT_CYCLE action
    bool (*visible)(void);                   // NULL => always visible
    bool (*warn)(void);                      // NULL => no warning icon

    // IT_INFO live-preview: when non-NULL, OK opens a detail screen that plots
    // a scrolling sparkline of this sampler's return value.
    float (*sample)(void);

    // IT_BARGRAPH
    int  *bar_ref;
    int   bar_min, bar_max, bar_step;
    void (*bar_fmt)(char *buf, int n, int val);
    void (*bar_change)(int val);             // optional live-apply
} menu_item_t;

struct menu {
    const char *title;
    const menu_item_t *items;
    int count;
};

// ===========================================================================
// Settings callbacks
// ===========================================================================
static void v_onoff(char *b, int n, bool on) { snprintf(b, n, "%s", on ? "ON" : "OFF"); }

// Persist locally and push the changed settings to the P4 (C6 -> P4 event) as a
// key-addressed TLV batch (covers HAT/DSP/screen/neopixel/wifi scalars).
static void settings_commit(void)
{
    settings_save();
    c6_config_send();
}

static void val_autorange(char *b, int n) { v_onoff(b, n, g_settings.autoranging); }
static void ok_autorange(void)            { g_settings.autoranging = !g_settings.autoranging; settings_commit(); }

static bool vis_manual(void) { return !g_settings.autoranging; }
static bool warn_manual(void){ return !g_settings.autoranging; }

static bool vis_fft(void) { return g_settings.fft_enable; }
static bool vis_wifi(void) { return g_settings.wifi_enable; }

static void val_range(char *b, int n) { snprintf(b, n, "%s", SETTINGS_RANGE[g_settings.range_idx]); }
static void ok_range(void)            { g_settings.range_idx = (g_settings.range_idx + 1) % 3; settings_commit(); }

static void val_srate(char *b, int n) { snprintf(b, n, "%s", SETTINGS_SAMPLERATE[g_settings.sample_rate_idx]); }
static void ok_srate(void)            { g_settings.sample_rate_idx = (g_settings.sample_rate_idx + 1) % 5; settings_commit(); }

// FFT (P4 DSP) — labels read from the registry schema (single source of truth).
static void val_fft(char *b, int n)    { v_onoff(b, n, g_settings.fft_enable); }
static void ok_fft(void)               { g_settings.fft_enable = !g_settings.fft_enable; settings_commit(); }
static void val_fftlen(char *b, int n) { snprintf(b, n, "%s", daq_config_schema(DAQ_K_FFT_LENGTH)->options[g_settings.fft_length_idx]); }
static void ok_fftlen(void)            { g_settings.fft_length_idx = (g_settings.fft_length_idx + 1) % DAQ_FFT_LEN_COUNT; settings_commit(); }
static void val_fftwin(char *b, int n) { snprintf(b, n, "%s", daq_config_schema(DAQ_K_FFT_WINDOW)->options[g_settings.fft_window_idx]); }
static void ok_fftwin(void)            { g_settings.fft_window_idx = (g_settings.fft_window_idx + 1) % DAQ_WIN_COUNT; settings_commit(); }
static void val_fftsrc(char *b, int n) { snprintf(b, n, "%s", daq_config_schema(DAQ_K_FFT_SOURCE)->options[g_settings.fft_source_idx]); }
static void ok_fftsrc(void)            { g_settings.fft_source_idx = (g_settings.fft_source_idx + 1) % DAQ_FFTSRC_COUNT; settings_commit(); }

static void fmt_current(char *b, int n, int ma)
{
    if (ma >= 1000) snprintf(b, n, "%d.%d A", ma / 1000, (ma % 1000) / 100);
    else            snprintf(b, n, "%d mA", ma);
}
static void fmt_voltage(char *b, int n, int mv)
{
    snprintf(b, n, "%d.%d V", mv / 1000, (mv % 1000) / 100);
}
static void fmt_pct(char *b, int n, int v) { snprintf(b, n, "%d%%", v); }

static void val_current(char *b, int n) { fmt_current(b, n, g_settings.dut_current_ma); }
static void val_voltage(char *b, int n) { fmt_voltage(b, n, g_settings.dut_voltage_mv); }
static void val_bright(char *b, int n)  { fmt_pct(b, n, g_settings.brightness_pct); }

static void apply_brightness(int pct)
{
    int lvl = pct * 255 / 100;
    if (lvl < 0) lvl = 0;
    if (lvl > 255) lvl = 255;
    display_set_backlight((uint8_t)lvl);
}

static void val_dark(char *b, int n) { v_onoff(b, n, g_settings.dark_mode); }
static void ok_dark(void)
{
    g_settings.dark_mode = !g_settings.dark_mode;
    theme_set_dark(g_settings.dark_mode);
    ui_refresh_theme();
    settings_commit();
}

// Neopixels (C6). Color is chosen from a small preset palette (3 buttons make
// freeform RGB editing impractical; the app can set any 24-bit colour).
static const uint32_t NPX_PRESETS[] = {
    0x00FF0000, 0x0000FF00, 0x000000FF, 0x00FFFF00,
    0x0000FFFF, 0x00FF00FF, 0x00FFFFFF, 0x00FF6000,
};
static const char *const NPX_PRESET_NAMES[] = {
    "Red", "Green", "Blue", "Yellow", "Cyan", "Magenta", "White", "Orange",
};
#define NPX_PRESET_COUNT ((int)(sizeof(NPX_PRESETS) / sizeof(NPX_PRESETS[0])))

static int npx_color_idx(void)
{
    for (int i = 0; i < NPX_PRESET_COUNT; i++)
        if (NPX_PRESETS[i] == g_settings.npx_color) return i;
    return 0;
}
static void val_npxmode(char *b, int n) { snprintf(b, n, "%s", daq_config_schema(DAQ_K_NPX_MODE)->options[g_settings.npx_mode]); }
static void ok_npxmode(void)            { g_settings.npx_mode = (g_settings.npx_mode + 1) % DAQ_NPX_MODE_COUNT; settings_commit(); }
static void val_npxcol(char *b, int n)  { snprintf(b, n, "%s", NPX_PRESET_NAMES[npx_color_idx()]); }
static void ok_npxcol(void)             { g_settings.npx_color = NPX_PRESETS[(npx_color_idx() + 1) % NPX_PRESET_COUNT]; settings_commit(); }
static void val_npxbright(char *b, int n) { fmt_pct(b, n, g_settings.npx_brightness); }

// WiFi (S3 mainboard radio, relayed). Enable/mode are editable on the C6; SSID
// and password are set from the app (text entry on 3 buttons is impractical).
static void val_wifi(char *b, int n)     { v_onoff(b, n, g_settings.wifi_enable); }
static void ok_wifi(void)                { g_settings.wifi_enable = !g_settings.wifi_enable; settings_commit(); }
static void val_wifimode(char *b, int n) { snprintf(b, n, "%s", daq_config_schema(DAQ_K_WIFI_MODE)->options[g_settings.wifi_mode]); }
static void ok_wifimode(void)            { g_settings.wifi_mode = (g_settings.wifi_mode + 1) % DAQ_WIFI_MODE_COUNT; settings_commit(); }
static void val_ssid(char *b, int n)     { snprintf(b, n, "%s", g_settings.ssid[0] ? g_settings.ssid : "(set via app)"); }
static void val_wifistatus(char *b, int n)
{
    const char *s = (g_settings.wifi_status == 2) ? "Connected" :
                    (g_settings.wifi_status == 1) ? "Connecting" : "Offline";
    snprintf(b, n, "%s", s);
}

// ===========================================================================
// Diagnostics — read the snapshot pushed by the P4 (DDP_CMD_SET_DIAGNOSTICS).
// Devices are grouped into submenus; each leaf row shows a live value and, when
// selected, opens a scrolling-sparkline detail view. The C6 fills its own
// self-stats (heap, die temp, uptime) locally. When no fresh P4 frame is
// available a light simulation keeps the menu useful on the bench.
// ===========================================================================
static uint32_t s_anim_ms = 0;
static ddp_diag_t s_dg;        // current diagnostics (live or simulated)
static bool       s_dg_live = false;

// C6 internal die-temperature sensor (installed lazily in menu_init).
static temperature_sensor_handle_t s_c6_tsens = NULL;

static float wob(float base, float amp, float hz, float ph)
{
    return base + amp * sinf((float)s_anim_ms * 0.001f * hz * 6.2832f + ph);
}

// Refresh the diagnostics cache: prefer a recent DDP push, else simulate a
// representative subset so the grouped menu and sparklines work without a P4.
static void diag_refresh(void)
{
    uint32_t age;
    if (ddp_get_diag(&s_dg, &age) && age < 2000) {
        s_dg_live = true;
        return;
    }
    s_dg_live = false;
    memset(&s_dg, 0, sizeof(s_dg));
    s_dg.t_board0_c10 = (int16_t)(wob(41.5f, 0.6f, 0.05f, 0) * 10);
    s_dg.t_board1_c10 = (int16_t)(wob(43.0f, 0.8f, 0.04f, 1) * 10);
    s_dg.t_adaq0_c10  = (int16_t)(wob(52.0f, 0.7f, 0.05f, 2) * 10);
    s_dg.t_adaq1_c10  = (int16_t)(wob(53.5f, 0.7f, 0.05f, 3) * 10);
    s_dg.t_adaq2_c10  = (int16_t)(wob(51.0f, 0.7f, 0.05f, 4) * 10);
    s_dg.t_p4_c10     = (int16_t)(wob(48.0f, 0.6f, 0.06f, 5) * 10);
    s_dg.t_s3_c10     = (int16_t)(wob(45.0f, 0.6f, 0.05f, 6) * 10);
    s_dg.i_ua         = (int32_t)(wob(0.1234f, 0.01f, 0.2f, 0) * 1e6f);
    s_dg.v_uv         = (int32_t)(wob(5.000f, 0.02f, 0.1f, 1) * 1e6f);
    s_dg.p_uw         = (int32_t)(wob(0.617f, 0.03f, 0.2f, 2) * 1e6f);
    s_dg.smu_iin_ma   = (int16_t)wob(96.0f, 3.0f, 0.15f, 3);
    s_dg.smu_iout_ma  = (int16_t)wob(128.0f, 4.0f, 0.2f, 2);
    s_dg.vdut_mv      = (uint16_t)(wob(5.00f, 0.01f, 0.1f, 4) * 1000);
    s_dg.pd_mv        = (uint16_t)(wob(20.0f, 0.05f, 0.1f, 5) * 1000);
    s_dg.pd_ma        = 5000;
    s_dg.vadj1_mv     = (uint16_t)(wob(3.30f, 0.01f, 0.1f, 6) * 1000);
    s_dg.vadj2_mv     = (uint16_t)(wob(5.00f, 0.01f, 0.1f, 7) * 1000);
    s_dg.vlogic_mv    = (uint16_t)(wob(3.30f, 0.01f, 0.1f, 8) * 1000);
    s_dg.p4_free_mem_kb  = (uint16_t)wob(211.0f, 2.0f, 0.1f, 4);
    s_dg.p4_free_stack_b = (uint16_t)(4096 - (int)((s_anim_ms / 500) % 7) * 16);
    s_dg.p4_tasks        = 23;
    s_dg.p4_uptime_s     = (uint32_t)(s_anim_ms / 1000);
    s_dg.valid = 0xFFFF;   // everything "valid" in sim so rows show numbers
}

static bool dvalid(uint16_t bit) { return (s_dg.valid & bit) != 0; }

// --- formatting helpers ----------------------------------------------------
static void fmt_temp(char *b, int n, int16_t c10, bool valid)
{
    if (!valid || c10 == (int16_t)DDP_DIAG_TEMP_NA) { snprintf(b, n, "--"); return; }
    int t = c10, neg = (t < 0); if (neg) t = -t;
    snprintf(b, n, "%s%d.%d C", neg ? "-" : "", t / 10, t % 10);
}
static void fmt_volts_mv(char *b, int n, uint16_t mv, bool valid)
{
    if (!valid) { snprintf(b, n, "--"); return; }
    snprintf(b, n, "%u.%02u V", mv / 1000u, (mv % 1000u) / 10u);
}
static void fmt_si(char *b, int n, float v, const char *unit, bool valid)
{
    // Auto-range a base-SI value into n/u/m/(base)/k with 3 sig digits.
    if (!valid) { snprintf(b, n, "--"); return; }
    float a = fabsf(v);
    const char *p = ""; float s = v;
    if      (a < 1e-6f)  { p = "n"; s = v * 1e9f; }
    else if (a < 1e-3f)  { p = "u"; s = v * 1e6f; }
    else if (a < 1.0f)   { p = "m"; s = v * 1e3f; }
    else if (a < 1e3f)   { p = "";  s = v; }
    else                 { p = "k"; s = v / 1e3f; }
    snprintf(b, n, "%.3g %s%s", (double)s, p, unit);
}
static void fmt_uptime(char *b, int n, uint32_t sec)
{
    if (sec >= 3600) snprintf(b, n, "%uh%02um", (unsigned)(sec / 3600), (unsigned)((sec % 3600) / 60));
    else if (sec >= 60) snprintf(b, n, "%um%02us", (unsigned)(sec / 60), (unsigned)(sec % 60));
    else snprintf(b, n, "%us", (unsigned)sec);
}

// Read the C6 internal die temperature (returns false if unavailable).
static bool c6_die_temp(float *out)
{
    if (!s_c6_tsens) return false;
    return temperature_sensor_get_celsius(s_c6_tsens, out) == ESP_OK;
}

// --- Temperatures ----------------------------------------------------------
static void d_t_board0(char *b, int n) { fmt_temp(b, n, s_dg.t_board0_c10, dvalid(DDP_DIAG_V_BOARD0)); }
static void d_t_board1(char *b, int n) { fmt_temp(b, n, s_dg.t_board1_c10, dvalid(DDP_DIAG_V_BOARD1)); }
static void d_t_adaq0(char *b, int n)  { fmt_temp(b, n, s_dg.t_adaq0_c10,  dvalid(DDP_DIAG_V_ADAQ0)); }
static void d_t_adaq1(char *b, int n)  { fmt_temp(b, n, s_dg.t_adaq1_c10,  dvalid(DDP_DIAG_V_ADAQ1)); }
static void d_t_adaq2(char *b, int n)  { fmt_temp(b, n, s_dg.t_adaq2_c10,  dvalid(DDP_DIAG_V_ADAQ2)); }
static void d_t_p4(char *b, int n)     { fmt_temp(b, n, s_dg.t_p4_c10,     dvalid(DDP_DIAG_V_P4TEMP)); }
static void d_t_s3(char *b, int n)     { fmt_temp(b, n, s_dg.t_s3_c10,     dvalid(DDP_DIAG_V_S3)); }
static void d_t_c6(char *b, int n)     { float t; if (c6_die_temp(&t)) snprintf(b, n, "%d.%d C", (int)t, (int)(fabsf(t) * 10) % 10); else snprintf(b, n, "--"); }

static float s_t_board0(void) { return s_dg.t_board0_c10 / 10.0f; }
static float s_t_board1(void) { return s_dg.t_board1_c10 / 10.0f; }
static float s_t_adaq0(void)  { return s_dg.t_adaq0_c10 / 10.0f; }
static float s_t_adaq1(void)  { return s_dg.t_adaq1_c10 / 10.0f; }
static float s_t_adaq2(void)  { return s_dg.t_adaq2_c10 / 10.0f; }
static float s_t_p4(void)     { return s_dg.t_p4_c10 / 10.0f; }
static float s_t_s3(void)     { return s_dg.t_s3_c10 / 10.0f; }
static float s_t_c6(void)     { float t; return c6_die_temp(&t) ? t : 0.0f; }

// --- Power -----------------------------------------------------------------
static void d_i(char *b, int n)    { fmt_si(b, n, s_dg.i_ua / 1e6f, "A", dvalid(DDP_DIAG_V_IVP)); }
static void d_v(char *b, int n)    { fmt_si(b, n, s_dg.v_uv / 1e6f, "V", dvalid(DDP_DIAG_V_IVP)); }
static void d_p(char *b, int n)    { fmt_si(b, n, s_dg.p_uw / 1e6f, "W", dvalid(DDP_DIAG_V_IVP)); }
static void d_iin(char *b, int n)  { if (dvalid(DDP_DIAG_V_SMU)) snprintf(b, n, "%d mA", s_dg.smu_iin_ma); else snprintf(b, n, "--"); }
static void d_iout(char *b, int n) { if (dvalid(DDP_DIAG_V_SMU)) snprintf(b, n, "%d mA", s_dg.smu_iout_ma); else snprintf(b, n, "--"); }

static float s_i(void)    { return s_dg.i_ua / 1e6f; }
static float s_v(void)    { return s_dg.v_uv / 1e6f; }
static float s_p(void)    { return s_dg.p_uw / 1e6f; }
static float s_iin(void)  { return s_dg.smu_iin_ma; }
static float s_iout(void) { return s_dg.smu_iout_ma; }

// --- Power rails ------------------------------------------------------------
static void d_pd_v(char *b, int n)   { fmt_volts_mv(b, n, s_dg.pd_mv, dvalid(DDP_DIAG_V_S3PD)); }
static void d_pd_i(char *b, int n)   { if (dvalid(DDP_DIAG_V_S3PD)) snprintf(b, n, "%u.%02u A", s_dg.pd_ma / 1000u, (s_dg.pd_ma % 1000u) / 10u); else snprintf(b, n, "--"); }
static void d_vdut(char *b, int n)   { fmt_volts_mv(b, n, s_dg.vdut_mv, dvalid(DDP_DIAG_V_VDUT)); }
static void d_vadj1(char *b, int n)  { fmt_volts_mv(b, n, s_dg.vadj1_mv, dvalid(DDP_DIAG_V_S3)); }
static void d_vadj2(char *b, int n)  { fmt_volts_mv(b, n, s_dg.vadj2_mv, dvalid(DDP_DIAG_V_S3)); }
static void d_vlogic(char *b, int n) { fmt_volts_mv(b, n, s_dg.vlogic_mv, dvalid(DDP_DIAG_V_S3)); }

static float s_pd_v(void)   { return s_dg.pd_mv / 1000.0f; }
static float s_pd_i(void)   { return s_dg.pd_ma / 1000.0f; }
static float s_vdut(void)   { return s_dg.vdut_mv / 1000.0f; }
static float s_vadj1(void)  { return s_dg.vadj1_mv / 1000.0f; }
static float s_vadj2(void)  { return s_dg.vadj2_mv / 1000.0f; }
static float s_vlogic(void) { return s_dg.vlogic_mv / 1000.0f; }

// --- ESP32-P4 stats ---------------------------------------------------------
static void d_p4_mem(char *b, int n)    { if (s_dg_live) snprintf(b, n, "%u KB", (unsigned)s_dg.p4_free_mem_kb); else snprintf(b, n, "--"); }
static void d_p4_stack(char *b, int n)  { if (s_dg_live) snprintf(b, n, "%u B", (unsigned)s_dg.p4_free_stack_b); else snprintf(b, n, "--"); }
static void d_p4_tasks(char *b, int n)  { if (s_dg_live) snprintf(b, n, "%u", (unsigned)s_dg.p4_tasks); else snprintf(b, n, "--"); }
static void d_p4_uptime(char *b, int n) { if (s_dg_live) fmt_uptime(b, n, s_dg.p4_uptime_s); else snprintf(b, n, "--"); }

static float s_p4_mem(void)   { return s_dg.p4_free_mem_kb; }
static float s_p4_stack(void) { return s_dg.p4_free_stack_b; }
static float s_p4_tasks(void) { return s_dg.p4_tasks; }

// --- ESP32-C6 self-stats (read locally) ------------------------------------
static void d_c6_mem(char *b, int n)    { snprintf(b, n, "%u KB", (unsigned)(esp_get_free_heap_size() / 1024u)); }
static void d_c6_uptime(char *b, int n) { fmt_uptime(b, n, (uint32_t)(esp_timer_get_time() / 1000000)); }

static float s_c6_mem(void)  { return esp_get_free_heap_size() / 1024.0f; }


// ===========================================================================
// Menu tree
// ===========================================================================
static const menu_t m_hat, m_screen, m_mainboard, m_wifi, m_diag;
static const menu_t m_diag_temp, m_diag_power, m_diag_rails, m_diag_p4, m_diag_c6;

static const menu_item_t root_items[] = {
    { .label = "HAT Settings",        .type = IT_SUBMENU, .sub = &m_hat },
    { .label = "Screen Settings",     .type = IT_SUBMENU, .sub = &m_screen },
    { .label = "Main Board Settings", .type = IT_SUBMENU, .sub = &m_mainboard },
    { .label = "WiFi Settings",       .type = IT_SUBMENU, .sub = &m_wifi },
    { .label = "Diagnostics",         .type = IT_SUBMENU, .sub = &m_diag },
};
static const menu_t m_root = { "Settings", root_items, 5 };

static const menu_item_t hat_items[] = {
    { .label = "Autoranging",      .type = IT_TOGGLE, .value = val_autorange, .ok = ok_autorange },
    { .label = "Range Setting",    .type = IT_CYCLE,  .value = val_range, .ok = ok_range,
      .visible = vis_manual, .warn = warn_manual },
    { .label = "Sample Rate",      .type = IT_CYCLE,  .value = val_srate, .ok = ok_srate },
    { .label = "DUT Current Limit",.type = IT_BARGRAPH, .value = val_current,
      .bar_ref = &g_settings.dut_current_ma, .bar_min = 100, .bar_max = 2500,
      .bar_step = 100, .bar_fmt = fmt_current },
    { .label = "DUT Voltage",      .type = IT_BARGRAPH, .value = val_voltage,
      .bar_ref = &g_settings.dut_voltage_mv, .bar_min = 1800, .bar_max = 20000,
      .bar_step = 100, .bar_fmt = fmt_voltage },
    { .label = "FFT",              .type = IT_TOGGLE, .value = val_fft,    .ok = ok_fft },
    { .label = "FFT Length",       .type = IT_CYCLE,  .value = val_fftlen, .ok = ok_fftlen,
      .visible = vis_fft },
    { .label = "FFT Window",       .type = IT_CYCLE,  .value = val_fftwin, .ok = ok_fftwin,
      .visible = vis_fft },
    { .label = "FFT Source",       .type = IT_CYCLE,  .value = val_fftsrc, .ok = ok_fftsrc,
      .visible = vis_fft },
};
static const menu_t m_hat = { "HAT Settings", hat_items, 9 };

static const menu_item_t screen_items[] = {
    { .label = "Brightness", .type = IT_BARGRAPH, .value = val_bright,
      .bar_ref = &g_settings.brightness_pct, .bar_min = 10, .bar_max = 100,
      .bar_step = 10, .bar_fmt = fmt_pct, .bar_change = apply_brightness },
    { .label = "Dark Mode",  .type = IT_TOGGLE, .value = val_dark, .ok = ok_dark },
    { .label = "LED Mode",       .type = IT_CYCLE, .value = val_npxmode, .ok = ok_npxmode },
    { .label = "LED Color",      .type = IT_CYCLE, .value = val_npxcol,  .ok = ok_npxcol },
    { .label = "LED Brightness", .type = IT_BARGRAPH, .value = val_npxbright,
      .bar_ref = &g_settings.npx_brightness, .bar_min = 0, .bar_max = 100,
      .bar_step = 5, .bar_fmt = fmt_pct },
};
static const menu_t m_screen = { "Screen Settings", screen_items, 5 };

static const menu_t m_mainboard = { "Main Board Settings", NULL, 0 };

static const menu_item_t wifi_items[] = {
    { .label = "WiFi",   .type = IT_TOGGLE, .value = val_wifi,     .ok = ok_wifi },
    { .label = "Mode",   .type = IT_CYCLE,  .value = val_wifimode, .ok = ok_wifimode, .visible = vis_wifi },
    { .label = "SSID",   .type = IT_INFO,   .value = val_ssid,       .visible = vis_wifi },
    { .label = "Status", .type = IT_INFO,   .value = val_wifistatus, .visible = vis_wifi },
};
static const menu_t m_wifi = { "WiFi Settings", wifi_items, 4 };

static const menu_item_t diag_temp_items[] = {
    { .label = "Board U2",  .type = IT_INFO, .value = d_t_board0, .sample = s_t_board0 },
    { .label = "Board U28", .type = IT_INFO, .value = d_t_board1, .sample = s_t_board1 },
    { .label = "ADAQ U1",   .type = IT_INFO, .value = d_t_adaq0,  .sample = s_t_adaq0 },
    { .label = "ADAQ U22",  .type = IT_INFO, .value = d_t_adaq1,  .sample = s_t_adaq1 },
    { .label = "ADAQ U23",  .type = IT_INFO, .value = d_t_adaq2,  .sample = s_t_adaq2 },
    { .label = "ESP32-P4",  .type = IT_INFO, .value = d_t_p4,     .sample = s_t_p4 },
    { .label = "ESP32-C6",  .type = IT_INFO, .value = d_t_c6,     .sample = s_t_c6 },
    { .label = "S3 Die",    .type = IT_INFO, .value = d_t_s3,     .sample = s_t_s3 },
};
static const menu_t m_diag_temp = { "Temperatures", diag_temp_items, 8 };

static const menu_item_t diag_power_items[] = {
    { .label = "Current",   .type = IT_INFO, .value = d_i,    .sample = s_i },
    { .label = "Voltage",   .type = IT_INFO, .value = d_v,    .sample = s_v },
    { .label = "Power",     .type = IT_INFO, .value = d_p,    .sample = s_p },
    { .label = "SMU In I",  .type = IT_INFO, .value = d_iin,  .sample = s_iin },
    { .label = "SMU Out I", .type = IT_INFO, .value = d_iout, .sample = s_iout },
};
static const menu_t m_diag_power = { "Power", diag_power_items, 5 };

static const menu_item_t diag_rails_items[] = {
    { .label = "USB-PD V", .type = IT_INFO, .value = d_pd_v,   .sample = s_pd_v },
    { .label = "USB-PD I", .type = IT_INFO, .value = d_pd_i,   .sample = s_pd_i },
    { .label = "V_DUT",    .type = IT_INFO, .value = d_vdut,   .sample = s_vdut },
    { .label = "VADJ1",    .type = IT_INFO, .value = d_vadj1,  .sample = s_vadj1 },
    { .label = "VADJ2",    .type = IT_INFO, .value = d_vadj2,  .sample = s_vadj2 },
    { .label = "VLOGIC",   .type = IT_INFO, .value = d_vlogic, .sample = s_vlogic },
};
static const menu_t m_diag_rails = { "Power Rails", diag_rails_items, 6 };

static const menu_item_t diag_p4_items[] = {
    { .label = "Free Mem",   .type = IT_INFO, .value = d_p4_mem,    .sample = s_p4_mem },
    { .label = "Free Stack", .type = IT_INFO, .value = d_p4_stack,  .sample = s_p4_stack },
    { .label = "Tasks",      .type = IT_INFO, .value = d_p4_tasks,  .sample = s_p4_tasks },
    { .label = "Uptime",     .type = IT_INFO, .value = d_p4_uptime },
};
static const menu_t m_diag_p4 = { "ESP32-P4", diag_p4_items, 4 };

static const menu_item_t diag_c6_items[] = {
    { .label = "Free Mem", .type = IT_INFO, .value = d_c6_mem,    .sample = s_c6_mem },
    { .label = "Temp",     .type = IT_INFO, .value = d_t_c6,      .sample = s_t_c6 },
    { .label = "Uptime",   .type = IT_INFO, .value = d_c6_uptime },
};
static const menu_t m_diag_c6 = { "ESP32-C6", diag_c6_items, 3 };

static const menu_item_t diag_items[] = {
    { .label = "Temperatures", .type = IT_SUBMENU, .sub = &m_diag_temp },
    { .label = "Power",        .type = IT_SUBMENU, .sub = &m_diag_power },
    { .label = "Power Rails",  .type = IT_SUBMENU, .sub = &m_diag_rails },
    { .label = "ESP32-P4",     .type = IT_SUBMENU, .sub = &m_diag_p4 },
    { .label = "ESP32-C6",     .type = IT_SUBMENU, .sub = &m_diag_c6 },
};
static const menu_t m_diag = { "Diagnostics", diag_items, 5 };

// ===========================================================================
// Navigation state
// ===========================================================================
#define NAV_MAX 6
#define TIMEOUT_MS 30000

typedef struct { const menu_t *menu; int sel; } nav_frame_t;

static nav_frame_t s_stack[NAV_MAX];
static int   s_depth = 0;
static bool  s_active = false;
static uint32_t s_last_input = 0;
static uint32_t s_last_paint = 0;

// Carousel spring animation.
static float s_pos = 0.0f;     // fractional centered index
static float s_vel = 0.0f;

// Bargraph editor.
static bool  s_in_editor = false;
static const menu_item_t *s_editor = NULL;
static float s_bar_disp = 0.0f;

// Live-preview detail screen (scrolling sparkline of one diagnostic sampler).
#define HIST_N 130
static bool  s_in_detail = false;
static const menu_item_t *s_detail = NULL;
static float s_hist[HIST_N];
static int   s_hist_count = 0;
static uint32_t s_hist_last = 0;

static const menu_t *cur_menu(void) { return s_stack[s_depth].menu; }
static int          *cur_sel(void)  { return &s_stack[s_depth].sel; }

// Visible-item handling (some items hide themselves, e.g. Range Setting).
static int vis_count(const menu_t *m)
{
    int c = 0;
    for (int i = 0; i < m->count; i++)
        if (!m->items[i].visible || m->items[i].visible()) c++;
    return c;
}
static const menu_item_t *vis_item(const menu_t *m, int vis_idx)
{
    int c = 0;
    for (int i = 0; i < m->count; i++) {
        if (m->items[i].visible && !m->items[i].visible()) continue;
        if (c == vis_idx) return &m->items[i];
        c++;
    }
    return NULL;
}

void menu_init(void)
{
    s_active = false; s_depth = 0;
#ifdef TARGET_C6
    /* C3 tsens HAL asserts on any range whose upper bound exceeds 80°C;
     * guard so the test board (TARGET_C3) doesn't panic here. */
    if (!s_c6_tsens) {
        temperature_sensor_config_t tcfg = TEMPERATURE_SENSOR_CONFIG_DEFAULT(-10, 110);
        if (temperature_sensor_install(&tcfg, &s_c6_tsens) == ESP_OK)
            temperature_sensor_enable(s_c6_tsens);
        else
            s_c6_tsens = NULL;
    }
#endif
}

bool menu_active(void) { return s_active; }

void menu_open(uint32_t now_ms)
{
    s_active = true;
    s_in_editor = false;
    s_depth = 0;
    s_stack[0].menu = &m_root;
    s_stack[0].sel = 0;
    s_pos = 0.0f;
    s_vel = 0.0f;
    s_last_input = now_ms;
    s_last_paint = 0;
}

static void clamp_sel(void)
{
    int n = vis_count(cur_menu());
    int *sel = cur_sel();
    if (n <= 0) { *sel = 0; return; }
    if (*sel < 0) *sel = 0;
    if (*sel >= n) *sel = n - 1;
}

// ===========================================================================
// Input
// ===========================================================================
static void enter_editor(const menu_item_t *it)
{
    s_in_editor = true;
    s_editor = it;
    s_bar_disp = (float)(*it->bar_ref);
}

static void enter_detail(const menu_item_t *it)
{
    s_in_detail = true;
    s_detail = it;
    s_hist_count = 0;
    s_hist_last = 0;
    if (it->sample) { s_hist[0] = it->sample(); s_hist_count = 1; }
}

static void handle_detail_event(uint32_t ev)
{
    if (ev & (BTN_EV_OK | BTN_EV_BACK)) { s_in_detail = false; s_detail = NULL; }
}

static void handle_menu_event(uint32_t ev)
{
    const menu_t *m = cur_menu();
    int n = vis_count(m);

    if (ev & BTN_EV_UP) {
        if (*cur_sel() > 0) (*cur_sel())--;
    }
    if (ev & BTN_EV_DOWN) {
        if (*cur_sel() < n - 1) (*cur_sel())++;
    }
    if (ev & BTN_EV_BACK) {
        if (s_depth > 0) { s_depth--; s_pos = (float)*cur_sel(); s_vel = 0; }
        else s_active = false;   // back out of root -> close
    }
    if (ev & BTN_EV_OK) {
        const menu_item_t *it = vis_item(m, *cur_sel());
        if (!it) return;
        switch (it->type) {
        case IT_SUBMENU:
            if (it->sub && s_depth < NAV_MAX - 1) {
                s_depth++;
                s_stack[s_depth].menu = it->sub;
                s_stack[s_depth].sel = 0;
                s_pos = 0; s_vel = 0;
            }
            break;
        case IT_TOGGLE:
        case IT_CYCLE:
            if (it->ok) it->ok();
            clamp_sel();
            break;
        case IT_BARGRAPH:
            enter_editor(it);
            break;
        case IT_INFO:
            if (it->sample) enter_detail(it);   // open live-preview sparkline
            break;
        default:
            break;
        }
    }
}

static void handle_editor_event(uint32_t ev)
{
    const menu_item_t *it = s_editor;
    if (ev & BTN_EV_UP) {
        *it->bar_ref += it->bar_step;
        if (*it->bar_ref > it->bar_max) *it->bar_ref = it->bar_max;
        if (it->bar_change) it->bar_change(*it->bar_ref);
    }
    if (ev & BTN_EV_DOWN) {
        *it->bar_ref -= it->bar_step;
        if (*it->bar_ref < it->bar_min) *it->bar_ref = it->bar_min;
        if (it->bar_change) it->bar_change(*it->bar_ref);
    }
    if (ev & (BTN_EV_OK | BTN_EV_BACK)) {
        s_in_editor = false;          // commit & leave editor
        s_editor = NULL;
        settings_commit();            // persist + push to P4 once, on exit
    }
}

menu_status_t menu_update(uint32_t events, uint32_t now_ms, bool *need_render)
{
    s_anim_ms = now_ms;
    diag_refresh();
    bool render = false;

    if (events) {
        s_last_input = now_ms;
        if (s_in_detail)      handle_detail_event(events);
        else if (s_in_editor) handle_editor_event(events);
        else                  handle_menu_event(events);
        render = true;
        if (!s_active) return MENU_CLOSED;
    }

    // Inactivity timeout.
    if (now_ms - s_last_input >= TIMEOUT_MS) {
        s_active = false;
        return MENU_CLOSED;
    }

    // Carousel spring toward the selected row (overshoot = Pebble "gravity").
    // Snappy: high stiffness + lighter damping = quick move with a small bump
    // that settles fast.
    if (s_in_detail) {
        // Sample the live value into the sparkline ring at ~4 Hz.
        if (now_ms - s_hist_last >= 250) {
            s_hist_last = now_ms;
            if (s_detail && s_detail->sample) {
                float v = s_detail->sample();
                if (s_hist_count < HIST_N) {
                    s_hist[s_hist_count++] = v;
                } else {
                    memmove(s_hist, s_hist + 1, (HIST_N - 1) * sizeof(float));
                    s_hist[HIST_N - 1] = v;
                }
                render = true;
            }
        }
    } else if (!s_in_editor) {
        float target = (float)*cur_sel();
        float diff = target - s_pos;
        s_vel = s_vel * 0.58f + diff * 0.58f;
        s_pos += s_vel;
        if (fabsf(diff) < 0.004f && fabsf(s_vel) < 0.004f) {
            s_pos = target; s_vel = 0.0f;
        } else {
            render = true;
        }
    } else {
        // Animate the bargraph fill toward the target value.
        float tgt = (float)(*s_editor->bar_ref);
        float d = tgt - s_bar_disp;
        if (fabsf(d) > 0.5f) { s_bar_disp += d * 0.35f; render = true; }
        else s_bar_disp = tgt;
    }

    // Low-rate heartbeat so live diagnostics / timeout stay fresh (cheap text).
    if (now_ms - s_last_paint >= 200) render = true;

    *need_render = render;
    return MENU_RUNNING;
}

// ===========================================================================
// Rendering
// ===========================================================================
#define TITLE_H   16
#define ROW_H     20

static void draw_warning(int x, int y, uint16_t color)
{
    // Small triangle with an exclamation mark (8x7).
    gfx_thick_line(x, y + 7, x + 8, y + 7, 1.4f, color);
    gfx_thick_line(x, y + 7, x + 4, y, 1.4f, color);
    gfx_thick_line(x + 8, y + 7, x + 4, y, 1.4f, color);
    gfx_fill_rect(x + 3, y + 3, 2, 2, color);   // dot
    gfx_fill_rect(x + 3, y + 5, 2, 1, color);   // stem tip
}

static void draw_chevron(int x, int y, uint16_t color)
{
    gfx_thick_line(x, y, x + 4, y + 4, 1.4f, color);
    gfx_thick_line(x, y + 8, x + 4, y + 4, 1.4f, color);
}

static void render_menu(void)
{
    const menu_t *m = cur_menu();
    int n = vis_count(m);

    gfx_clear(g_theme.bg);

    int list_top = TITLE_H + 1;
    int center_y = list_top + (DISP_HEIGHT - list_top) / 2;

    // Selection box fixed at the vertical center.
    int box_y = center_y - ROW_H / 2;
    gfx_round_rect(3, box_y, DISP_WIDTH - 6, ROW_H, 5, g_theme.sel);

    if (n == 0) {
        gfx_text((DISP_WIDTH - gfx_text_w("(empty)", 1)) / 2, center_y - 3,
                 "(empty)", 1, g_theme.sel_text);
    } else {
        // Draw the visible rows around the (fractional) centered position.
        for (int i = 0; i < n; i++) {
            float ry = center_y + ((float)i - s_pos) * ROW_H;
            if (ry < list_top - ROW_H || ry > DISP_HEIGHT + ROW_H) continue;
            int row_cy = (int)(ry + 0.5f);
            int ty = row_cy - 3;

            bool centered = fabsf((float)i - s_pos) < 0.5f;
            uint16_t lc = centered ? g_theme.sel_text : g_theme.text;
            uint16_t vc = centered ? g_theme.sel_text : g_theme.dim;

            const menu_item_t *it = vis_item(m, i);
            if (!it) continue;

            gfx_text(8, ty, it->label, 1, lc);

            int rx = DISP_WIDTH - 10;
            if (it->type == IT_SUBMENU) {
                draw_chevron(rx - 4, row_cy - 4, lc);
            } else if (it->value) {
                char buf[20];
                it->value(buf, sizeof(buf));
                int vw = gfx_text_w(buf, 1);
                gfx_text(rx - vw, ty, buf, 1, vc);
                if (it->warn && it->warn())
                    draw_warning(rx - vw - 14, row_cy - 4, g_theme.amber);
            }
        }
    }

    // Title strip LAST so rows that scroll up are masked behind it (otherwise
    // they bleed over the header text).
    gfx_fill_rect(0, 0, DISP_WIDTH, TITLE_H, g_theme.header);
    gfx_hline(0, TITLE_H, DISP_WIDTH, g_theme.border);
    gfx_text(6, 5, m->title, 1, g_theme.dim);

    if (n > 0) {
        char pos[24];
        snprintf(pos, sizeof(pos), "%d/%d", *cur_sel() + 1, n);
        gfx_text(DISP_WIDTH - gfx_text_w(pos, 1) - 6, 5, pos, 1, g_theme.muted);
    }
}

static void render_editor(void)
{
    const menu_item_t *it = s_editor;
    gfx_clear(g_theme.bg);

    // Title.
    gfx_fill_rect(0, 0, DISP_WIDTH, TITLE_H, g_theme.header);
    gfx_hline(0, TITLE_H, DISP_WIDTH, g_theme.border);
    gfx_text(6, 5, it->label, 1, g_theme.dim);

    // Big current value, centered.
    char buf[20];
    it->bar_fmt(buf, sizeof(buf), *it->bar_ref);
    gfx_text((DISP_WIDTH - gfx_text_w(buf, 2)) / 2, 24, buf, 2, g_theme.text);

    // Horizontal bar.
    int bx = 18, bw = DISP_WIDTH - 36, by = 52, bh = 10;
    gfx_round_rect_border(bx, by, bw, bh, 3, g_theme.border);
    float frac = (s_bar_disp - it->bar_min) / (float)(it->bar_max - it->bar_min);
    if (frac < 0) frac = 0;
    if (frac > 1) frac = 1;
    int fillw = (int)(frac * (bw - 4));
    if (fillw > 0) gfx_round_rect(bx + 2, by + 2, fillw, bh - 4, 2, g_theme.sel);

    // Min/max hints.
    char mn[16], mx[16];
    it->bar_fmt(mn, sizeof(mn), it->bar_min);
    it->bar_fmt(mx, sizeof(mx), it->bar_max);
    gfx_text(bx, by + bh + 2, mn, 1, g_theme.muted);
    gfx_text(bx + bw - gfx_text_w(mx, 1), by + bh + 2, mx, 1, g_theme.muted);
}

static void render_detail(void)
{
    const menu_item_t *it = s_detail;
    gfx_clear(g_theme.bg);

    // Title strip.
    gfx_fill_rect(0, 0, DISP_WIDTH, TITLE_H, g_theme.header);
    gfx_hline(0, TITLE_H, DISP_WIDTH, g_theme.border);
    gfx_text(6, 5, it->label, 1, g_theme.dim);

    // Current value (formatted), top-left under the title.
    char buf[20];
    it->value(buf, sizeof(buf));
    gfx_text(6, 20, buf, 2, g_theme.text);

    // Sparkline plot area.
    int gx = 6, gw = DISP_WIDTH - 12;
    int gy = 40, gh = DISP_HEIGHT - gy - 3;
    gfx_round_rect_border(gx, gy, gw, gh, 3, g_theme.border);

    if (s_hist_count >= 2) {
        float mn = s_hist[0], mx = s_hist[0];
        for (int i = 1; i < s_hist_count; i++) {
            if (s_hist[i] < mn) mn = s_hist[i];
            if (s_hist[i] > mx) mx = s_hist[i];
        }
        float span = mx - mn;
        if (span < 1e-6f) span = 1.0f;   // flat line -> center it

        int ax = gx + 2, aw = gw - 4;
        int ay = gy + 2, ah = gh - 4;
        int px = 0, py = 0; bool have = false;
        for (int i = 0; i < s_hist_count; i++) {
            int x = ax + (s_hist_count > 1 ? i * (aw - 1) / (s_hist_count - 1) : 0);
            int y = ay + ah - 1 - (int)((s_hist[i] - mn) / span * (ah - 1));
            if (have) gfx_thick_line((float)px, (float)py, (float)x, (float)y, 1.2f, g_theme.sel);
            px = x; py = y; have = true;
        }
    } else {
        gfx_text(gx + 6, gy + gh / 2 - 3, "collecting...", 1, g_theme.muted);
    }
}

void menu_render(uint32_t now_ms)
{
    s_last_paint = now_ms;
    if (s_in_detail)      render_detail();
    else if (s_in_editor) render_editor();
    else                  render_menu();
}
