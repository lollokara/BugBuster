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
    bool (*value_alert)(void);               // NULL => normal; true => value in red
    void (*ok_arg)(int);                     // radio-list: OK sets .arg
    int  arg;                                // value for ok_arg / sel_ref match
    const int *sel_ref;                      // radio-list: mark when *sel_ref==arg

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
static void pick_srate(int v)         { g_settings.sample_rate_idx = v; settings_commit(); }

// Digital filter + decimation (P4 ADAQ FINE+COARSE) — labels from the registry.
static void val_filter(char *b, int n) { snprintf(b, n, "%s", daq_config_schema(DAQ_K_FILTER)->options[g_settings.filter_idx % DAQ_FILT_COUNT]); }
static void pick_filter(int v)         { g_settings.filter_idx = v; settings_commit(); }
static void val_decim(char *b, int n)  { snprintf(b, n, "%s", daq_config_schema(DAQ_K_DECIMATION)->options[g_settings.decim_idx % DAQ_DEC_COUNT]); }
static void pick_decim(int v)          { g_settings.decim_idx = v; settings_commit(); }
static void val_reject(char *b, int n) { v_onoff(b, n, g_settings.reject_5060); }
static void ok_reject(void)            { g_settings.reject_5060 = !g_settings.reject_5060; settings_commit(); }

// Super Resolution: the P4 pins the ADAQs to Sinc3 at maximum decimation and
// low-pass/decimates to DAQ_SR_CURRENT_SPS / DAQ_SR_VOLTAGE_SPS, so while it is
// on the Sample Rate / Filter / Decimation rows would be lying — hide them.
static void val_sr(char *b, int n)     { v_onoff(b, n, g_settings.sr_mode); }
static void ok_sr(void)                { g_settings.sr_mode = !g_settings.sr_mode; settings_commit(); }
static bool vis_not_sr(void)           { return !g_settings.sr_mode; }
static bool vis_sr(void)               { return g_settings.sr_mode; }
static void val_sr_rate(char *b, int n)
{
    snprintf(b, n, "%u/%u sps", (unsigned)DAQ_SR_CURRENT_SPS,
             (unsigned)DAQ_SR_VOLTAGE_SPS);
}
// 50/60 Hz rejection is a Sinc3 option, and SR always runs Sinc3.
static bool vis_sinc3(void)            { return g_settings.sr_mode || g_settings.filter_idx == DAQ_FILT_SINC3; }

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
// Main Board Settings — S3 rails + e-fuses over the DDP settings tunnel.
// The C6 pulls the power snapshot only while the Power menu is open; writes go
// C6 -> P4 -> S3. The P4 defers the tunnel while streaming to the PC, so this
// is inert during acquisition. Rapid rail edits coalesce into the P4's one-deep
// pending request and apply at the next ~1 s S3 poll.
// ===========================================================================
static ddp_mb_power_t s_mbp;            // last power snapshot from the S3
static bool           s_mbp_valid = false;
static int  s_vlogic_mv = 3300;         // IT_BARGRAPH refs (seeded from snapshot)
static int  s_vadj1_mv  = 5000;
static int  s_vadj2_mv  = 5000;
static uint32_t s_mb_last_req = 0;

// Converging power-write retry: keep re-sending the desired e-fuse / rail state
// until the reported snapshot matches (bounded window), so a dropped DDP frame
// can't silently lose a toggle.
static uint8_t  s_want_efuse_mask = 0, s_want_efuse_val = 0;  // bit i per e-fuse
static uint8_t  s_want_rail_mask  = 0, s_want_rail_val  = 0;  // bit r per rail
static uint32_t s_want_last = 0;      // last (re)send time
static uint32_t s_want_until = 0;     // give-up deadline (handles e-fuse trip)

static void fmt_mv_volts(char *b, int n, int mv)
{
    if (mv < 0) mv = 0;
    snprintf(b, n, "%d.%02d V", mv / 1000, (mv % 1000) / 10);
}
static void val_vlogic(char *b, int n) { fmt_mv_volts(b, n, s_vlogic_mv); }
static void val_vadj1(char *b, int n)  { fmt_mv_volts(b, n, s_vadj1_mv); }
static void val_vadj2(char *b, int n)  { fmt_mv_volts(b, n, s_vadj2_mv); }

static void send_set_rail(uint8_t rail, int mv)
{
    if (mv < 0) mv = 0;
    uint8_t args[3] = { rail, (uint8_t)(mv & 0xFF), (uint8_t)((mv >> 8) & 0xFF) };
    ddp_send_mb_request(DDP_MB_SET_RAIL, args, sizeof(args));
}
static void ch_vlogic(int mv) { send_set_rail(DDP_MB_RAIL_VLOGIC, mv); }
static void ch_vadj1(int mv)  { send_set_rail(DDP_MB_RAIL_VADJ1,  mv); }
static void ch_vadj2(int mv)  { send_set_rail(DDP_MB_RAIL_VADJ2,  mv); }

static void efuse_toggle(int idx)
{
    bool want = !(s_mbp_valid && (s_mbp.efuse_en & (1u << idx)));
    s_want_efuse_mask |= (uint8_t)(1u << idx);
    if (want) s_want_efuse_val |=  (uint8_t)(1u << idx);
    else      s_want_efuse_val &= (uint8_t)~(1u << idx);
    s_want_last  = 0;
    s_want_until = s_anim_ms + 2500;
    uint8_t args[2] = { (uint8_t)idx, (uint8_t)(want ? 1 : 0) };
    ddp_send_mb_request(DDP_MB_SET_EFUSE, args, sizeof(args));
}
static void ok_efuse0(void) { efuse_toggle(0); }
static void ok_efuse1(void) { efuse_toggle(1); }
static void ok_efuse2(void) { efuse_toggle(2); }
static void ok_efuse3(void) { efuse_toggle(3); }

static void val_efuse(char *b, int n, int idx)
{
    if (!s_mbp_valid) { snprintf(b, n, "--"); return; }
    bool on  = s_mbp.efuse_en  & (1u << idx);
    bool flt = s_mbp.efuse_flt & (1u << idx);
    snprintf(b, n, "%s", flt ? "FAULT" : (on ? "ON" : "OFF"));
}
static void val_efuse0(char *b, int n) { val_efuse(b, n, 0); }
static void val_efuse1(char *b, int n) { val_efuse(b, n, 1); }
static void val_efuse2(char *b, int n) { val_efuse(b, n, 2); }
static void val_efuse3(char *b, int n) { val_efuse(b, n, 3); }
static bool warn_efuse0(void) { return s_mbp_valid && (s_mbp.efuse_flt & 1u); }
static bool warn_efuse1(void) { return s_mbp_valid && (s_mbp.efuse_flt & 2u); }
static bool warn_efuse2(void) { return s_mbp_valid && (s_mbp.efuse_flt & 4u); }
static bool warn_efuse3(void) { return s_mbp_valid && (s_mbp.efuse_flt & 8u); }

