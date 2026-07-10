// =============================================================================
// adaq7769.c — High-level HAL for the ADAQ7769-1
// =============================================================================

#include "adaq7769.h"
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_rom_sys.h"
#include "esp_log.h"
#include "config.h"

static const char *TAG = "adaq7769";

// -----------------------------------------------------------------------------
// Small lookup helpers
// -----------------------------------------------------------------------------

// MCLK divider enum -> integer divisor.
static uint32_t mclk_div_value(uint8_t div_sel)
{
    switch (div_sel) {
        case ADAQ_MCLK_DIV_2:  return 2;
        case ADAQ_MCLK_DIV_4:  return 4;
        case ADAQ_MCLK_DIV_8:  return 8;
        default:               return 16;  // ADAQ_MCLK_DIV_16
    }
}

// DEC_RATE enum -> decimation factor (Sinc5 / wideband).
static uint32_t dec_rate_value(uint8_t dec_sel)
{
    switch (dec_sel & 7) {
        case ADAQ_DEC_X32:  return 32;
        case ADAQ_DEC_X64:  return 64;
        case ADAQ_DEC_X128: return 128;
        case ADAQ_DEC_X256: return 256;
        case ADAQ_DEC_X512: return 512;
        default:            return 1024;   // x1024 (5,6,7)
    }
}

// PGA gain (1..128) -> 3-bit GAIN[2:0] code (= log2(gain)).
static bool pga_gain_to_code(uint8_t gain, uint8_t *code)
{
    switch (gain) {
        case 1:   *code = 0; return true;
        case 2:   *code = 1; return true;
        case 4:   *code = 2; return true;
        case 8:   *code = 3; return true;
        case 16:  *code = 4; return true;
        case 32:  *code = 5; return true;
        case 64:  *code = 6; return true;
        case 128: *code = 7; return true;
        default:  return false;
    }
}

// AAF input -> fixed AAF_GAIN.
static float aaf_gain_value(adaq_aaf_input_t input)
{
    switch (input) {
        case ADAQ_AAF_IN2: return 0.364f;
        case ADAQ_AAF_IN3: return 0.143f;
        default:           return 1.0f;   // IN1
    }
}

// Rewrite POWER_CLOCK from the shadow.
static esp_err_t write_power_clock(adaq7769_t *dev)
{
    uint8_t v = (uint8_t)((dev->cfg.clock_sel << ADAQ_PC_CLOCK_SEL_SHIFT) |
                          (dev->cfg.mclk_div  << ADAQ_PC_MCLK_DIV_SHIFT)  |
                          (dev->cfg.adc_mode  << ADAQ_PC_ADC_MODE_SHIFT));
    return adaq_ll_write_reg(&dev->ll, ADAQ_REG_POWER_CLOCK, v);
}

// Rewrite DIGITAL_FILTER from the shadow.
static esp_err_t write_digital_filter(adaq7769_t *dev)
{
    uint8_t v = (uint8_t)((dev->cfg.filter   << ADAQ_DF_FILTER_SHIFT) |
                          (dev->cfg.dec_rate  << ADAQ_DF_DEC_RATE_SHIFT));
    if (dev->cfg.reject_50_60) {
        v |= ADAQ_DF_EN_60HZ_REJ;
    }
    return adaq_ll_write_reg(&dev->ll, ADAQ_REG_DIGITAL_FILTER, v);
}

// Rewrite ANALOG from the shadow.
static esp_err_t write_analog(adaq7769_t *dev)
{
    uint8_t v = (uint8_t)((dev->cfg.ref_buf_pos << ADAQ_AN_REF_BUF_POS_SHIFT) |
                          (dev->cfg.ref_buf_neg << ADAQ_AN_REF_BUF_NEG_SHIFT));
    if (!dev->cfg.lin_boost) {
        v |= ADAQ_AN_LIN_BOOST_A_OFF | ADAQ_AN_LIN_BOOST_B_OFF;
    }
    return adaq_ll_write_reg(&dev->ll, ADAQ_REG_ANALOG, v);
}

