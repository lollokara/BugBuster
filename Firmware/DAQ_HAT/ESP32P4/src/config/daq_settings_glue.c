// =============================================================================
// daq_settings_glue.c — translate settings-store changes into P4 subsystem calls.
// =============================================================================

#include "daq_settings_glue.h"
#include "daq_settings.h"
#include "daq_config_registry.h"
#include "adaq7769.h"
#include "adaq7769_regs.h"

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
        range_manager_force(&b->range, RANGE_UNKNOWN);   // release -> firmware autorange
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

// Apply the current Filter / Decimation / 50-60 reject settings to the two
// current ADAQs (FINE + COARSE; VOLTAGE unchanged). ADAQ config registers are
// inaccessible during continuous-read capture, so bracket with a fast-acq
// pause/resume, mirroring the TUI 'f'/'d'/'r' hotkeys.
static void apply_adaq_filter(daq_board_t *b)
{
    int32_t f = DAQ_FILT_WIDEBAND, d = DAQ_DEC_256, rej = 0;
    daq_settings_get_i32(DAQ_K_FILTER, &f);
    daq_settings_get_i32(DAQ_K_DECIMATION, &d);
    daq_settings_get_i32(DAQ_K_REJECT_5060, &rej);
    if (d < 0) d = 0;
    if (d > ADAQ_DEC_X1024) d = ADAQ_DEC_X1024;
    uint8_t filt = (f == DAQ_FILT_SINC5) ? ADAQ_FILTER_SINC5 :
                   (f == DAQ_FILT_SINC3) ? ADAQ_FILTER_SINC3 : ADAQ_FILTER_WIDEBAND;

    bool was = b->fast_running;
    if (was) daq_board_stop_fast(b);
    for (int i = 0; i <= 1; ++i) {           // FINE + COARSE current ADAQs
        if (!b->adaq_ok[i]) continue;
        if (filt == ADAQ_FILTER_SINC3) {
            uint32_t dec = 32u << (uint8_t)d;   // x32..x1024
            adaq7769_set_sinc3(&b->adaq[i], dec, rej != 0);
        } else {
            adaq7769_set_filter(&b->adaq[i], filt, (uint8_t)d);
        }
    }
    if (was) daq_board_run_fast(b, DAQ_RING_CAPACITY);
}

// Apply the top-level Sample Rate: pick the ADAQ filter+decimation that hits the
// target SPS on the two current ADAQs, sync the DSP integration rate to the new
// FINE ODR, and reset the energy accumulator so the rate change doesn't skew
// the running energy/charge/time. Pause/resume the fast acquisition around it.
static void apply_sample_rate(daq_board_t *b)
{
    int32_t idx = DAQ_SR_100K;
    daq_settings_get_i32(DAQ_K_SAMPLE_RATE_IDX, &idx);
    if (idx < 0) idx = 0;
    if (idx >= DAQ_SR_COUNT) idx = DAQ_SR_COUNT - 1;
    float target = (float)DAQ_SAMPLE_RATE_SPS[idx];

    bool was = b->fast_running;
    if (was) daq_board_stop_fast(b);
    float achieved = target;
    for (int i = 0; i <= 1; ++i) {           // FINE + COARSE current ADAQs
        if (!b->adaq_ok[i]) continue;
        float a = target;
        if (adaq7769_set_output_data_rate(&b->adaq[i], target, &a) == ESP_OK && i == 0)
            achieved = a;
    }
    power_dsp_set_rate(&b->dsp, achieved);
    power_dsp_reset_energy(&b->dsp);
    if (was) daq_board_run_fast(b, DAQ_RING_CAPACITY);
}
// Integer decimation from a measured ADC ODR down to a target output rate,
// clamped to what sr_filter can build. Rounds to nearest so a slightly
// off-nominal ODR still selects the intended factor.
static uint16_t sr_decim_for(float odr, uint32_t target_sps)
{
    if (odr <= 0.0f || target_sps == 0) return 1;
    int d = (int)(odr / (float)target_sps + 0.5f);
    if (d < 1) d = 1;
    if (d > SR_FILTER_MAX_DECIM) d = SR_FILTER_MAX_DECIM;
    return (uint16_t)d;
}