// --- Rail enables (VLOGIC/level-shifter OE + VADJ1/VADJ2) -------------------
static void rail_en_toggle(uint8_t rail)
{
    uint8_t bit = (uint8_t)(1u << rail);
    bool want = !(s_mbp_valid && (s_mbp.rail_en & bit));
    s_want_rail_mask |= bit;
    if (want) s_want_rail_val |=  bit;
    else      s_want_rail_val &= (uint8_t)~bit;
    s_want_last  = 0;
    s_want_until = s_anim_ms + 2500;
    uint8_t args[2] = { rail, (uint8_t)(want ? 1 : 0) };
    ddp_send_mb_request(DDP_MB_SET_RAIL_EN, args, sizeof(args));
}
static void ok_vlogic_en(void) { rail_en_toggle(0); }
static void ok_vadj1_en(void)  { rail_en_toggle(1); }
static void ok_vadj2_en(void)  { rail_en_toggle(2); }

static void val_vlogic_en(char *b, int n)
{
    if (!s_mbp_valid) { snprintf(b, n, "--"); return; }
    snprintf(b, n, "%s", (s_mbp.rail_en & DDP_MB_RAILEN_VLOGIC) ? "ON" : "OFF");
}
static void val_vadj_en(char *b, int n, uint8_t bit)
{
    if (!s_mbp_valid) { snprintf(b, n, "--"); return; }
    bool on = s_mbp.rail_en & bit;
    bool pg = s_mbp.rail_pg & bit;
    if (!on) snprintf(b, n, "OFF");
    else     snprintf(b, n, pg ? "ON" : "NO PG");
}
static void val_vadj1_en(char *b, int n) { val_vadj_en(b, n, DDP_MB_RAILEN_VADJ1); }
static void val_vadj2_en(char *b, int n) { val_vadj_en(b, n, DDP_MB_RAILEN_VADJ2); }
// Enabled but power-good is missing -> flag the status in red.
static bool alert_vadj1_en(void) { return s_mbp_valid && (s_mbp.rail_en & DDP_MB_RAILEN_VADJ1) && !(s_mbp.rail_pg & DDP_MB_RAILEN_VADJ1); }
static bool alert_vadj2_en(void) { return s_mbp_valid && (s_mbp.rail_en & DDP_MB_RAILEN_VADJ2) && !(s_mbp.rail_pg & DDP_MB_RAILEN_VADJ2); }


// ===========================================================================
// Menu tree
// ===========================================================================
static const menu_t m_hat, m_screen, m_mainboard, m_wifi, m_diag, m_cal;
static const menu_t m_diag_temp, m_diag_power, m_diag_rails, m_diag_p4, m_diag_c6;
static const menu_t m_srate, m_filter, m_decim;
static void scripts_open(void);   // opens the custom MicroPython Scripts screen
static void fw_open(void);        // opens the custom Firmware / update screen
static void cal_open_volt(void);  // DUT source calibration wizard entry points
static void cal_open_curr(void);
static void cal_open_base(void);

static const menu_item_t root_items[] = {
    { .label = "HAT Settings",        .type = IT_SUBMENU, .sub = &m_hat },
    { .label = "Screen Settings",     .type = IT_SUBMENU, .sub = &m_screen },
    { .label = "Main Board Settings", .type = IT_SUBMENU, .sub = &m_mainboard },
    { .label = "WiFi Settings",       .type = IT_SUBMENU, .sub = &m_wifi },
    { .label = "Diagnostics",         .type = IT_SUBMENU, .sub = &m_diag },
    { .label = "Firmware",            .type = IT_CYCLE,   .ok  = fw_open },
};
static const menu_t m_root = { "Settings", root_items, 6 };

// DUT supply. Also on the home screen (hold BACK), but that shortcut is
// undiscoverable, so mirror it here. The 9 V / 3 A USB-PD guard matches
// main.c's -- the P4 enforces it independently either way.
static void val_source(char *b, int n) { v_onoff(b, n, ui_source_on()); }
static bool alert_source(void)         { return ui_source_on(); }
static void ok_source(void)
{
    bool want_on = !ui_source_on();
    if (want_on && !(dvalid(DDP_DIAG_V_S3PD) && s_dg.pd_mv >= 9000 && s_dg.pd_ma >= 3000)) {
        ui_show_warning("Need USB-PD 9V/3A");
        return;
    }
    c6_config_send_source_enable(want_on);
}

// Stateless one-shot operations (daq_action_t). These reset accumulators on the
// P4; there is nothing to persist locally, so they bypass settings_commit().
static void ok_reset_energy(void) { ddp_send_config_action(DAQ_ACT_ENERGY_RESET); ui_show_warning("Energy reset"); }
static void ok_reset_charge(void) { ddp_send_config_action(DAQ_ACT_CHARGE_RESET); ui_show_warning("Charge reset"); }
static void ok_factory_reset(void)
{
    // Two-press confirm: a stray OK on a 3-button UI must not wipe calibration.
    static uint32_t s_armed_ms = 0;
    if (s_armed_ms && (s_anim_ms - s_armed_ms) < 4000) {
        s_armed_ms = 0;
        ddp_send_config_action(DAQ_ACT_FACTORY_RESET);
        ui_show_warning("Factory reset sent");
    } else {
        s_armed_ms = s_anim_ms ? s_anim_ms : 1;
        ui_show_warning("Press OK again");
    }
}

static const menu_item_t hat_items[] = {
    { .label = "DUT Supply",       .type = IT_TOGGLE, .value = val_source, .ok = ok_source,
      .value_alert = alert_source },
    { .label = "Autoranging",      .type = IT_TOGGLE, .value = val_autorange, .ok = ok_autorange },
    { .label = "Range Setting",    .type = IT_CYCLE,  .value = val_range, .ok = ok_range,
      .visible = vis_manual, .warn = warn_manual },
    { .label = "Super Resolution", .type = IT_TOGGLE, .value = val_sr, .ok = ok_sr },
    { .label = "  SR Output",      .type = IT_INFO,   .value = val_sr_rate,
      .visible = vis_sr },
    { .label = "Sample Rate",      .type = IT_SUBMENU, .sub = &m_srate,  .value = val_srate,
      .visible = vis_not_sr },
    { .label = "Filter",           .type = IT_SUBMENU, .sub = &m_filter, .value = val_filter,
      .visible = vis_not_sr },
    { .label = "Decimation",       .type = IT_SUBMENU, .sub = &m_decim,  .value = val_decim,
      .visible = vis_not_sr },
    { .label = "50/60Hz Reject",   .type = IT_TOGGLE, .value = val_reject, .ok = ok_reject,
      .visible = vis_sinc3 },
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
    { .label = "Calibration",      .type = IT_SUBMENU, .sub = &m_cal },
    { .label = "Reset Energy",     .type = IT_CYCLE,  .ok = ok_reset_energy },
    { .label = "Reset Charge",     .type = IT_CYCLE,  .ok = ok_reset_charge },
    { .label = "Factory Reset",    .type = IT_CYCLE,  .ok = ok_factory_reset },
};
static const menu_t m_hat = { "HAT Settings", hat_items, 19 };

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