// Rewrite INTERFACE_FORMAT from the shadow.
static esp_err_t write_interface_format(adaq7769_t *dev)
{
    uint8_t v = 0;
    if (dev->cfg.status_append) v |= ADAQ_IF_STATUS_EN;
    if (dev->cfg.crc_append)    v |= ADAQ_IF_EN_SPI_CRC;
    if (dev->cfg.crc_xor)       v |= ADAQ_IF_CRC_TYPE_XOR;
    if (dev->cfg.conv16)        v |= ADAQ_IF_CONVLEN_16;
    if (dev->cfg.cont_read)     v |= ADAQ_IF_EN_CONT_READ;
    esp_err_t err = adaq_ll_write_reg(&dev->ll, ADAQ_REG_INTERFACE_FORMAT, v);
    if (err == ESP_OK) {
        adaq_ll_set_crc(&dev->ll, dev->cfg.crc_append, dev->cfg.crc_xor);
    }
    return err;
}

// -----------------------------------------------------------------------------
// Lifecycle
// -----------------------------------------------------------------------------
esp_err_t adaq7769_attach(adaq7769_t *dev, const adaq7769_params_t *p)
{
    memset(dev, 0, sizeof(*dev));
    dev->reset_pin      = p->reset_pin;
    dev->drdy_pin       = p->drdy_pin;
    dev->is_sync_master = p->is_sync_master;
    dev->mclk_hz        = p->mclk_hz ? p->mclk_hz : ADAQ_MCLK_HZ;
    dev->vref           = (p->vref > 0.0f) ? p->vref : 4.096f;
    dev->aaf_input      = p->aaf_input;

    esp_err_t err = adaq_ll_add_device(&dev->ll, p->host, p->cs_pin,
                                       p->cfg_hz  ? p->cfg_hz  : ADAQ_SCLK_CFG_HZ,
                                       p->data_hz ? p->data_hz : ADAQ_SCLK_DATA_HZ);
    if (err != ESP_OK) {
        return err;
    }

    if (dev->reset_pin != GPIO_NUM_NC) {
        gpio_config_t io = {
            .pin_bit_mask = 1ULL << dev->reset_pin,
            .mode         = GPIO_MODE_OUTPUT,
        };
        gpio_config(&io);
        gpio_set_level(dev->reset_pin, 1);   // active low, idle high
    }
    if (dev->drdy_pin != GPIO_NUM_NC) {
        gpio_config_t io = {
            .pin_bit_mask = 1ULL << dev->drdy_pin,
            .mode         = GPIO_MODE_INPUT,
        };
        gpio_config(&io);
    }
    return ESP_OK;
}

esp_err_t adaq7769_hw_reset(adaq7769_t *dev)
{
    if (dev->reset_pin == GPIO_NUM_NC) {
        return adaq7769_soft_reset(dev);
    }
    gpio_set_level(dev->reset_pin, 0);
    esp_rom_delay_us(10);
    gpio_set_level(dev->reset_pin, 1);
    // Datasheet: time from RESET to first SPI write must be >= 200 us.
    esp_rom_delay_us(300);
    return ESP_OK;
}

esp_err_t adaq7769_soft_reset(adaq7769_t *dev)
{
    esp_err_t err = adaq_ll_write_reg(&dev->ll, ADAQ_REG_SYNC_RESET,
                                      (0x80u | ADAQ_SPI_RESET_ARM));
    if (err != ESP_OK) return err;
    err = adaq_ll_write_reg(&dev->ll, ADAQ_REG_SYNC_RESET,
                            (0x80u | ADAQ_SPI_RESET_FIRE));
    esp_rom_delay_us(300);
    return err;
}

