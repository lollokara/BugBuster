// =============================================================================
// daq_settings_glue.c — translate settings-store changes into P4 subsystem calls.
// =============================================================================

#include "daq_settings_glue.h"
#include "daq_settings.h"
#include "daq_config_registry.h"

#include "esp_log.h"

static const char *TAG = "daqcfg-glue";

// Registry range index (0=A,1=mA,2=uA) -> hardware current_range_t.
//   A  (full scale) -> LO  (50 mohm, COARSE)
//   mA              -> MID (2 ohm,   FINE)
//   uA              -> HI  (51 ohm,  FINE)
static current_range_t range_idx_to_hw(int32_t idx)
{
    switch (idx) {
    case DAQ_RANGE_A:  return RANGE_LO;
    case DAQ_RANGE_MA: return RANGE_MID;
    case DAQ_RANGE_UA: return RANGE_HI;
    default:           return RANGE_UNKNOWN;
    }
}

static spectrum_window_t win_idx_to_hw(int32_t idx)
{
    switch (idx) {
    case DAQ_WIN_HANN:            return SPEC_WIN_HANN;
    case DAQ_WIN_BLACKMAN_HARRIS: return SPEC_WIN_BLACKMAN_HARRIS;
    case DAQ_WIN_RECT:
    default:                      return SPEC_WIN_RECT;
    }
}

// Apply the current autoranging/range settings to the range manager: when auto,
// release any override; when manual, force the configured range.
static void apply_ranging(daq_board_t *b)
{
    int32_t autorange = 1, range_idx = DAQ_RANGE_MA;
    daq_settings_get_i32(DAQ_K_AUTORANGING, &autorange);
    daq_settings_get_i32(DAQ_K_RANGE_IDX, &range_idx);

    if (autorange) {
        range_manager_force(&b->range, RANGE_UNKNOWN);   // release -> analog auto
    } else {
        range_manager_force(&b->range, range_idx_to_hw(range_idx));
    }
}

// Apply the current FFT length + window together (spectrum_configure needs both).
static void apply_spectrum(daq_board_t *b)
{
    int32_t len_idx = DAQ_FFT_512, win_idx = DAQ_WIN_HANN;
    daq_settings_get_i32(DAQ_K_FFT_LENGTH, &len_idx);
    daq_settings_get_i32(DAQ_K_FFT_WINDOW, &win_idx);
    if (len_idx < 0 || len_idx >= DAQ_FFT_LEN_COUNT) len_idx = DAQ_FFT_512;
    spectrum_configure(&b->spectrum, DAQ_FFT_LENGTH_BINS[len_idx],
                       win_idx_to_hw(win_idx));
}

// ---------------------------------------------------------------------------
// Apply callback: one freshly-changed (or boot-seeded) value -> subsystem.
// C6-local keys (display/neopixel/wifi) have no P4 subsystem and are skipped
// here; they are forwarded to the C6 by the notify path (Phase 4).
// ---------------------------------------------------------------------------
static void on_apply(uint16_t key, int32_t ival, const char *sval, void *user)
{
    daq_board_t *b = (daq_board_t *)user;
    (void)sval;

    const daq_setting_schema_t *sc = daq_config_schema(key);
    if (sc && (sc->flags & (DAQ_F_C6_LOCAL | DAQ_F_S3_LOCAL))) return;   // applied on the C6 / S3

    switch (key) {
    case DAQ_K_AUTORANGING:
    case DAQ_K_RANGE_IDX:
        apply_ranging(b);
        break;

    case DAQ_K_SOURCE_ENABLE:
        smu_enable(&b->smu, ival != 0);
        break;
    case DAQ_K_DUT_VOLTAGE_MV:
        smu_set_voltage(&b->smu, (float)ival / 1000.0f);
        break;
    case DAQ_K_DUT_ILIMIT_MA:
        smu_set_current_limit(&b->smu, (float)ival / 1000.0f);
        break;

    case DAQ_K_FFT_ENABLE:
        spectrum_set_enabled(&b->spectrum, ival != 0);
        break;
    case DAQ_K_FFT_LENGTH:
    case DAQ_K_FFT_WINDOW:
        apply_spectrum(b);
        break;
    case DAQ_K_FFT_SOURCE:
        b->fft_source = (ival == DAQ_FFTSRC_POWER) ? 1 : 0;
        break;

    case DAQ_K_USB_DECIMATION:
        b->wave_decim = (uint8_t)((ival < 1) ? 1 : (ival > 255 ? 255 : ival));
        break;

    // TODO(phase-follow-up): these need a coordinated ADAQ ODR reprogram +
    // stream restart; applying power_dsp_set_rate alone would skew energy
    // integration, so they are store-only for now.
    case DAQ_K_SAMPLE_RATE_IDX:
    case DAQ_K_MULTIRES_TIERS:
    case DAQ_K_STATS_WINDOW_MS:
    case DAQ_K_STREAMING:
    case DAQ_K_DEVICE_LABEL:
        break;

    default:
        break;
    }
}

// ---------------------------------------------------------------------------
// Action callback: stateless one-shot operations.
// ---------------------------------------------------------------------------
static bool on_action(uint8_t action_id, void *user)
{
    daq_board_t *b = (daq_board_t *)user;
    switch (action_id) {
    case DAQ_ACT_ENERGY_RESET:
    case DAQ_ACT_CHARGE_RESET:
        // The DSP keeps a single energy/charge/time accumulator window; resetting
        // it clears both energy and charge.
        power_dsp_reset_energy(&b->dsp);
        return true;
    case DAQ_ACT_FACTORY_RESET:
        // Values were already reset to defaults + re-applied by the store; also
        // clear the accumulators for a clean slate.
        power_dsp_reset_energy(&b->dsp);
        power_dsp_reset_stats(&b->dsp);
        return true;
    default:
        return false;
    }
}

// ---------------------------------------------------------------------------
// notify callback: forward a change to the OTHER control plane(s). A change not
// originating from the C6 is pushed to the C6 (so its menu mirrors S3-side
// edits). S3-direction notification (IRQ poll) is added in Phase 7.
// ---------------------------------------------------------------------------
static void on_notify(uint16_t key, daq_src_t src, void *user)
{
    daq_board_t *b = (daq_board_t *)user;
    ESP_LOGD(TAG, "setting 0x%04X changed (src=%d)", key, (int)src);

    if (src == DAQ_SRC_C6) return;        // came from the C6; don't echo back
    if (!b->ddp.running) return;          // C6 link not up yet (e.g. boot apply)

    uint8_t tlv[DAQ_TLV_HDR_LEN + DAQ_TLV_MAX_VAL];
    int n = daq_settings_encode_one(key, tlv, sizeof(tlv));
    if (n > 0) ddp_master_config_push(&b->ddp, tlv, (uint8_t)n);
}

void daq_board_bind_settings(daq_board_t *b)
{
    daq_settings_init();
    daq_settings_set_callbacks(on_apply, on_notify, on_action, b);
    daq_settings_apply_all();

    // Safety: the DUT supply (V_DUT) must NEVER come up enabled on boot, even if
    // a previous session persisted SOURCE_ENABLE=1. It is gated on an explicit
    // command (CLI 'vdut on', or a host/C6 SOURCE_ENABLE write) only.
    daq_settings_set_i32(DAQ_K_SOURCE_ENABLE, 0, DAQ_SRC_BOOT);
    smu_enable(&b->smu, false);

    ESP_LOGI(TAG, "settings store bound + applied (V_DUT forced OFF)");
}
