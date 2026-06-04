// =============================================================================
// adc_dsp.cpp — ADC DSP pipeline implementation
// See adc_dsp.h for threading model and API contract.
// =============================================================================

#include "adc_dsp.h"
#include "esp_log.h"
#include "dsps_fft2r.h"
#include "dsps_wind.h"
#include <math.h>
#include <string.h>
#include <stdlib.h>

static const char *TAG = "adc_dsp";

// ---------------------------------------------------------------------------
// Ping-pong sample buffers (ADC task fills one, DSP task reads the other)
// 2 × 256 × 4 bytes = 2 KB total — keeps them in fast internal RAM.
// ---------------------------------------------------------------------------
static float s_samples[2][ADC_DSP_WINDOW_SIZE];

// Per-buffer running statistics (accumulated by the ADC task while filling).
struct BufStats {
    float    sum, sum_sq;
    float    min_v, max_v;
    uint32_t window_start_us;
};
static BufStats s_stats[2];

// Active buffer index: written only by the ADC poll task after window flip.
static volatile uint8_t  s_active_buf = 0;
static volatile uint16_t s_pos        = 0;

// Index of the buffer most recently completed (valid after push returns true).
static uint8_t s_last_completed = 0;

// ---------------------------------------------------------------------------
// FFT workspace (heap-allocated only when FFT is requested)
// ---------------------------------------------------------------------------
static float *s_fft_buf  = nullptr;  // ADC_DSP_WINDOW_SIZE * 2 floats (complex)
static float *s_hann_win = nullptr;  // ADC_DSP_WINDOW_SIZE floats (Hann window)

static AdcDspConfig s_cfg      = {};
static bool         s_init     = false;
static bool         s_fft_init = false;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