esp_err_t adaq7769_identify(adaq7769_t *dev)
{
    uint8_t chip = 0, pid_l = 0, pid_h = 0, ven_l = 0, ven_h = 0;

    // ESP32-P4 SPI reads drop the final bit at the ADAQ interface-reset edge, so
    // a plain 8-bit register read loses its LSB (CHIP_TYPE 0x07 -> 0x06, and the
    // device reads as absent). Enabling EN_SPI_CRC makes the ADAQ drive a CRC
    // byte AFTER the data byte, so the data byte is no longer the final driven
    // bit and is captured intact. The device clears this on reset (POR) and
    // identify always runs post-reset, so (re)enable it here first. The enable
    // frame is a plain write (CRC still off); afterwards every register frame
    // carries a trailing CRC byte.
    adaq_ll_set_crc(&dev->ll, false, false);
    adaq_ll_write_reg(&dev->ll, ADAQ_REG_INTERFACE_FORMAT, ADAQ_IF_EN_SPI_CRC);
    adaq_ll_set_crc(&dev->ll, true, false);

    esp_err_t err = adaq_ll_read_reg(&dev->ll, ADAQ_REG_CHIP_TYPE, &chip);
    if (err != ESP_OK) return err;
    adaq_ll_read_reg(&dev->ll, ADAQ_REG_PRODUCT_ID_L, &pid_l);
    adaq_ll_read_reg(&dev->ll, ADAQ_REG_PRODUCT_ID_H, &pid_h);
    adaq_ll_read_reg(&dev->ll, ADAQ_REG_VENDOR_L, &ven_l);
    adaq_ll_read_reg(&dev->ll, ADAQ_REG_VENDOR_H, &ven_h);

    uint16_t vendor = (uint16_t)((ven_h << 8) | ven_l);
    bool ok = ((chip & 0x0F) == ADAQ_CHIP_TYPE_CLASS_ADC) &&
              (pid_l == ADAQ_PRODUCT_ID_LSB) &&
              (pid_h == ADAQ_PRODUCT_ID_MSB) &&
              (vendor == ADAQ_VENDOR_ID);
    dev->present = ok;
    if (!ok) {
        ESP_LOGW(TAG, "identify mismatch: chip=0x%02X pid=%02X%02X vendor=0x%04X",
                 chip, pid_h, pid_l, vendor);
        return ESP_ERR_NOT_FOUND;
    }
    return ESP_OK;
}

esp_err_t adaq7769_begin(adaq7769_t *dev)
{
    esp_err_t err = adaq7769_hw_reset(dev);
    if (err != ESP_OK) return err;

    err = adaq7769_identify(dev);
    if (err != ESP_OK) return err;

    // Default config shadow.
    dev->cfg.clock_sel   = ADAQ_CLKSEL_CMOS;
    dev->cfg.mclk_div    = ADAQ_MCLK_DIV_2;     // fMOD = MCLK/2 = 8.192 MHz
    dev->cfg.adc_mode    = ADAQ_ADC_MODE_FAST;
    dev->cfg.filter      = ADAQ_FILTER_WIDEBAND;
    // Default ODR = the measured end-to-end LOSSLESS ceiling for the current
    // pipeline (per-sample GPIO-ISR capture + FINE/COARSE sequence-fusion + DSP
    // + USB). At 8 kSPS/channel the whole chain keeps up with ZERO drops
    // (verified: BusA 7994/7996, BusB 15994/15992, 0 overflow). 16 kSPS already
    // overflows the consumer. Raise at runtime with `odr` once the fusion/read
    // fast-path work lifts the ceiling.
    dev->cfg.dec_rate    = ADAQ_DEC_X1024;      // 8 kSPS @ fMOD/1024
    dev->cfg.sinc3_dec   = 0;
    dev->cfg.reject_50_60 = false;
    dev->cfg.conv_mode   = ADAQ_CONVMODE_CONTINUOUS;
    dev->cfg.pga_gain    = 1;
    dev->cfg.pga_enabled = true;
    dev->cfg.ref_buf_pos = ADAQ_REFBUF_PRECHARGE;
    dev->cfg.ref_buf_neg = ADAQ_REFBUF_PRECHARGE;
    dev->cfg.lin_boost   = true;
    dev->cfg.cont_read   = false;
    dev->cfg.status_append = false;
    // Keep EN_SPI_CRC on for register access: the trailing CRC byte is what lets
    // the ESP32-P4 capture each register's LSB (identify enabled it above). The
    // high-speed sample stream uses continuous-read mode, where the datasheet
    // holds the LSB until the next DRDY, so adaq_stream sets its own format.
    dev->cfg.crc_append  = true;
    dev->cfg.crc_xor     = false;
    dev->cfg.conv16      = false;

    if ((err = write_power_clock(dev))      != ESP_OK) return err;
    if ((err = write_analog(dev))           != ESP_OK) return err;
    if ((err = write_digital_filter(dev))   != ESP_OK) return err;
    if ((err = write_interface_format(dev)) != ESP_OK) return err;
    if ((err = adaq_ll_write_reg(&dev->ll, ADAQ_REG_CONVERSION,
                                 dev->cfg.conv_mode)) != ESP_OK) return err;

    // PGA: configure GPIO0..3 as outputs and apply gain 1 + EN_PGA.
    if ((err = adaq7769_set_pga_gain(dev, dev->cfg.pga_gain)) != ESP_OK) return err;

    // Flush filters / synchronise after configuration.
    if (dev->is_sync_master) {
        adaq7769_sync_pulse(dev);
    }
    return ESP_OK;
}

