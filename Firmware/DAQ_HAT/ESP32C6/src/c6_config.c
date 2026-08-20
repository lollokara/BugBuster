// =============================================================================
// c6_config.c — settings_t <-> registry-TLV mapping for the C6.
// =============================================================================

#include "c6_config.h"
#include "settings.h"
#include "ddp.h"
#include "daq_config_registry.h"
#include "display.h"
#include "theme.h"
#include "ui.h"

#include <string.h>

// Read a C6-tracked scalar setting by registry key. Returns false if the key is
// not mirrored on the C6.
static bool field_get_i32(uint16_t key, int32_t *out)
{
    switch (key) {
    case DAQ_K_AUTORANGING:     *out = g_settings.autoranging;     return true;
    case DAQ_K_RANGE_IDX:       *out = g_settings.range_idx;       return true;
    case DAQ_K_SAMPLE_RATE_IDX: *out = g_settings.sample_rate_idx; return true;
    case DAQ_K_DUT_ILIMIT_MA:   *out = g_settings.dut_current_ma;  return true;
    case DAQ_K_DUT_VOLTAGE_MV:  *out = g_settings.dut_voltage_mv;  return true;
    case DAQ_K_FILTER:          *out = g_settings.filter_idx;      return true;
    case DAQ_K_DECIMATION:      *out = g_settings.decim_idx;       return true;
    case DAQ_K_REJECT_5060:     *out = g_settings.reject_5060;     return true;
    case DAQ_K_SR_MODE:         *out = g_settings.sr_mode;         return true;
    case DAQ_K_FFT_ENABLE:      *out = g_settings.fft_enable;      return true;
    case DAQ_K_FFT_LENGTH:      *out = g_settings.fft_length_idx;  return true;
    case DAQ_K_FFT_WINDOW:      *out = g_settings.fft_window_idx;  return true;
    case DAQ_K_FFT_SOURCE:      *out = g_settings.fft_source_idx;  return true;
    case DAQ_K_BRIGHTNESS_PCT:  *out = g_settings.brightness_pct;  return true;
    case DAQ_K_DARK_MODE:       *out = g_settings.dark_mode;       return true;
    case DAQ_K_NPX_MODE:        *out = g_settings.npx_mode;        return true;
    case DAQ_K_NPX_BRIGHTNESS:  *out = g_settings.npx_brightness;  return true;
    case DAQ_K_NPX_COLOR:       *out = (int32_t)g_settings.npx_color; return true;
    case DAQ_K_WIFI_ENABLE:     *out = g_settings.wifi_enable;     return true;
    case DAQ_K_WIFI_MODE:       *out = g_settings.wifi_mode;       return true;
    default:                    return false;
    }
}

// Apply a scalar value from the P4 into g_settings, running any local side
// effect (backlight, theme). Unknown keys are ignored.
static void field_apply_i32(uint16_t key, int32_t v)
{
    switch (key) {
    case DAQ_K_AUTORANGING:     g_settings.autoranging     = (v != 0); break;
    case DAQ_K_RANGE_IDX:       g_settings.range_idx       = v;        break;
    case DAQ_K_SAMPLE_RATE_IDX: g_settings.sample_rate_idx = v;        break;
    case DAQ_K_DUT_ILIMIT_MA:   g_settings.dut_current_ma  = v;        break;
    case DAQ_K_DUT_VOLTAGE_MV:  g_settings.dut_voltage_mv  = v;        break;
    case DAQ_K_FILTER:          g_settings.filter_idx      = v;        break;
    case DAQ_K_DECIMATION:      g_settings.decim_idx       = v;        break;
    case DAQ_K_REJECT_5060:     g_settings.reject_5060     = (v != 0); break;
    case DAQ_K_SR_MODE:         g_settings.sr_mode         = (v != 0); break;
    case DAQ_K_FFT_ENABLE:      g_settings.fft_enable      = (v != 0); break;
    case DAQ_K_FFT_LENGTH:      g_settings.fft_length_idx  = v;        break;
    case DAQ_K_FFT_WINDOW:      g_settings.fft_window_idx  = v;        break;
    case DAQ_K_FFT_SOURCE:      g_settings.fft_source_idx  = v;        break;
    case DAQ_K_BRIGHTNESS_PCT:
        g_settings.brightness_pct = v;
        display_set_backlight((uint8_t)((v < 0 ? 0 : v > 100 ? 100 : v) * 255 / 100));
        break;
    case DAQ_K_DARK_MODE:
        g_settings.dark_mode = (v != 0);
        theme_set_dark(g_settings.dark_mode);
        ui_refresh_theme();
        break;
    case DAQ_K_NPX_MODE:        g_settings.npx_mode        = v;            break;
    case DAQ_K_NPX_BRIGHTNESS:  g_settings.npx_brightness  = v;            break;
    case DAQ_K_NPX_COLOR:       g_settings.npx_color       = (uint32_t)v;  break;
    case DAQ_K_WIFI_ENABLE:     g_settings.wifi_enable     = (v != 0);     break;
    case DAQ_K_WIFI_MODE:       g_settings.wifi_mode       = v;            break;
    default: break;
    }
}