// Approximate sample-to-sample interval in µs for each AdcRate code.
static uint32_t rate_to_interval_us(uint8_t rate_code)
{
    switch (rate_code) {
        case 0:  return 100000u;  // ADC_RATE_10SPS_H
        case 1:  return  50000u;  // ADC_RATE_20SPS
        case 3:  return  50000u;  // ADC_RATE_20SPS_H
        case 4:  return   5000u;  // ADC_RATE_200SPS_H1
        case 6:  return   5000u;  // ADC_RATE_200SPS_H
        case 8:  return    833u;  // ADC_RATE_1_2KSPS
        case 9:  return    833u;  // ADC_RATE_1_2KSPS_H
        case 12: return    208u;  // ADC_RATE_4_8KSPS
        case 13: return    104u;  // ADC_RATE_9_6KSPS
        default: return   5000u;
    }
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

esp_err_t adc_dsp_init(const AdcDspConfig *cfg)
{
    if (!cfg) return ESP_ERR_INVALID_ARG;

    adc_dsp_deinit();

    s_cfg        = *cfg;
    s_active_buf = 0;
    s_pos        = 0;
    s_last_completed = 0;

    for (int b = 0; b < 2; b++) {
        s_stats[b] = { 0.0f, 0.0f, 1e30f, -1e30f, 0u };
    }

    if (cfg->n_fft_peaks > 0) {
        s_fft_buf = static_cast<float *>(
            malloc(ADC_DSP_WINDOW_SIZE * 2 * sizeof(float)));
        if (!s_fft_buf) return ESP_ERR_NO_MEM;

        s_hann_win = static_cast<float *>(
            malloc(ADC_DSP_WINDOW_SIZE * sizeof(float)));
        if (!s_hann_win) {
            free(s_fft_buf); s_fft_buf = nullptr;
            return ESP_ERR_NO_MEM;
        }

        dsps_wind_hann_f32(s_hann_win, ADC_DSP_WINDOW_SIZE);

        esp_err_t ret = dsps_fft2r_init_fc32(nullptr, ADC_DSP_WINDOW_SIZE);
        if (ret != ESP_OK) {
            free(s_fft_buf);  s_fft_buf  = nullptr;
            free(s_hann_win); s_hann_win = nullptr;
            ESP_LOGE(TAG, "FFT init failed: %d", ret);
            return ret;
        }
        s_fft_init = true;
    }

    s_init = true;
    ESP_LOGI(TAG, "init ch=%d rate=%d fft_peaks=%d thr=%.4f",
             cfg->channel, cfg->rate_code, cfg->n_fft_peaks, cfg->spike_threshold);
    return ESP_OK;
}

void adc_dsp_deinit(void)
{
    s_init     = false;
    s_fft_init = false;
    s_pos      = 0;
    if (s_fft_buf)  { free(s_fft_buf);  s_fft_buf  = nullptr; }
    if (s_hann_win) { free(s_hann_win); s_hann_win = nullptr; }
}

bool adc_dsp_push_sample(float voltage, uint32_t timestamp_us)
{
    if (!s_init) return false;

    uint8_t  buf = s_active_buf;
    uint16_t pos = s_pos;

    if (pos == 0) s_stats[buf].window_start_us = timestamp_us;

    s_samples[buf][pos] = voltage;

    BufStats &st = s_stats[buf];
    if (voltage < st.min_v) st.min_v = voltage;
    if (voltage > st.max_v) st.max_v = voltage;
    st.sum    += voltage;
    st.sum_sq += voltage * voltage;

    pos++;
    if (pos >= ADC_DSP_WINDOW_SIZE) {
        // Window complete: save completed buffer index, flip to other buffer
        s_last_completed = buf;
        uint8_t next = buf ^ 1u;
        s_stats[next] = { 0.0f, 0.0f, 1e30f, -1e30f, 0u };
        s_active_buf = next;  // single byte write — atomic on Xtensa
        s_pos = 0;
        return true;
    }

    s_pos = pos;
    return false;
}

uint8_t adc_dsp_last_completed_buf(void)
{
    return s_last_completed;
}

void adc_dsp_process(uint8_t buf_idx, AdcDspWindow *out)
{
    if (!out) return;
    buf_idx &= 1u;

    const float    *samples = s_samples[buf_idx];
    const BufStats &st      = s_stats[buf_idx];
    const uint16_t  n       = ADC_DSP_WINDOW_SIZE;

    float mean = st.sum    / (float)n;
    float rms  = sqrtf(st.sum_sq / (float)n);

    out->channel         = s_cfg.channel;
    out->window_start_us = st.window_start_us;
    out->n_samples       = n;
    out->min_v           = st.min_v;
    out->max_v           = st.max_v;
    out->mean_v          = mean;
    out->rms_v           = rms;
    out->n_fft_peaks     = 0;
    out->n_spikes        = 0;

    // ---- Spike detection ---------------------------------------------------
    float thresh = s_cfg.spike_threshold;
    if (thresh > 0.0f) {
        uint32_t interval_us = rate_to_interval_us(s_cfg.rate_code);
        for (uint16_t i = 0; i < n && out->n_spikes < ADC_DSP_MAX_SPIKES; i++) {
            if (fabsf(samples[i] - mean) > thresh) {
                out->spikes[out->n_spikes].offset_us = (uint32_t)i * interval_us;
                out->spikes[out->n_spikes].value     = samples[i];
                out->n_spikes++;
            }
        }
    }

    // ---- FFT ---------------------------------------------------------------
    if (!s_fft_init || s_cfg.n_fft_peaks == 0 || !s_fft_buf || !s_hann_win) return;

    // Pack real samples into interleaved complex buffer with Hann window
    for (int i = 0; i < ADC_DSP_WINDOW_SIZE; i++) {
        s_fft_buf[2*i]   = samples[i] * s_hann_win[i];
        s_fft_buf[2*i+1] = 0.0f;
    }

    dsps_fft2r_fc32(s_fft_buf, ADC_DSP_WINDOW_SIZE);
    dsps_bit_rev_fc32(s_fft_buf, ADC_DSP_WINDOW_SIZE);

    // Find top N magnitude bins (skip DC at bin 0)
    uint8_t want = s_cfg.n_fft_peaks;
    if (want > ADC_DSP_MAX_FFT_PEAKS) want = ADC_DSP_MAX_FFT_PEAKS;

    // Maintain a descending-sorted list of (magnitude, bin) pairs
    float   top_mag[ADC_DSP_MAX_FFT_PEAKS] = {};
    uint8_t top_bin[ADC_DSP_MAX_FFT_PEAKS] = {};
    uint8_t found = 0;
    const float norm = 2.0f / (float)ADC_DSP_WINDOW_SIZE;

    for (int k = 1; k < ADC_DSP_WINDOW_SIZE / 2; k++) {
        float re  = s_fft_buf[2*k];
        float im  = s_fft_buf[2*k+1];
        float mag = sqrtf(re*re + im*im) * norm;

        if (found < want) {
            top_mag[found] = mag;
            top_bin[found] = (uint8_t)k;
            found++;
            // Bubble up to maintain descending order
            for (int j = (int)found-1; j > 0 && top_mag[j] > top_mag[j-1]; j--) {
                float   tm = top_mag[j-1]; top_mag[j-1] = top_mag[j]; top_mag[j] = tm;
                uint8_t tb = top_bin[j-1]; top_bin[j-1] = top_bin[j]; top_bin[j] = tb;
            }
        } else if (mag > top_mag[found-1]) {
            top_mag[found-1] = mag;
            top_bin[found-1] = (uint8_t)k;
            for (int j = (int)found-1; j > 0 && top_mag[j] > top_mag[j-1]; j--) {
                float   tm = top_mag[j-1]; top_mag[j-1] = top_mag[j]; top_mag[j] = tm;
                uint8_t tb = top_bin[j-1]; top_bin[j-1] = top_bin[j]; top_bin[j] = tb;
            }
        }
    }

    out->n_fft_peaks = found;
    for (uint8_t i = 0; i < found; i++) {
        out->fft_peaks[i].bin       = top_bin[i];
        out->fft_peaks[i].magnitude = top_mag[i];
    }
}

const AdcDspConfig *adc_dsp_get_config(void)
{
    return s_init ? &s_cfg : nullptr;
}