// -----------------------------------------------------------------------------
// Clock & power
// -----------------------------------------------------------------------------
esp_err_t adaq7769_set_clock_source(adaq7769_t *dev, uint8_t clock_sel)
{
    dev->cfg.clock_sel = clock_sel & 3;
    return write_power_clock(dev);
}

esp_err_t adaq7769_set_mclk_div(adaq7769_t *dev, uint8_t mclk_div)
{
    dev->cfg.mclk_div = mclk_div & 3;
    return write_power_clock(dev);
}

esp_err_t adaq7769_set_power_mode(adaq7769_t *dev, uint8_t adc_mode)
{
    dev->cfg.adc_mode = adc_mode & 3;
    return write_power_clock(dev);
}

esp_err_t adaq7769_power_down(adaq7769_t *dev)
{
    // Must write 0x08 alone (any other bits set causes the write to be ignored).
    return adaq_ll_write_reg(&dev->ll, ADAQ_REG_POWER_CLOCK, ADAQ_PC_ADC_POWER_DOWN);
}

esp_err_t adaq7769_resume(adaq7769_t *dev)
{
    // SPI resume: a 1 followed by 63 zeros clocked in on SDI while CS low.
    uint8_t seq[8] = { 0x80, 0, 0, 0, 0, 0, 0, 0 };
    esp_err_t err = adaq_ll_write_raw(&dev->ll, seq, sizeof(seq));
    esp_rom_delay_us(300);
    if (err == ESP_OK) {
        err = write_power_clock(dev);   // restore divider/mode shadow
    }
    return err;
}

// -----------------------------------------------------------------------------
// Digital filter & ODR
// -----------------------------------------------------------------------------
esp_err_t adaq7769_set_filter(adaq7769_t *dev, uint8_t filter, uint8_t dec_rate)
{
    dev->cfg.filter   = filter & 7;
    dev->cfg.dec_rate = dec_rate & 7;
    dev->cfg.conv16   = (filter == ADAQ_FILTER_SINC5_X8);  // 16-bit only path
    esp_err_t err = write_digital_filter(dev);
    if (err == ESP_OK && dev->cfg.conv16 != 0) {
        err = write_interface_format(dev);
    }
    if (err == ESP_OK && dev->is_sync_master) {
        adaq7769_sync_pulse(dev);  // SYNC required after any filter change
    }
    return err;
}