// Apply Super-Resolution mode.
//
// ON:  put the two CURRENT ADAQs on Sinc3 at DAQ_SR_ADC_DECIM — the narrowest
//      noise bandwidth the part offers — then design the stage-2 FIR decimators
//      that take the resulting ODRs down to DAQ_SR_CURRENT_SPS /
//      DAQ_SR_VOLTAGE_SPS. The decimation factors are derived from the ODR each
//      ADC actually reports rather than assumed, so a different MCLK or MCLK
//      divider still lands on the advertised output rates.
// OFF: restore whatever Filter/Decimation/Sample Rate the store holds.
//
// VOLTAGE (U23) is deliberately NOT reprogrammed. It shares SPI bus B and one
// common SYNC line with COARSE (U22) and cannot be phase-staggered, so giving it
// the same ODR as COARSE makes both assert DRDY on the same edge every sample
// and the voltage channel starves — measured as zero WAVE_V samples. It keeps
// VOLTAGE_ODR_TARGET_SPS and the FIR decimates from there instead. Same ruling
// as CTRL_MSG_SET_ACQ_CONFIG in daq_board.c.
//
// ADAQ config registers are inaccessible during continuous-read capture, so
// this is bracketed by a fast-acquisition pause/resume like apply_adaq_filter().
static void apply_sr_mode(daq_board_t *b)
{
    int32_t on = 0;
    daq_settings_get_i32(DAQ_K_SR_MODE, &on);

    bool was = b->fast_running;
    if (was) daq_board_stop_fast(b);

    if (!on) {
        b->sr_mode = false;
        if (was) daq_board_run_fast(b, DAQ_RING_CAPACITY);
        // Restoring the operator's filter/rate needs the fast path stopped too,
        // and both helpers bracket themselves, so run them after the resume.
        apply_adaq_filter(b);
        apply_sample_rate(b);
        return;
    }

    int32_t rej = 0;
    daq_settings_get_i32(DAQ_K_REJECT_5060, &rej);

    for (int i = ADAQ_ROLE_FINE; i <= ADAQ_ROLE_COARSE; ++i) {
        if (!b->adaq_ok[i]) continue;
        adaq7769_set_sinc3(&b->adaq[i], DAQ_SR_ADC_DECIM, rej != 0);
    }

    // Re-assert VOLTAGE's independent target rate. FINE is the SYNC master, so
    // the pulse fired above reset every device's counter over the shared SYNC
    // line; VOLTAGE's config registers are untouched by that, but re-applying
    // is cheap insurance.
    if (b->adaq_ok[ADAQ_ROLE_VOLTAGE]) {
        float v_ach = 0.0f;
        adaq7769_set_output_data_rate(&b->adaq[ADAQ_ROLE_VOLTAGE],
                                      VOLTAGE_ODR_TARGET_SPS, &v_ach);
    }

    float i_odr = b->adaq_ok[ADAQ_ROLE_FINE]
                      ? adaq7769_output_data_rate(&b->adaq[ADAQ_ROLE_FINE]) : 0.0f;
    float v_odr = b->adaq_ok[ADAQ_ROLE_VOLTAGE]
                      ? adaq7769_output_data_rate(&b->adaq[ADAQ_ROLE_VOLTAGE]) : 0.0f;

    uint16_t i_dec = sr_decim_for(i_odr, DAQ_SR_CURRENT_SPS);
    uint16_t v_dec = sr_decim_for(v_odr, DAQ_SR_VOLTAGE_SPS);
    sr_filter_init(&b->sr_i, i_dec);
    sr_filter_init(&b->sr_v, v_dec);
    sr_filter_reset(&b->sr_i);
    sr_filter_reset(&b->sr_v);
    b->sr_mode = true;

    // The DSP tail integrates energy/charge off the pre-SR fused stream, so it
    // still has to track the raw ODR, not the SR output rate.
    power_dsp_set_rate(&b->dsp, i_odr > 0.0f ? i_odr : 1.0f);
    power_dsp_reset_energy(&b->dsp);

    ESP_LOGI(TAG, "SR mode ON: adc %.0f/%.0f sps -> decim %u/%u -> %u/%u sps (I/V)",
             i_odr, v_odr, (unsigned)i_dec, (unsigned)v_dec,
             (unsigned)DAQ_SR_CURRENT_SPS, (unsigned)DAQ_SR_VOLTAGE_SPS);

    if (was) daq_board_run_fast(b, DAQ_RING_CAPACITY);
}

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
        // Hard guard (no override): the DUT output may only be enabled with a
        // USB-PD contract of at least 9 V / 3 A negotiated on the S3.
        if (ival != 0 && !daq_board_pd_ok(b, 9000, 3000)) {
            ESP_LOGW(TAG, "DUT enable BLOCKED: USB-PD contract < 9 V / 3 A");
            smu_enable(&b->smu, false);
        } else {
            smu_enable(&b->smu, ival != 0);
        }
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

    case DAQ_K_FILTER:
    case DAQ_K_DECIMATION:
        if (b->sr_mode) break;          // SR owns the ADAQ filter configuration
        apply_adaq_filter(b);
        break;
    case DAQ_K_REJECT_5060:
        // 50/60 Hz rejection is a Sinc3 option, so it stays meaningful under SR.
        if (b->sr_mode) apply_sr_mode(b);
        else            apply_adaq_filter(b);
        break;

    case DAQ_K_SR_MODE:
        apply_sr_mode(b);
        break;

    case DAQ_K_SAMPLE_RATE_IDX:
        if (b->sr_mode) break;          // SR pins the ODR to its own choice
        apply_sample_rate(b);
        break;

    // TODO(phase-follow-up): these need a coordinated ADAQ ODR reprogram +
    // stream restart; applying power_dsp_set_rate alone would skew energy
    // integration, so they are store-only for now.
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
