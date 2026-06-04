#pragma once
// =============================================================================
// adc_dsp.h — On-device DSP pipeline for high-rate ADC streaming
//
// Accumulates single-channel ADC samples into fixed-size windows, applies a
// Hann window + 256-point FFT (top-N dominant bins), computes running
// statistics (min/max/mean/RMS), and detects individual voltage spikes above
// a configurable threshold.
//
// Threading model:
//   adc_dsp_push_sample() is called from the ADC poll task (Core 1).
//   adc_dsp_process()     is called from the DSP task     (Core 0).
//   A ping-pong buffer separates the two; xTaskNotify carries the completed
//   buffer index as the synchronisation barrier.
// =============================================================================

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ADC_DSP_WINDOW_SIZE    256
#define ADC_DSP_MAX_SPIKES     16
#define ADC_DSP_MAX_FFT_PEAKS  16

typedef struct {
    uint8_t  channel;          // logical AD74416H channel (0–3)
    uint8_t  rate_code;        // AdcRate enum value (for spike offset calculation)
    uint16_t window_samples;   // must equal ADC_DSP_WINDOW_SIZE
    float    spike_threshold;  // |v - mean| > threshold → emit spike record
    uint8_t  n_fft_peaks;      // 0 = skip FFT; 1–16 = dominant bins to report
} AdcDspConfig;

typedef struct {
    uint32_t offset_us;  // sample time offset from window_start_us
    float    value;      // voltage at spike sample
} AdcDspSpike;

typedef struct {
    uint8_t bin;         // FFT bin 0–127; freq = bin × sample_rate_hz / window_size
    float   magnitude;   // linear magnitude (V, normalised to window)
} AdcDspFftPeak;

typedef struct {
    uint8_t       channel;
    uint32_t      window_start_us;
    uint16_t      n_samples;
    float         min_v, max_v, mean_v, rms_v;
    uint8_t       n_fft_peaks;
    AdcDspFftPeak fft_peaks[ADC_DSP_MAX_FFT_PEAKS];
    uint8_t       n_spikes;
    AdcDspSpike   spikes[ADC_DSP_MAX_SPIKES];
} AdcDspWindow;

/**
 * @brief Initialise the DSP pipeline. Heap-allocates FFT workspace if FFT
 *        is requested. Safe to call again after adc_dsp_deinit().
 */
esp_err_t adc_dsp_init(const AdcDspConfig *cfg);

/** @brief Release all DSP heap allocations. Safe to call when not init'd. */
void adc_dsp_deinit(void);

/**
 * @brief Push one voltage sample from the ADC poll task (Core 1).
 * @return true when a 256-sample window is complete; the completed buffer
 *         index is then available via adc_dsp_last_completed_buf().
 *         The caller should immediately notify the DSP task.
 */
bool adc_dsp_push_sample(float voltage, uint32_t timestamp_us);

/** @brief Index (0 or 1) of the last completed ping-pong buffer.
 *         Valid only for one call after adc_dsp_push_sample() returns true. */
uint8_t adc_dsp_last_completed_buf(void);

/**
 * @brief Process the completed window (FFT + stats + spikes). Called from
 *        the DSP task. buf_idx must match the value from
 *        adc_dsp_last_completed_buf() delivered via task notification.
 */
void adc_dsp_process(uint8_t buf_idx, AdcDspWindow *out);

/** @brief Active config pointer (valid between init and deinit). */
const AdcDspConfig *adc_dsp_get_config(void);

#ifdef __cplusplus
}
#endif