esp_err_t adaq7769_set_sinc3(adaq7769_t *dev, uint32_t decimation, bool reject_50_60)
{
    if (decimation < 32) decimation = 32;
    // Register value = (decimation / 32) - 1, 13-bit.
    uint32_t regval = (decimation / 32);
    if (regval == 0) regval = 1;
    regval -= 1;
    if (regval > 0x1FFF) regval = 0x1FFF;

    dev->cfg.filter       = ADAQ_FILTER_SINC3;
    dev->cfg.sinc3_dec    = (regval + 1) * 32;
    dev->cfg.reject_50_60 = reject_50_60;
    dev->cfg.conv16       = false;

    esp_err_t err = adaq_ll_write_reg(&dev->ll, ADAQ_REG_SINC3_DEC_MSB,
                                      (uint8_t)((regval >> 8) & ADAQ_SINC3_DEC_MSB_MASK));
    if (err != ESP_OK) return err;
    err = adaq_ll_write_reg(&dev->ll, ADAQ_REG_SINC3_DEC_LSB, (uint8_t)(regval & 0xFF));
    if (err != ESP_OK) return err;
    err = write_digital_filter(dev);
    if (err == ESP_OK && dev->is_sync_master) {
        adaq7769_sync_pulse(dev);
    }
    return err;
}

float adaq7769_output_data_rate(const adaq7769_t *dev)
{
    float fmod = (float)dev->mclk_hz / (float)mclk_div_value(dev->cfg.mclk_div);
    switch (dev->cfg.filter) {
        case ADAQ_FILTER_SINC5_X8:  return fmod / 8.0f;
        case ADAQ_FILTER_SINC5_X16: return fmod / 16.0f;
        case ADAQ_FILTER_SINC3:
            return dev->cfg.sinc3_dec ? (fmod / (float)dev->cfg.sinc3_dec) : 0.0f;
        case ADAQ_FILTER_SINC5:
        case ADAQ_FILTER_WIDEBAND:
        default:
            return fmod / (float)dec_rate_value(dev->cfg.dec_rate);
    }
}

esp_err_t adaq7769_set_output_data_rate(adaq7769_t *dev, float target_sps,
                                        float *achieved)
{
    // Strategy: keep fMOD high (MCLK/2) for best AAF rejection, then choose a
    // filter + decimation. Wideband FIR is preferred for spectral work; Sinc5
    // covers the very fast end; Sinc3 covers the very slow end.
    float fmod = (float)dev->mclk_hz / 2.0f;   // MCLK_DIV_2
    esp_err_t err = adaq7769_set_mclk_div(dev, ADAQ_MCLK_DIV_2);
    if (err != ESP_OK) return err;

    if (target_sps >= fmod / 12.0f) {
        // Very high rate -> Sinc5 fixed paths.
        if (target_sps >= fmod / 6.0f) {
            err = adaq7769_set_filter(dev, ADAQ_FILTER_SINC5_X8, ADAQ_DEC_X32);   // /8
        } else {
            err = adaq7769_set_filter(dev, ADAQ_FILTER_SINC5_X16, ADAQ_DEC_X32);  // /16
        }
    } else if (target_sps >= fmod / 1024.0f) {
        // Wideband FIR, pick nearest decimation x32..x1024.
        static const uint8_t decs[] = { ADAQ_DEC_X32, ADAQ_DEC_X64, ADAQ_DEC_X128,
                                        ADAQ_DEC_X256, ADAQ_DEC_X512, ADAQ_DEC_X1024 };
        uint8_t best = ADAQ_DEC_X1024;
        float best_err = 1e30f;
        for (size_t i = 0; i < sizeof(decs); ++i) {
            float odr = fmod / (float)dec_rate_value(decs[i]);
            float e = odr > target_sps ? (odr - target_sps) : (target_sps - odr);
            if (e < best_err) { best_err = e; best = decs[i]; }
        }
        err = adaq7769_set_filter(dev, ADAQ_FILTER_WIDEBAND, best);
    } else {
        // Very low rate -> Sinc3 programmable decimation.
        uint32_t dec = (uint32_t)(fmod / target_sps + 0.5f);
        err = adaq7769_set_sinc3(dev, dec, false);
    }

    if (err == ESP_OK && achieved) {
        *achieved = adaq7769_output_data_rate(dev);
    }
    return err;
}

