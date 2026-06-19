#pragma once

// =============================================================================
// adaq7769.h — High-level HAL for the ADAQ7769-1 24-bit DAQ uModule
//
// Wraps adaq7769_ll to provide a register-free API: reset/identify, clock and
// power configuration, digital-filter + output-data-rate selection across every
// filter type and decimation rate the part offers, PGA gain control (via the
// part's own GPIO0..2 wired to GAIN0..2), AAF input scaling, reference-buffer
// selection, conversion-mode control, offset/gain calibration, diagnostics and
// sample-to-volts conversion.
//
// The fast DRDY-driven DMA capture path lives in adaq7769_stream.*; this HAL
// covers configuration and single/polled reads.
// =============================================================================

#include <stdint.h>
#include <stdbool.h>
#include "driver/gpio.h"
#include "esp_err.h"

#include "adaq7769_ll.h"
#include "adaq7769_regs.h"

#ifdef __cplusplus
extern "C" {
#endif

// Which AAF input pair the signal chain is wired to (fixes AAF_GAIN, hardware).
typedef enum {
    ADAQ_AAF_IN1 = 0,   // gain 1.000, max diff +/-4.096 V  @VREF=4.096
    ADAQ_AAF_IN2,       // gain 0.364, max diff +/-11.264 V
    ADAQ_AAF_IN3,       // gain 0.143, max diff +/-28.672 V (0..24 V unipolar)
} adaq_aaf_input_t;

// Shadow of the device configuration the HAL maintains.
typedef struct {
    uint8_t  clock_sel;      // ADAQ_CLKSEL_*
    uint8_t  mclk_div;       // ADAQ_MCLK_DIV_*
    uint8_t  adc_mode;       // ADAQ_ADC_MODE_*
    uint8_t  filter;         // ADAQ_FILTER_*
    uint8_t  dec_rate;       // ADAQ_DEC_*  (Sinc5 / wideband)
    uint32_t sinc3_dec;      // actual Sinc3 decimation (multiple of 32)
    bool     reject_50_60;   // Sinc3 50+60 Hz rejection
    uint8_t  conv_mode;      // ADAQ_CONVMODE_*
    uint8_t  pga_gain;       // 1,2,4,8,16,32,64,128
    bool     pga_enabled;
    uint8_t  ref_buf_pos;    // ADAQ_REFBUF_*
    uint8_t  ref_buf_neg;
    bool     lin_boost;      // linearity boost buffers enabled
    bool     cont_read;      // continuous read mode active
    bool     status_append;  // append 8-bit status header to samples
    bool     crc_append;     // append 8-bit CRC to samples
    bool     crc_xor;        // CRC uses XOR instead of poly (reads)
    bool     conv16;         // 16-bit conversion length (Sinc5 x8 path)
} adaq_config_t;

typedef struct {
    adaq_ll_t        ll;
    gpio_num_t       reset_pin;   // active-low hardware reset (or GPIO_NUM_NC)
    gpio_num_t       drdy_pin;    // DRDY input (or GPIO_NUM_NC)
    bool             is_sync_master;
    uint32_t         mclk_hz;     // shared MCLK frequency
    float            vref;        // reference voltage (V), nominal 4.096
    adaq_aaf_input_t aaf_input;   // fixes AAF_GAIN
    uint8_t          gpio_control_shadow;
    uint8_t          gpio_write_shadow;
    adaq_config_t    cfg;
    bool             present;
} adaq7769_t;

// Optional construction parameters; defaults are applied for any zero field.
typedef struct {
    spi_host_device_t host;
    gpio_num_t        cs_pin;
    gpio_num_t        reset_pin;
    gpio_num_t        drdy_pin;
    uint32_t          cfg_hz;        // 0 -> ADAQ_SCLK_CFG_HZ
    uint32_t          data_hz;       // 0 -> ADAQ_SCLK_DATA_HZ
    uint32_t          mclk_hz;       // 0 -> ADAQ_MCLK_HZ
    float             vref;          // 0 -> 4.096
    adaq_aaf_input_t  aaf_input;
    bool              is_sync_master;
} adaq7769_params_t;

// -----------------------------------------------------------------------------
// Lifecycle
// -----------------------------------------------------------------------------

/** @brief Attach the device to its (already bus-initialised) SPI host + GPIOs. */
esp_err_t adaq7769_attach(adaq7769_t *dev, const adaq7769_params_t *p);

/** @brief Hardware reset pulse on RESET pin (>=200us settle before SPI). */
esp_err_t adaq7769_hw_reset(adaq7769_t *dev);

/** @brief Software reset over SPI (SYNC_RESET 11 then 10). */
esp_err_t adaq7769_soft_reset(adaq7769_t *dev);

/** @brief Verify CHIP_TYPE/PRODUCT_ID/VENDOR_ID over SPI; sets dev->present. */
esp_err_t adaq7769_identify(adaq7769_t *dev);

/**
 * @brief Reset + identify + apply a sane default configuration:
 *   CMOS clock, MCLK_DIV=2 (fMOD=MCLK/2), fast power, wideband FIR dec x32,
 *   precharge ref buffers, linearity boost on, continuous conversion,
 *   PGA enabled at gain 1.
 */
esp_err_t adaq7769_begin(adaq7769_t *dev);

// -----------------------------------------------------------------------------
// Clock & power
// -----------------------------------------------------------------------------
esp_err_t adaq7769_set_clock_source(adaq7769_t *dev, uint8_t clock_sel);
esp_err_t adaq7769_set_mclk_div(adaq7769_t *dev, uint8_t mclk_div);
esp_err_t adaq7769_set_power_mode(adaq7769_t *dev, uint8_t adc_mode);
esp_err_t adaq7769_power_down(adaq7769_t *dev);
/** @brief Resume from power-down: 1 followed by 63 zeros on SDI (CS low). */
esp_err_t adaq7769_resume(adaq7769_t *dev);

// -----------------------------------------------------------------------------
// Digital filter & output data rate
// -----------------------------------------------------------------------------

/** @brief Select filter + decimation for Sinc5 / wideband paths. */
esp_err_t adaq7769_set_filter(adaq7769_t *dev, uint8_t filter, uint8_t dec_rate);

/** @brief Set Sinc3 filter with an explicit decimation (multiple of 32). */
esp_err_t adaq7769_set_sinc3(adaq7769_t *dev, uint32_t decimation, bool reject_50_60);

/** @brief Compute current output data rate (SPS) from clock + filter shadow. */
float adaq7769_output_data_rate(const adaq7769_t *dev);

/**
 * @brief Convenience: pick a filter + MCLK_DIV + decimation to hit (approx) the
 *        requested ODR. Prefers wideband FIR; falls back to Sinc5/Sinc3 for very
 *        high / very low rates. Returns the achieved ODR in *achieved (if set).
 */
esp_err_t adaq7769_set_output_data_rate(adaq7769_t *dev, float target_sps,
                                        float *achieved);

// -----------------------------------------------------------------------------
// Analog front-end
// -----------------------------------------------------------------------------

/** @brief Set PGA binary gain (1,2,4,8,16,32,64,128) via the part's GPIO0..2. */
esp_err_t adaq7769_set_pga_gain(adaq7769_t *dev, uint8_t gain);

/** @brief Enable/disable the PGA (EN_PGA via GPIO3). */
esp_err_t adaq7769_enable_pga(adaq7769_t *dev, bool enable);

/** @brief Record which AAF input is wired (affects scaling, not registers). */
void adaq7769_set_aaf_input(adaq7769_t *dev, adaq_aaf_input_t input);

/** @brief Configure reference buffers + linearity boost (ANALOG reg). */
esp_err_t adaq7769_set_reference(adaq7769_t *dev, uint8_t ref_buf_pos,
                                 uint8_t ref_buf_neg, bool linearity_boost);

// -----------------------------------------------------------------------------
// Conversion mode & readback
// -----------------------------------------------------------------------------
esp_err_t adaq7769_set_conv_mode(adaq7769_t *dev, uint8_t conv_mode);

/** @brief Enable/disable continuous-read mode and status/CRC appends. */
esp_err_t adaq7769_set_read_format(adaq7769_t *dev, bool continuous,
                                   bool append_status, bool append_crc,
                                   bool crc_xor, bool conv16);

/** @brief Single instructed read of ADC_DATA (24-bit, sign-extended). */
esp_err_t adaq7769_read_sample(adaq7769_t *dev, int32_t *raw);

/** @brief Convert a raw 24-bit code to volts using current gain + VREF. */
float adaq7769_code_to_volts(const adaq7769_t *dev, int32_t raw);

/** @brief Total signal-chain gain = PGA_GAIN * AAF_GAIN. */
float adaq7769_total_gain(const adaq7769_t *dev);

// -----------------------------------------------------------------------------
// Calibration (24-bit signed offset, ~0x555555 nominal gain)
// -----------------------------------------------------------------------------
esp_err_t adaq7769_set_offset_cal(adaq7769_t *dev, int32_t offset24);
esp_err_t adaq7769_get_offset_cal(adaq7769_t *dev, int32_t *offset24);
esp_err_t adaq7769_set_gain_cal(adaq7769_t *dev, uint32_t gain24);
esp_err_t adaq7769_get_gain_cal(adaq7769_t *dev, uint32_t *gain24);

// -----------------------------------------------------------------------------
// GPIO (the four ADAQ GPIO pins; on this board mostly used for PGA control)
// -----------------------------------------------------------------------------
esp_err_t adaq7769_gpio_config(adaq7769_t *dev, uint8_t output_mask,
                               uint8_t open_drain_mask);
esp_err_t adaq7769_gpio_write(adaq7769_t *dev, uint8_t level_mask);
esp_err_t adaq7769_gpio_read(adaq7769_t *dev, uint8_t *level_mask);

// -----------------------------------------------------------------------------
// Synchronisation
// -----------------------------------------------------------------------------

/** @brief Master only: pulse SYNC_OUT via SPI_START (resets all wired devices). */
esp_err_t adaq7769_sync_pulse(adaq7769_t *dev);

/** @brief Enable the GPIO3 START input path (asynchronous external trigger). */
esp_err_t adaq7769_enable_gpio_start(adaq7769_t *dev, bool enable);

// -----------------------------------------------------------------------------
// Diagnostics
// -----------------------------------------------------------------------------
esp_err_t adaq7769_read_status(adaq7769_t *dev, uint8_t *master_status);
esp_err_t adaq7769_read_spi_errors(adaq7769_t *dev, uint8_t *spi_status);
esp_err_t adaq7769_clear_spi_errors(adaq7769_t *dev, uint8_t mask);
esp_err_t adaq7769_enable_diagnostics(adaq7769_t *dev, uint8_t adc_diag_mask,
                                      uint8_t dig_diag_mask);

/**
 * @brief Route an internal diagnostic source (temp / short / +FS / -FS) to the
 *        ADC and take one reading. Requires low-power mode + MCLK/16 per the
 *        datasheet; the caller is responsible for restoring normal conversion.
 */
esp_err_t adaq7769_read_diagnostic(adaq7769_t *dev, uint8_t diag_mux, int32_t *raw);

#ifdef __cplusplus
}
#endif