static const menu_item_t power_items[] = {
    { .label = "VLOGIC", .type = IT_BARGRAPH, .value = val_vlogic,
      .bar_ref = &s_vlogic_mv, .bar_min = 1800, .bar_max = 5000,
      .bar_step = 100, .bar_fmt = fmt_mv_volts, .bar_change = ch_vlogic },
    { .label = "  LShift OE", .type = IT_TOGGLE, .value = val_vlogic_en, .ok = ok_vlogic_en },
    { .label = "VADJ1",  .type = IT_BARGRAPH, .value = val_vadj1,
      .bar_ref = &s_vadj1_mv, .bar_min = 2000, .bar_max = 20000,
      .bar_step = 250, .bar_fmt = fmt_mv_volts, .bar_change = ch_vadj1 },
    { .label = "  Enable", .type = IT_TOGGLE, .value = val_vadj1_en, .ok = ok_vadj1_en,
      .value_alert = alert_vadj1_en },
    { .label = "VADJ2",  .type = IT_BARGRAPH, .value = val_vadj2,
      .bar_ref = &s_vadj2_mv, .bar_min = 2000, .bar_max = 20000,
      .bar_step = 250, .bar_fmt = fmt_mv_volts, .bar_change = ch_vadj2 },
    { .label = "  Enable", .type = IT_TOGGLE, .value = val_vadj2_en, .ok = ok_vadj2_en,
      .value_alert = alert_vadj2_en },
    { .label = "E-Fuse 1", .type = IT_TOGGLE, .value = val_efuse0, .ok = ok_efuse0, .warn = warn_efuse0, .value_alert = warn_efuse0 },
    { .label = "E-Fuse 2", .type = IT_TOGGLE, .value = val_efuse1, .ok = ok_efuse1, .warn = warn_efuse1, .value_alert = warn_efuse1 },
    { .label = "E-Fuse 3", .type = IT_TOGGLE, .value = val_efuse2, .ok = ok_efuse2, .warn = warn_efuse2, .value_alert = warn_efuse2 },
    { .label = "E-Fuse 4", .type = IT_TOGGLE, .value = val_efuse3, .ok = ok_efuse3, .warn = warn_efuse3, .value_alert = warn_efuse3 },
};
static const menu_t m_power = { "Power", power_items, 10 };

static const menu_item_t cal_items[] = {
    { .label = "Baseline",    .type = IT_CYCLE, .ok = cal_open_base },
    { .label = "Voltage Out", .type = IT_CYCLE, .ok = cal_open_volt },
    { .label = "Current Out", .type = IT_CYCLE, .ok = cal_open_curr },
};
static const menu_t m_cal = { "Calibration", cal_items, 3 };

// Radio pick-lists for the acquisition enums (selected row shows a dot).
static const menu_item_t srate_items[] = {
    { .label = "10 ksps",  .type = IT_TOGGLE, .ok_arg = pick_srate, .arg = 0, .sel_ref = &g_settings.sample_rate_idx },
    { .label = "50 ksps",  .type = IT_TOGGLE, .ok_arg = pick_srate, .arg = 1, .sel_ref = &g_settings.sample_rate_idx },
    { .label = "100 ksps", .type = IT_TOGGLE, .ok_arg = pick_srate, .arg = 2, .sel_ref = &g_settings.sample_rate_idx },
    { .label = "250 ksps", .type = IT_TOGGLE, .ok_arg = pick_srate, .arg = 3, .sel_ref = &g_settings.sample_rate_idx },
    { .label = "1 Msps",   .type = IT_TOGGLE, .ok_arg = pick_srate, .arg = 4, .sel_ref = &g_settings.sample_rate_idx },
};
static const menu_t m_srate = { "Sample Rate", srate_items, 5 };

static const menu_item_t filter_items[] = {
    { .label = "Wideband", .type = IT_TOGGLE, .ok_arg = pick_filter, .arg = 0, .sel_ref = &g_settings.filter_idx },
    { .label = "Sinc5",    .type = IT_TOGGLE, .ok_arg = pick_filter, .arg = 1, .sel_ref = &g_settings.filter_idx },
    { .label = "Sinc3",    .type = IT_TOGGLE, .ok_arg = pick_filter, .arg = 2, .sel_ref = &g_settings.filter_idx },
};
static const menu_t m_filter = { "Filter", filter_items, 3 };

static const menu_item_t decim_items[] = {
    { .label = "x32",   .type = IT_TOGGLE, .ok_arg = pick_decim, .arg = 0, .sel_ref = &g_settings.decim_idx },
    { .label = "x64",   .type = IT_TOGGLE, .ok_arg = pick_decim, .arg = 1, .sel_ref = &g_settings.decim_idx },
    { .label = "x128",  .type = IT_TOGGLE, .ok_arg = pick_decim, .arg = 2, .sel_ref = &g_settings.decim_idx },
    { .label = "x256",  .type = IT_TOGGLE, .ok_arg = pick_decim, .arg = 3, .sel_ref = &g_settings.decim_idx },
    { .label = "x512",  .type = IT_TOGGLE, .ok_arg = pick_decim, .arg = 4, .sel_ref = &g_settings.decim_idx },
    { .label = "x1024", .type = IT_TOGGLE, .ok_arg = pick_decim, .arg = 5, .sel_ref = &g_settings.decim_idx },
};
static const menu_t m_decim = { "Decimation", decim_items, 6 };

static const menu_item_t mainboard_items[] = {
    { .label = "Power",   .type = IT_SUBMENU, .sub = &m_power },
    { .label = "Scripts", .type = IT_CYCLE,   .ok = scripts_open },
};
static const menu_t m_mainboard = { "Main Board Settings", mainboard_items, 2 };

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
// Custom MicroPython Scripts screen (dynamic list, tunneled from the S3).
static bool             s_in_scripts = false;
static ddp_mb_scripts_t s_scr;             // last script snapshot from the S3
static int              s_scr_sel = 0;     // selection over [Stop?]+scripts
static uint32_t         s_scr_last_req = 0;