// -----------------------------------------------------------------------------
// Analog front-end
// -----------------------------------------------------------------------------
esp_err_t adaq7769_set_pga_gain(adaq7769_t *dev, uint8_t gain)
{
    uint8_t code;
    if (!pga_gain_to_code(gain, &code)) {
        return ESP_ERR_INVALID_ARG;
    }
    dev->cfg.pga_gain = gain;

    // GPIO0..2 = GAIN0..2, GPIO3 = EN_PGA. All outputs, UGPIO_EN set.
    dev->gpio_control_shadow = ADAQ_GC_UGPIO_EN |
                               ADAQ_GC_GPIO0_OP_EN | ADAQ_GC_GPIO1_OP_EN |
                               ADAQ_GC_GPIO2_OP_EN | ADAQ_GC_GPIO3_OP_EN;
    esp_err_t err = adaq_ll_write_reg(&dev->ll, ADAQ_REG_GPIO_CONTROL,
                                      dev->gpio_control_shadow);
    if (err != ESP_OK) return err;

    dev->gpio_write_shadow = (uint8_t)(code & 0x07);
    if (dev->cfg.pga_enabled) {
        dev->gpio_write_shadow |= ADAQ_GPIO3_BIT;   // EN_PGA high
    }
    return adaq_ll_write_reg(&dev->ll, ADAQ_REG_GPIO_WRITE, dev->gpio_write_shadow);
}

esp_err_t adaq7769_enable_pga(adaq7769_t *dev, bool enable)
{
    dev->cfg.pga_enabled = enable;
    if (enable) {
        dev->gpio_write_shadow |= ADAQ_GPIO3_BIT;
    } else {
        dev->gpio_write_shadow &= (uint8_t)~ADAQ_GPIO3_BIT;
    }
    return adaq_ll_write_reg(&dev->ll, ADAQ_REG_GPIO_WRITE, dev->gpio_write_shadow);
}

void adaq7769_set_aaf_input(adaq7769_t *dev, adaq_aaf_input_t input)
{
    dev->aaf_input = input;
}

esp_err_t adaq7769_set_reference(adaq7769_t *dev, uint8_t ref_buf_pos,
                                 uint8_t ref_buf_neg, bool linearity_boost)
{
    dev->cfg.ref_buf_pos = ref_buf_pos & 3;
    dev->cfg.ref_buf_neg = ref_buf_neg & 3;
    dev->cfg.lin_boost   = linearity_boost;
    return write_analog(dev);
}

// -----------------------------------------------------------------------------
// Conversion mode & readback
// -----------------------------------------------------------------------------
esp_err_t adaq7769_set_conv_mode(adaq7769_t *dev, uint8_t conv_mode)
{
    dev->cfg.conv_mode = conv_mode & 7;
    uint8_t v = dev->cfg.conv_mode & ADAQ_CV_CONV_MODE_MASK;
    esp_err_t err = adaq_ll_write_reg(&dev->ll, ADAQ_REG_CONVERSION, v);
    if (err == ESP_OK && dev->is_sync_master) {
        adaq7769_sync_pulse(dev);
    }
    return err;
}

esp_err_t adaq7769_set_read_format(adaq7769_t *dev, bool continuous,
                                   bool append_status, bool append_crc,
                                   bool crc_xor, bool conv16)
{
    dev->cfg.cont_read     = continuous;
    dev->cfg.status_append = append_status;
    dev->cfg.crc_append    = append_crc;
    dev->cfg.crc_xor       = crc_xor;
    dev->cfg.conv16        = conv16;
    return write_interface_format(dev);
}

esp_err_t adaq7769_read_sample(adaq7769_t *dev, int32_t *raw)
{
    return adaq_ll_read_adc24(&dev->ll, raw);
}

float adaq7769_total_gain(const adaq7769_t *dev)
{
    return (float)dev->cfg.pga_gain * aaf_gain_value(dev->aaf_input);
}

float adaq7769_code_to_volts(const adaq7769_t *dev, int32_t raw)
{
    // Voltage = (code) * 2 * VREF / (2^24 * TOTAL_GAIN)   [code already signed]
    float total_gain = adaq7769_total_gain(dev);
    if (total_gain <= 0.0f) total_gain = 1.0f;
    return ((float)raw * 2.0f * dev->vref) / (16777216.0f * total_gain);
}