static void str_apply(uint16_t key, const uint8_t *val, uint8_t vlen)
{
    char *dst = NULL; size_t cap = 0;
    if (key == DAQ_K_WIFI_SSID)          { dst = g_settings.ssid;     cap = sizeof(g_settings.ssid); }
    else if (key == DAQ_K_WIFI_PASSWORD) { dst = g_settings.password; cap = sizeof(g_settings.password); }
    if (!dst) return;
    size_t n = vlen;
    if (n > cap - 1) n = cap - 1;
    if (n && val) memcpy(dst, val, n);
    dst[n] = '\0';
}

// Scalar keys the C6 sends upstream on a menu commit.
//
// This list is INTENTIONALLY INCOMPLETE vs. the full registry. Keys excluded:
//   - P4-local keys with no C6 menu entry: DAQ_K_STREAMING, _USB_DECIMATION,
//     _MULTIRES_TIERS, _STATS_WINDOW_MS, _RANGE_DWELL_US, _RANGE_LOCK_US,
//     _RANGE_FLAP (range stability tunables are MCP/desktop-only; the C6 has
//     no UI for microsecond-precision tuning on a 3-button 284 px panel).
//   - DAQ_K_SOURCE_ENABLE: sent separately via c6_config_send_source_enable()
//     (see :122) because it requires USB-PD guard logic.
//   - String keys (WIFI_SSID, WIFI_PASSWORD): sent via str_apply() on a
//     CONFIG_PUSH from the P4, never originated by the C6.
//
// Before adding a key here, confirm: (1) the C6 menu exposes it (see menu.c
// m_hat_items / m_screen_items), (2) g_settings has the field, and (3)
// field_get_i32 / field_apply_i32 handle it.
static const uint16_t SEND_KEYS[] = {
    DAQ_K_AUTORANGING, DAQ_K_RANGE_IDX, DAQ_K_SAMPLE_RATE_IDX,
    DAQ_K_DUT_ILIMIT_MA, DAQ_K_DUT_VOLTAGE_MV,
    DAQ_K_FILTER, DAQ_K_DECIMATION, DAQ_K_REJECT_5060, DAQ_K_SR_MODE,
    DAQ_K_FFT_ENABLE, DAQ_K_FFT_LENGTH, DAQ_K_FFT_WINDOW, DAQ_K_FFT_SOURCE,
    DAQ_K_BRIGHTNESS_PCT, DAQ_K_DARK_MODE,
    DAQ_K_NPX_MODE, DAQ_K_NPX_BRIGHTNESS, DAQ_K_NPX_COLOR,
    DAQ_K_WIFI_ENABLE, DAQ_K_WIFI_MODE,
};

void c6_config_send(void)
{
    uint8_t buf[DDP_MAX_PAYLOAD];
    size_t off = 0;
    for (size_t i = 0; i < sizeof(SEND_KEYS) / sizeof(SEND_KEYS[0]); i++) {
        uint16_t key = SEND_KEYS[i];
        const daq_setting_schema_t *sc = daq_config_schema(key);
        if (!sc) continue;
        int32_t v = 0;
        if (!field_get_i32(key, &v)) continue;
        int n = daq_tlv_encode_i32(buf + off, sizeof(buf) - off, key, sc->type, v);
        if (n < 0) break;
        off += (size_t)n;
    }
    if (off) ddp_send_config_tlv(buf, (uint8_t)off);
}

void c6_config_send_source_enable(bool on)
{
    // SOURCE_ENABLE is a P4-local key (not in SEND_KEYS / g_settings): encode it
    // directly and send as a one-TLV CONFIG_SET. The P4 applies it via
    // smu_enable() in its on_apply() callback.
    uint8_t buf[8];
    const daq_setting_schema_t *sc = daq_config_schema(DAQ_K_SOURCE_ENABLE);
    uint8_t type = sc ? sc->type : DAQ_T_BOOL;
    int n = daq_tlv_encode_i32(buf, sizeof(buf), DAQ_K_SOURCE_ENABLE, type,
                               on ? 1 : 0);
    if (n > 0) ddp_send_config_tlv(buf, (uint8_t)n);
}

void c6_config_apply_push(const uint8_t *tlvs, uint16_t len)
{
    size_t off = 0;
    bool changed = false;
    while (off < len) {
        uint16_t key; uint8_t type, vlen; const uint8_t *val;
        int used = daq_tlv_parse(tlvs + off, len - off, &key, &type, &val, &vlen);
        if (used < 0) break;
        if (type == DAQ_T_STR) {
            str_apply(key, val, vlen);
        } else {
            int32_t v;
            if (daq_tlv_value_i32(type, val, vlen, &v)) field_apply_i32(key, v);
        }
        changed = true;
        off += (size_t)used;
    }
    if (changed) settings_save();
}