// DUT source calibration wizard (driven on the P4 over DDP).
static bool             s_in_cal = false;
static uint8_t          s_cal_mode = 0;    // DDP_CAL_MODE_*
static ddp_cal_status_t s_calst;           // latest status from the P4
static uint32_t         s_cal_last_req = 0;

// Firmware screen: installed-vs-available per MCU, tunneled C6 -> P4 -> S3.
// Three stages on one screen: the device table, a release picker for the
// selected device, then a confirmation before anything is flashed.
#define FW_STAGE_TABLE   0
#define FW_STAGE_PICK    1
#define FW_STAGE_CONFIRM 2
static bool             s_in_fw = false;
static uint8_t          s_fw_stage = FW_STAGE_TABLE;
static ddp_mb_fwinfo_t  s_fwi;
static bool             s_fwi_have = false;
static int              s_fw_dev = 0;      // DDP_FW_IDX_*
static int              s_fw_rel = 0;      // index into s_fwi.rel[]
static uint32_t         s_fw_last_req = 0;
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
     * guard so the test board (TARGET_C3) doesn't panic here. The range must
     * also map to a single HW range — (-10,110) spans two and makes install
     * fail (blanking the C6 die-temp row), so use (-10,80). */
    if (!s_c6_tsens) {
        temperature_sensor_config_t tcfg = TEMPERATURE_SENSOR_CONFIG_DEFAULT(-10, 80);
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
    s_in_detail = false;
    s_in_scripts = false;
    s_in_cal = false;
    s_in_fw = false;
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
            if (it->ok_arg) it->ok_arg(it->arg);
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

// Refresh the mainboard power snapshot. Pulls from the S3 only while the Power
// menu is open (on-demand), and folds the latest returned snapshot into the
// e-fuse status + rail setpoint displays.
static void mb_refresh(uint32_t now_ms)
{
    if (s_active && cur_menu() == &m_power) {
        if (now_ms - s_mb_last_req >= 500) {
            s_mb_last_req = now_ms;
            ddp_send_mb_request(DDP_MB_POWER, NULL, 0);
        }
    }
    ddp_mb_power_t p; uint32_t age;
    if (ddp_get_mb_power(&p, &age) && age < 5000) {
        s_mbp = p;
        s_mbp_valid = true;
        if (!s_in_editor) {            // don't fight an in-progress rail edit
            if (p.vlogic_mv) s_vlogic_mv = p.vlogic_mv;
            if (p.vadj1_mv)  s_vadj1_mv  = p.vadj1_mv;
            if (p.vadj2_mv)  s_vadj2_mv  = p.vadj2_mv;
        }
    }

    // Converge pending power writes: drop satisfied bits, re-drive the rest so a
    // dropped DDP frame can't lose a toggle. Bounded so a genuinely stuck write
    // (e.g. an e-fuse that trips right back off) stops retrying.
    if (s_mbp_valid) {
        for (int i = 0; i < 4; i++)
            if ((s_want_efuse_mask & (1u << i)) &&
                (!!(s_mbp.efuse_en & (1u << i)) == !!(s_want_efuse_val & (1u << i))))
                s_want_efuse_mask &= (uint8_t)~(1u << i);
        for (int r = 0; r < 3; r++)
            if ((s_want_rail_mask & (1u << r)) &&
                (!!(s_mbp.rail_en & (1u << r)) == !!(s_want_rail_val & (1u << r))))
                s_want_rail_mask &= (uint8_t)~(1u << r);
    }
    if (s_want_efuse_mask || s_want_rail_mask) {
        if ((int32_t)(now_ms - s_want_until) >= 0) {
            s_want_efuse_mask = 0;
            s_want_rail_mask  = 0;
        } else if (now_ms - s_want_last >= 300) {
            s_want_last = now_ms;
            if (s_want_efuse_mask) {
                int i = __builtin_ctz(s_want_efuse_mask);
                uint8_t a[2] = { (uint8_t)i, (uint8_t)((s_want_efuse_val >> i) & 1) };
                ddp_send_mb_request(DDP_MB_SET_EFUSE, a, sizeof(a));
            } else {
                int r = __builtin_ctz(s_want_rail_mask);
                uint8_t a[2] = { (uint8_t)r, (uint8_t)((s_want_rail_val >> r) & 1) };
                ddp_send_mb_request(DDP_MB_SET_RAIL_EN, a, sizeof(a));
            }
        }
    }
}

// ---- Custom Scripts screen -------------------------------------------------
// Number of selectable rows: the script list, plus a leading "Stop" row while a
// script is running.
static int scr_total(void)
{
    bool running = (s_scr.state == DDP_MB_SCR_RUNNING);
    return s_scr.count + (running ? 1 : 0);
}

static void scripts_open(void)
{
    s_in_scripts = true;
    s_scr_sel = 0;
    s_scr_last_req = 0;   // request the list immediately in scr_refresh()
}

// Pull the script list + engine status from the S3 while the screen is open.
static void scr_refresh(uint32_t now_ms)
{
    if (!s_in_scripts) return;
    if (now_ms - s_scr_last_req >= 1200) {
        s_scr_last_req = now_ms;
        ddp_send_mb_request(DDP_MB_SCRIPTS, NULL, 0);
    }
    ddp_mb_scripts_t s; uint32_t age;
    if (ddp_get_mb_scripts(&s, &age) && age < 5000) {
        s_scr = s;
        int total = scr_total();
        if (s_scr_sel >= total) s_scr_sel = total > 0 ? total - 1 : 0;
    }
}

static void handle_scripts_event(uint32_t ev)
{
    bool running = (s_scr.state == DDP_MB_SCR_RUNNING);
    int total = scr_total();
    if (ev & BTN_EV_UP)   { if (s_scr_sel > 0) s_scr_sel--; }
    if (ev & BTN_EV_DOWN) { if (s_scr_sel < total - 1) s_scr_sel++; }
    if (ev & BTN_EV_BACK) { s_in_scripts = false; return; }
    if (ev & BTN_EV_OK) {
        if (running && s_scr_sel == 0) {
            ddp_send_mb_request(DDP_MB_SCRIPT_STOP, NULL, 0);
        } else {
            int si = running ? s_scr_sel - 1 : s_scr_sel;
            if (si >= 0 && si < s_scr.count) {
                const char *nm = s_scr.name[si];
                int nl = (int)strlen(nm);
                if (nl > MB_SCR_NAME_MAX) nl = MB_SCR_NAME_MAX;
                uint8_t args[1 + MB_SCR_NAME_MAX];
                args[0] = (uint8_t)nl;
                memcpy(&args[1], nm, nl);
                ddp_send_mb_request(DDP_MB_SCRIPT_RUN, args, (uint8_t)(1 + nl));
            }
        }
        s_scr_last_req = 0;   // refresh status promptly after the action
    }
}

// ---- Firmware / update screen ----------------------------------------------
// Device labels + the update-target bit each row maps to, in DDP_FW_IDX_ order.
static const char *const FW_DEV_NAME[DDP_FW_DEV_MAX] = { "RP2040", "ESP32-S3", "ESP32-P4", "ESP32-C6" };
static const uint8_t     FW_DEV_BIT[DDP_FW_DEV_MAX]  = { DDP_FW_T_RP2040, DDP_FW_T_S3,
                                                         DDP_FW_T_P4, DDP_FW_T_C6 };

// A row is selectable only when the S3 reported a version for it — an absent
// MCU (no HAT, or a C6 that has not answered yet) must not be flashable.
static bool fw_dev_present(int i)
{
    return i >= 0 && i < (int)DDP_FW_DEV_MAX && s_fwi.installed[i][0] != '\0';
}

static void fw_open(void)
{
    s_in_fw = true;
    s_fw_stage = FW_STAGE_TABLE;
    s_fw_dev = 0;
    s_fw_rel = 0;
    s_fw_last_req = 0;   // fetch immediately in fw_refresh()
    for (int i = 0; i < (int)DDP_FW_DEV_MAX; i++) {
        if (fw_dev_present(i)) { s_fw_dev = i; break; }
    }
}

static void fw_step_dev(int dir)
{
    for (int n = 0; n < (int)DDP_FW_DEV_MAX; n++) {
        s_fw_dev += dir;
        if (s_fw_dev < 0) { s_fw_dev = 0; return; }
        if (s_fw_dev >= (int)DDP_FW_DEV_MAX) { s_fw_dev = DDP_FW_DEV_MAX - 1; return; }
        if (fw_dev_present(s_fw_dev)) return;
    }
}

// Poll the S3 while the screen is open. The first reply is usually state
// CHECKING with an empty release list (the S3 kicks off the GitHub query on the
// worker); polling faster while checking makes the list appear promptly.
// Also poll faster while APPLYING so the progress bar (render_fw()) moves
// smoothly instead of jumping every 4 s.
static void fw_refresh(uint32_t now_ms)
{
    if (!s_in_fw) return;
    uint32_t period = (s_fwi.state == DDP_FW_ST_CHECKING || s_fwi.state == DDP_FW_ST_APPLYING)
                       ? 1000 : 4000;
    if (now_ms - s_fw_last_req >= period) {
        s_fw_last_req = now_ms;
        ddp_send_mb_request(DDP_MB_FWINFO, NULL, 0);
    }
    ddp_mb_fwinfo_t f; uint32_t age;
    if (ddp_get_mb_fwinfo(&f, &age) && age < 15000) {
        s_fwi = f;
        s_fwi_have = true;
        if (s_fw_rel >= s_fwi.rel_count) s_fw_rel = s_fwi.rel_count ? s_fwi.rel_count - 1 : 0;
        if (!fw_dev_present(s_fw_dev)) fw_step_dev(0);
    }
    // A flash takes far longer than the idle timeout and must not be
    // interrupted by the menu closing under it.
    if (s_fwi.state == DDP_FW_ST_APPLYING) s_last_input = now_ms;
}

static void handle_fw_event(uint32_t ev)
{
    switch (s_fw_stage) {
    case FW_STAGE_TABLE:
        if (ev & BTN_EV_UP)   fw_step_dev(-1);
        if (ev & BTN_EV_DOWN) fw_step_dev(+1);
        if (ev & BTN_EV_BACK) { s_in_fw = false; return; }
        if (ev & BTN_EV_OK) {
            if (s_fwi.rel_count > 0 && fw_dev_present(s_fw_dev)) {
                s_fw_rel = 0;
                s_fw_stage = FW_STAGE_PICK;
            }
        }
        break;

    case FW_STAGE_PICK:
        if ((ev & BTN_EV_UP)   && s_fw_rel > 0) s_fw_rel--;
        if ((ev & BTN_EV_DOWN) && s_fw_rel < s_fwi.rel_count - 1) s_fw_rel++;
        if (ev & BTN_EV_BACK) { s_fw_stage = FW_STAGE_TABLE; return; }
        if (ev & BTN_EV_OK)   s_fw_stage = FW_STAGE_CONFIRM;
        break;

    case FW_STAGE_CONFIRM:
        if (ev & BTN_EV_BACK) { s_fw_stage = FW_STAGE_PICK; return; }
        if (ev & BTN_EV_OK) {
            uint8_t args[2] = { (uint8_t)s_fw_rel, FW_DEV_BIT[s_fw_dev] };
            ddp_send_mb_request(DDP_MB_FW_APPLY, args, sizeof(args));
            s_fw_last_req = 0;              // show the new state promptly
            s_fw_stage = FW_STAGE_TABLE;
        }
        break;
    }
}

// ---- DUT source calibration wizard ----------------------------------------
static void cal_open(uint8_t mode)
{
    s_in_cal = true;
    s_cal_mode = mode;
    memset(&s_calst, 0, sizeof(s_calst));
    s_cal_last_req = 0;
    ddp_send_cal_ctrl(DDP_CAL_OP_START, mode);   // kick off; P4 -> PROMPT
}
static void cal_open_volt(void) { cal_open(DDP_CAL_MODE_VOLTAGE); }
static void cal_open_curr(void) { cal_open(DDP_CAL_MODE_CURRENT); }
static void cal_open_base(void) { cal_open(DDP_CAL_MODE_BASELINE); }

// Poll the live calibration status from the P4 while the wizard is open.
static void cal_refresh(uint32_t now_ms)
{
    if (!s_in_cal) return;
    if (now_ms - s_cal_last_req >= 200) {
        s_cal_last_req = now_ms;
        ddp_send_cal_ctrl(DDP_CAL_OP_STATUS, 0);
    }
    ddp_cal_status_t st; uint32_t age;
    if (ddp_get_cal_status(&st, &age) && age < 3000) s_calst = st;
    // A running sweep takes far longer than the 30 s idle timeout with no key
    // presses, so keep the menu alive while the P4 is actively calibrating.
    if (s_calst.phase == DDP_CAL_PH_RUNNING || s_calst.phase == DDP_CAL_PH_PROMPT)
        s_last_input = now_ms;
}

static void handle_cal_event(uint32_t ev)
{
    if (ev & BTN_EV_BACK) {
        ddp_send_cal_ctrl(DDP_CAL_OP_ABORT, 0);
        s_in_cal = false;
        return;
    }
    if (ev & BTN_EV_OK) {
        if (s_calst.phase == DDP_CAL_PH_PROMPT) {
            ddp_send_cal_ctrl(DDP_CAL_OP_ACK, 0);         // operator ready
        } else if (s_calst.phase == DDP_CAL_PH_IDLE ||
                   s_calst.phase == DDP_CAL_PH_SUCCESS ||
                   s_calst.phase == DDP_CAL_PH_FAILED) {
            ddp_send_cal_ctrl(DDP_CAL_OP_START, s_cal_mode);  // (re)run
        }
        s_cal_last_req = 0;   // fetch fresh status promptly
    }
}

menu_status_t menu_update(uint32_t events, uint32_t now_ms, bool *need_render)
{
    s_anim_ms = now_ms;
    diag_refresh();
    mb_refresh(now_ms);
    scr_refresh(now_ms);
    cal_refresh(now_ms);
    fw_refresh(now_ms);
    bool render = false;

    if (events) {
        s_last_input = now_ms;
        if (s_in_cal)         handle_cal_event(events);
        else if (s_in_fw)     handle_fw_event(events);
        else if (s_in_scripts) handle_scripts_event(events);
        else if (s_in_detail) handle_detail_event(events);
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

// Device health bubbles for the root Settings title strip (mirrors the home
// screen). Green = the device is reporting and within limits, red = missing/
// stale telemetry or an over-temperature reading. Derived from the latest DDP
// diagnostics snapshot pushed by the P4.
static void draw_status_bubbles(void)
{
    ddp_diag_t dg; uint32_t age;
    bool live = ddp_get_diag(&dg, &age) && age < 3000;
    const int16_t T_HOT = 900;   // 90.0 C over-temp threshold
    const int16_t T_NA  = (int16_t)DDP_DIAG_TEMP_NA;

    const char *lbl[5] = { "S3", "P4", "A1", "A2", "A3" };
    bool ok[5];
    // S3 is remote: it must be relaying fresh telemetry to count as good.
    ok[0] = live && (dg.valid & DDP_DIAG_V_S3) &&
            (dg.t_s3_c10 == T_NA || dg.t_s3_c10 < T_HOT);
    // P4 is the sender: a live frame implies it is alive; flag red only when
    // its own die sensor reads over-temp.
    ok[1] = live && !((dg.valid & DDP_DIAG_V_P4TEMP) &&
                      dg.t_p4_c10 != T_NA && dg.t_p4_c10 >= T_HOT);
    // ADAQ die temps read NA while streaming (that is normal, not a fault), so
    // treat NA as good and only flag a genuine over-temp reading.
    int16_t  at[3] = { dg.t_adaq0_c10, dg.t_adaq1_c10, dg.t_adaq2_c10 };
    uint16_t av[3] = { DDP_DIAG_V_ADAQ0, DDP_DIAG_V_ADAQ1, DDP_DIAG_V_ADAQ2 };
    for (int i = 0; i < 3; i++)
        ok[2 + i] = live && !((dg.valid & av[i]) && at[i] != T_NA && at[i] >= T_HOT);

    int x = 84;
    for (int i = 0; i < 5; i++) {
        ui_draw_dot(x + 2, 8, ok[i] ? g_theme.green : g_theme.rose);
        gfx_text(x + 6, 5, lbl[i], 1, g_theme.dim);
        x += 6 + gfx_text_w(lbl[i], 1) + 7;
    }
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
                if (it->value) {
                    char buf[20];
                    it->value(buf, sizeof(buf));
                    int vw = gfx_text_w(buf, 1);
                    gfx_text(rx - 8 - vw, ty, buf, 1, vc);
                }
                draw_chevron(rx - 4, row_cy - 4, lc);
            } else if (it->value) {
                char buf[20];
                it->value(buf, sizeof(buf));
                int vw = gfx_text_w(buf, 1);
                uint16_t val_c = (it->value_alert && it->value_alert())
                                     ? g_theme.rose : vc;
                gfx_text(rx - vw, ty, buf, 1, val_c);
                if (it->warn && it->warn())
                    draw_warning(rx - vw - 14, row_cy - 4, g_theme.amber);
            }
            // Radio-list selected marker.
            if (it->sel_ref && *it->sel_ref == it->arg)
                ui_draw_dot(rx - 2, row_cy, lc);
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

    // Root Settings screen: show the device health bubbles in the title strip.
    if (m == &m_root) draw_status_bubbles();
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

// DUT source calibration wizard screen: mode title + phase pill, the operator
// prompt ("short/disconnect/leave open the output"), the live measured value,
// and a progress bar. OK acks the prompt or (re)starts; BACK aborts + exits.
static void render_cal(void)
{
    gfx_clear(g_theme.bg);

    gfx_fill_rect(0, 0, DISP_WIDTH, TITLE_H, g_theme.header);
    gfx_hline(0, TITLE_H, DISP_WIDTH, g_theme.border);
    const char *mname = s_cal_mode == DDP_CAL_MODE_VOLTAGE ? "Cal: Voltage" :
                        s_cal_mode == DDP_CAL_MODE_CURRENT ? "Cal: Current" :
                                                             "Cal: Baseline";
    gfx_text(6, 5, mname, 1, g_theme.dim);

    const char *ph; uint16_t pc;
    switch (s_calst.phase) {
        case DDP_CAL_PH_PROMPT:  ph = "WAIT"; pc = g_theme.amber; break;
        case DDP_CAL_PH_RUNNING: ph = "RUN";  pc = g_theme.green; break;
        case DDP_CAL_PH_SUCCESS: ph = "DONE"; pc = g_theme.green; break;
        case DDP_CAL_PH_FAILED:  ph = "FAIL"; pc = g_theme.rose;  break;
        default:                 ph = "IDLE"; pc = g_theme.muted; break;
    }
    int pw = gfx_text_w(ph, 1);
    ui_draw_dot(DISP_WIDTH - pw - 12, 9, pc);
    gfx_text(DISP_WIDTH - pw - 6, 5, ph, 1, pc);

    int y = TITLE_H + 6;
    if (s_calst.phase == DDP_CAL_PH_PROMPT) {
        // Big, bold, centered operator prompt.
        const char *msg = s_calst.prompt == DDP_CAL_PR_SHORT      ? "SHORT OUTPUT" :
                          s_calst.prompt == DDP_CAL_PR_DISCONNECT ? "REMOVE LOAD"  :
                          s_calst.prompt == DDP_CAL_PR_OPEN       ? "LEAVE OPEN"   :
                                                                    "PREP OUTPUT";
        int tw = gfx_text_w(msg, 2);
        int mx = (DISP_WIDTH - tw) / 2; if (mx < 2) mx = 2;
        int my = TITLE_H + 12;
        gfx_text(mx,     my, msg, 2, g_theme.amber);   // draw twice (+1px) for a
        gfx_text(mx + 1, my, msg, 2, g_theme.amber);   // faux-bold weight
        const char *hint = "then press OK";
        gfx_text((DISP_WIDTH - gfx_text_w(hint, 1)) / 2, my + 20, hint, 1, g_theme.dim);
    } else if (s_calst.phase == DDP_CAL_PH_SUCCESS) {
        const char *ps = s_calst.persist == DDP_CAL_PERSIST_SAVED  ? "Saved to NVS" :
                         s_calst.persist == DDP_CAL_PERSIST_FAILED ? "Save FAILED" : "Complete";
        gfx_text(6, y, "Calibration complete", 1, g_theme.green);
        gfx_text(6, y + 12, ps, 1, g_theme.dim);
    } else if (s_calst.phase == DDP_CAL_PH_FAILED) {
        if (s_calst.flags & DDP_CAL_FLAG_NO_PD) {
            gfx_text(6, y, "Need USB-PD 20V/3A", 1, g_theme.rose);
            gfx_text(6, y + 12, "connect a PD source", 1, g_theme.dim);
        } else {
            char fb[24];
            gfx_text(6, y, "Calibration failed", 1, g_theme.rose);
            snprintf(fb, sizeof(fb), "flags 0x%04X", s_calst.flags);
            gfx_text(6, y + 12, fb, 1, g_theme.dim);
        }
    } else if (s_calst.phase == DDP_CAL_PH_RUNNING) {
        char mb[28];
        const char *unit = s_cal_mode == DDP_CAL_MODE_CURRENT ? "A" : "V";
        int mu = (int)lroundf(s_calst.measured * 1000.0f);
        int neg = mu < 0; if (neg) mu = -mu;
        snprintf(mb, sizeof(mb), "%s%d.%03d %s  c%d", neg ? "-" : "",
                 mu / 1000, mu % 1000, unit, s_calst.code);
        gfx_text(6, y, mb, 1, g_theme.text);
        char pt[20];
        snprintf(pt, sizeof(pt), "point %u", (unsigned)s_calst.point);
        gfx_text(6, y + 12, pt, 1, g_theme.dim);
    } else {
        gfx_text(6, y, "Press OK to start", 1, g_theme.dim);
    }

    // Progress bar along the bottom.
    int bx = 6, bw = DISP_WIDTH - 12, by = DISP_HEIGHT - 12, bh = 9;
    int frac = s_calst.progress > 100 ? 100 : s_calst.progress;
    char pb[8];
    snprintf(pb, sizeof(pb), "%d%%", frac);
    gfx_text(DISP_WIDTH - gfx_text_w(pb, 1) - 6, by - 10, pb, 1, g_theme.muted);
    gfx_round_rect_border(bx, by, bw, bh, 3, g_theme.border);
    int fillw = frac * (bw - 4) / 100;
    if (fillw > 0) gfx_round_rect(bx + 2, by + 2, fillw, bh - 4, 2, g_theme.sel);
}

// Firmware screen. The 284 px width fits a real table, so all four MCUs show
// installed and available side by side without scrolling; OK drills into a
// release picker for the highlighted MCU and then a confirmation.
static void render_fw(void)
{
    gfx_clear(g_theme.bg);
    gfx_fill_rect(0, 0, DISP_WIDTH, TITLE_H, g_theme.header);
    gfx_hline(0, TITLE_H, DISP_WIDTH, g_theme.border);

    const char *sstate; uint16_t scol;
    switch (s_fwi.state) {
        case DDP_FW_ST_CHECKING: sstate = "CHECKING"; scol = g_theme.amber; break;
        case DDP_FW_ST_APPLYING: sstate = "UPDATING"; scol = g_theme.cyan;  break;
        case DDP_FW_ST_ERROR:    sstate = "NO NET";   scol = g_theme.rose;  break;
        default:                 sstate = "OK";       scol = g_theme.green; break;
    }
    int stw = gfx_text_w(sstate, 1);
    ui_draw_dot(DISP_WIDTH - stw - 12, 9, scol);
    gfx_text(DISP_WIDTH - stw - 6, 5, sstate, 1, scol);

    // While a flash is in flight, take over the whole screen with a progress
    // bar (same layout as the cal wizard's, see render_cal()) instead of the
    // device table -- there is nothing else useful to show, and the table
    // would imply the user could still navigate/select another target.
    if (s_fwi.state == DDP_FW_ST_APPLYING) {
        gfx_text(6, 5, "Updating", 1, g_theme.dim);

        const char *devname = "device";
        for (int i = 0; i < (int)DDP_FW_DEV_MAX; i++) {
            if (FW_DEV_BIT[i] == s_fwi.active_target) { devname = FW_DEV_NAME[i]; break; }
        }
        char line[40];
        snprintf(line, sizeof(line), "%s", devname);
        gfx_text(6, TITLE_H + 14, line, 1, g_theme.text);
        gfx_text(6, TITLE_H + 30, "Do not power off during the update.", 1, g_theme.amber);

        // Progress bar along the bottom, identical geometry/style to render_cal().
        int bx = 6, bw = DISP_WIDTH - 12, by = DISP_HEIGHT - 12, bh = 9;
        int frac = 0;
        if (s_fwi.progress_total > 0) {
            frac = (int)((uint64_t)s_fwi.progress_done * 100 / s_fwi.progress_total);
            if (frac > 100) frac = 100;
        }
        // 16 (not 8): frac's range isn't provable at compile time from the u64
        // divide above (unlike render_cal()'s already-narrow uint8_t source),
        // so -Werror=format-truncation sizes against a full int's digit count.
        char pb[16];
        snprintf(pb, sizeof(pb), "%d%%", frac);
        gfx_text(DISP_WIDTH - gfx_text_w(pb, 1) - 6, by - 10, pb, 1, g_theme.muted);
        gfx_round_rect_border(bx, by, bw, bh, 3, g_theme.border);
        int fillw = frac * (bw - 4) / 100;
        if (fillw > 0) gfx_round_rect(bx + 2, by + 2, fillw, bh - 4, 2, g_theme.sel);
        return;
    }

    if (s_fw_stage == FW_STAGE_CONFIRM) {
        gfx_text(6, 5, "Confirm Update", 1, g_theme.dim);
        char line[64];
        snprintf(line, sizeof(line), "Update %s to %s?",
                 FW_DEV_NAME[s_fw_dev], s_fwi.rel[s_fw_rel]);
        gfx_text(8, TITLE_H + 12, line, 1, g_theme.text);
        gfx_text(8, TITLE_H + 26, "Do not power off during the update.", 1, g_theme.amber);
        gfx_text(8, TITLE_H + 42, "OK = update      BACK = cancel", 1, g_theme.muted);
        return;
    }

    if (s_fw_stage == FW_STAGE_PICK) {
        char title[40];
        snprintf(title, sizeof(title), "%s - select version", FW_DEV_NAME[s_fw_dev]);
        gfx_text(6, 5, title, 1, g_theme.dim);

        const int RH = 11;
        int top = TITLE_H + 3;
        int rows_vis = (DISP_HEIGHT - top) / RH;
        if (rows_vis < 1) rows_vis = 1;
        int first = s_fw_rel - rows_vis / 2;
        if (first > s_fwi.rel_count - rows_vis) first = s_fwi.rel_count - rows_vis;
        if (first < 0) first = 0;

        for (int r = 0; r < rows_vis && (first + r) < s_fwi.rel_count; r++) {
            int idx = first + r;
            int y = top + r * RH;
            bool sel = (idx == s_fw_rel);
            if (sel) gfx_round_rect(3, y - 1, DISP_WIDTH - 6, RH, 3, g_theme.sel);
            uint16_t tc = sel ? g_theme.sel_text : g_theme.text;
            gfx_text(8, y + 1, s_fwi.rel[idx], 1, tc);
            if (idx == 0) gfx_text(120, y + 1, "(latest)", 1, sel ? g_theme.sel_text : g_theme.muted);
            // Mark the release the selected MCU is already running.
            if (strcmp(s_fwi.rel[idx], s_fwi.installed[s_fw_dev]) == 0)
                gfx_text(DISP_WIDTH - 60, y + 1, "installed", 1, sel ? g_theme.sel_text : g_theme.green);
        }
        return;
    }

    // ---- Stage 1: the device table -----------------------------------------
    gfx_text(6, 5, "Firmware", 1, g_theme.dim);

    const int COL_DEV = 8, COL_INST = 96, COL_AVAIL = 186;
    const int RH = 12;
    int top = TITLE_H + 2;

    gfx_text(COL_DEV,   top, "DEVICE",    1, g_theme.muted);
    gfx_text(COL_INST,  top, "INSTALLED", 1, g_theme.muted);
    gfx_text(COL_AVAIL, top, "AVAILABLE", 1, g_theme.muted);
    top += 10;

    const char *latest = (s_fwi.rel_count > 0) ? s_fwi.rel[0] : "--";

    for (int i = 0; i < (int)DDP_FW_DEV_MAX; i++) {
        int y = top + i * RH;
        bool present = fw_dev_present(i);
        bool sel = (i == s_fw_dev) && present;
        if (sel) gfx_round_rect(3, y - 1, DISP_WIDTH - 6, RH, 3, g_theme.sel);

        uint16_t tc = sel ? g_theme.sel_text
                          : (present ? g_theme.text : g_theme.muted);
        gfx_text(COL_DEV, y + 1, FW_DEV_NAME[i], 1, tc);
        gfx_text(COL_INST, y + 1, present ? s_fwi.installed[i] : "absent", 1, tc);

        if (!present) continue;

        bool upd = (s_fwi.update_avail & FW_DEV_BIT[i]) != 0;
        uint16_t ac = sel ? g_theme.sel_text : (upd ? g_theme.amber : g_theme.green);
        gfx_text(COL_AVAIL, y + 1, latest, 1, ac);
        if (upd) gfx_text(DISP_WIDTH - 26, y + 1, "NEW", 1, ac);
    }
}

// MicroPython Scripts screen: engine status pill + a scrollable list of stored
// scripts. OK runs the selected script (or stops the running one via the "Stop"
// row); BACK returns to the menu. Data is tunneled from the S3 on demand.
static void render_scripts(void)
{
    gfx_clear(g_theme.bg);

    // Title strip + engine-state pill on the right.
    gfx_fill_rect(0, 0, DISP_WIDTH, TITLE_H, g_theme.header);
    gfx_hline(0, TITLE_H, DISP_WIDTH, g_theme.border);
    gfx_text(6, 5, "Scripts", 1, g_theme.dim);

    const char *sstate; uint16_t scol;
    switch (s_scr.state) {
        case DDP_MB_SCR_RUNNING: sstate = "RUNNING"; scol = g_theme.green; break;
        case DDP_MB_SCR_CRASHED: sstate = "CRASHED"; scol = g_theme.rose;  break;
        case DDP_MB_SCR_EXITED:  sstate = "EXITED";  scol = g_theme.amber; break;
        default:                 sstate = "IDLE";    scol = g_theme.muted; break;
    }
    int stw = gfx_text_w(sstate, 1);
    ui_draw_dot(DISP_WIDTH - stw - 12, 9, scol);
    gfx_text(DISP_WIDTH - stw - 6, 5, sstate, 1, scol);

    bool running = (s_scr.state == DDP_MB_SCR_RUNNING);
    int total = scr_total();

    // Crash detail line (truncated) just under the title.
    int list_top = TITLE_H + 2;
    if (s_scr.state == DDP_MB_SCR_CRASHED && s_scr.err[0]) {
        gfx_text(6, list_top, s_scr.err, 1, g_theme.rose);
        list_top += 10;
    }

    if (total == 0) {
        gfx_text(8, list_top + 6, "(no scripts)", 1, g_theme.muted);
        return;
    }

    // Windowed, scrolling list keeping the selection visible.
    const int RH = 11;
    int rows_vis = (DISP_HEIGHT - list_top) / RH;
    if (rows_vis < 1) rows_vis = 1;
    int first = s_scr_sel - rows_vis / 2;
    if (first > total - rows_vis) first = total - rows_vis;
    if (first < 0) first = 0;

    for (int r = 0; r < rows_vis && (first + r) < total; r++) {
        int idx = first + r;
        int y = list_top + r * RH;
        bool sel = (idx == s_scr_sel);
        if (sel) gfx_round_rect(3, y - 1, DISP_WIDTH - 6, RH, 3, g_theme.sel);
        uint16_t tc = sel ? g_theme.sel_text : g_theme.text;

        if (running && idx == 0) {
            gfx_text(8, y + 1, "Stop", 1, sel ? g_theme.sel_text : g_theme.rose);
        } else {
            int si = running ? idx - 1 : idx;
            if (si >= 0 && si < s_scr.count)
                gfx_text(8, y + 1, s_scr.name[si], 1, tc);
        }
    }
}

void menu_render(uint32_t now_ms)
{
    s_last_paint = now_ms;
    if (s_in_detail)      render_detail();
    else if (s_in_editor) render_editor();
    else if (s_in_fw)     render_fw();
    else if (s_in_scripts) render_scripts();
    else if (s_in_cal)    render_cal();
    else                  render_menu();
}