// -----------------------------------------------------------------------------
// Calibration
// -----------------------------------------------------------------------------
esp_err_t adaq7769_set_offset_cal(adaq7769_t *dev, int32_t offset24)
{
    uint32_t v = (uint32_t)offset24 & 0xFFFFFFu;
    esp_err_t err = adaq_ll_write_reg(&dev->ll, ADAQ_REG_OFFSET_HI, (v >> 16) & 0xFF);
    if (err != ESP_OK) return err;
    err = adaq_ll_write_reg(&dev->ll, ADAQ_REG_OFFSET_MID, (v >> 8) & 0xFF);
    if (err != ESP_OK) return err;
    return adaq_ll_write_reg(&dev->ll, ADAQ_REG_OFFSET_LO, v & 0xFF);
}

esp_err_t adaq7769_get_offset_cal(adaq7769_t *dev, int32_t *offset24)
{
    uint8_t hi = 0, mid = 0, lo = 0;
    esp_err_t err = adaq_ll_read_reg(&dev->ll, ADAQ_REG_OFFSET_HI, &hi);
    if (err != ESP_OK) return err;
    adaq_ll_read_reg(&dev->ll, ADAQ_REG_OFFSET_MID, &mid);
    adaq_ll_read_reg(&dev->ll, ADAQ_REG_OFFSET_LO, &lo);
    uint32_t v = ((uint32_t)hi << 16) | ((uint32_t)mid << 8) | lo;
    *offset24 = adaq_sign_extend24(v);
    return ESP_OK;
}

esp_err_t adaq7769_set_gain_cal(adaq7769_t *dev, uint32_t gain24)
{
    gain24 &= 0xFFFFFFu;
    esp_err_t err = adaq_ll_write_reg(&dev->ll, ADAQ_REG_GAIN_HI, (gain24 >> 16) & 0xFF);
    if (err != ESP_OK) return err;
    err = adaq_ll_write_reg(&dev->ll, ADAQ_REG_GAIN_MID, (gain24 >> 8) & 0xFF);
    if (err != ESP_OK) return err;
    return adaq_ll_write_reg(&dev->ll, ADAQ_REG_GAIN_LO, gain24 & 0xFF);
}

esp_err_t adaq7769_get_gain_cal(adaq7769_t *dev, uint32_t *gain24)
{
    uint8_t hi = 0, mid = 0, lo = 0;
    esp_err_t err = adaq_ll_read_reg(&dev->ll, ADAQ_REG_GAIN_HI, &hi);
    if (err != ESP_OK) return err;
    adaq_ll_read_reg(&dev->ll, ADAQ_REG_GAIN_MID, &mid);
    adaq_ll_read_reg(&dev->ll, ADAQ_REG_GAIN_LO, &lo);
    *gain24 = ((uint32_t)hi << 16) | ((uint32_t)mid << 8) | lo;
    return ESP_OK;
}

// -----------------------------------------------------------------------------
// GPIO
// -----------------------------------------------------------------------------
esp_err_t adaq7769_gpio_config(adaq7769_t *dev, uint8_t output_mask,
                               uint8_t open_drain_mask)
{
    uint8_t v = ADAQ_GC_UGPIO_EN | (output_mask & 0x0F);
    if (open_drain_mask & ADAQ_GPIO0_BIT) v |= ADAQ_GC_GPIO0_OPEN_DRAIN;
    if (open_drain_mask & ADAQ_GPIO1_BIT) v |= ADAQ_GC_GPIO1_OPEN_DRAIN;
    if (open_drain_mask & ADAQ_GPIO2_BIT) v |= ADAQ_GC_GPIO2_OPEN_DRAIN;
    dev->gpio_control_shadow = v;
    return adaq_ll_write_reg(&dev->ll, ADAQ_REG_GPIO_CONTROL, v);
}

esp_err_t adaq7769_gpio_write(adaq7769_t *dev, uint8_t level_mask)
{
    dev->gpio_write_shadow = level_mask & 0x0F;
    return adaq_ll_write_reg(&dev->ll, ADAQ_REG_GPIO_WRITE, dev->gpio_write_shadow);
}

esp_err_t adaq7769_gpio_read(adaq7769_t *dev, uint8_t *level_mask)
{
    uint8_t v = 0;
    esp_err_t err = adaq_ll_read_reg(&dev->ll, ADAQ_REG_GPIO_READ, &v);
    if (err == ESP_OK) *level_mask = v & 0x0F;
    return err;
}

// -----------------------------------------------------------------------------
// Synchronisation
// -----------------------------------------------------------------------------
esp_err_t adaq7769_sync_pulse(adaq7769_t *dev)
{
    // SPI_START is active-low: drive SYNC_OUT low (bit7 = 0) then restore.
    uint8_t base = 0x00;  // SYNC_OUT_POS_EDGE off, no GPIO start
    esp_err_t err = adaq_ll_write_reg(&dev->ll, ADAQ_REG_SYNC_RESET, base);
    if (err != ESP_OK) return err;
    // Bit self-clears; restore default (bit7 high).
    return adaq_ll_write_reg(&dev->ll, ADAQ_REG_SYNC_RESET, ADAQ_SR_SPI_START);
}

esp_err_t adaq7769_enable_gpio_start(adaq7769_t *dev, bool enable)
{
    uint8_t v = ADAQ_SR_SPI_START;  // keep SPI_START idle high
    if (enable) v |= ADAQ_SR_EN_GPIO_START;
    return adaq_ll_write_reg(&dev->ll, ADAQ_REG_SYNC_RESET, v);
}

// -----------------------------------------------------------------------------
// Diagnostics
// -----------------------------------------------------------------------------
esp_err_t adaq7769_read_status(adaq7769_t *dev, uint8_t *master_status)
{
    return adaq_ll_read_reg(&dev->ll, ADAQ_REG_MASTER_STATUS, master_status);
}

esp_err_t adaq7769_read_spi_errors(adaq7769_t *dev, uint8_t *spi_status)
{
    return adaq_ll_read_reg(&dev->ll, ADAQ_REG_SPI_DIAG_STATUS, spi_status);
}

esp_err_t adaq7769_clear_spi_errors(adaq7769_t *dev, uint8_t mask)
{
    // W1C bits.
    return adaq_ll_write_reg(&dev->ll, ADAQ_REG_SPI_DIAG_STATUS, mask);
}

esp_err_t adaq7769_enable_diagnostics(adaq7769_t *dev, uint8_t adc_diag_mask,
                                      uint8_t dig_diag_mask)
{
    esp_err_t err = adaq_ll_write_reg(&dev->ll, ADAQ_REG_ADC_DIAG_ENABLE, adc_diag_mask);
    if (err != ESP_OK) return err;
    return adaq_ll_write_reg(&dev->ll, ADAQ_REG_DIG_DIAG_ENABLE, dig_diag_mask);
}

esp_err_t adaq7769_read_diagnostic(adaq7769_t *dev, uint8_t diag_mux, int32_t *raw)
{
    uint8_t v = (uint8_t)(((diag_mux & 0xF) << ADAQ_CV_DIAG_MUX_SHIFT) |
                          ADAQ_CV_CONV_DIAG_SELECT |
                          (dev->cfg.conv_mode & ADAQ_CV_CONV_MODE_MASK));
    esp_err_t err = adaq_ll_write_reg(&dev->ll, ADAQ_REG_CONVERSION, v);
    if (err != ESP_OK) return err;
    if (dev->is_sync_master) adaq7769_sync_pulse(dev);
    // Allow the filter to settle before reading.
    vTaskDelay(pdMS_TO_TICKS(2));
    err = adaq_ll_read_adc24(&dev->ll, raw);
    // Restore normal signal-chain conversion.
    adaq_ll_write_reg(&dev->ll, ADAQ_REG_CONVERSION,
                      dev->cfg.conv_mode & ADAQ_CV_CONV_MODE_MASK);
    return err;
}
