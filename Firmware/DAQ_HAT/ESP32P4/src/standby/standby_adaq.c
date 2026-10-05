#include "standby_adaq.h"

#include <string.h>

esp_err_t sb_adaq_save(adaq7769_t *dev, bool usable, sb_adaq_shadow_t *s)
{
    memset(s, 0, sizeof(*s));
    s->was_ok = usable;
    if (!usable) return ESP_OK;               // absent at boot: nothing to keep, nothing to restore

    esp_err_t err = adaq7769_get_offset_cal(dev, &s->offset24);
    if (err != ESP_OK) return err;
    err = adaq7769_get_gain_cal(dev, &s->gain24);
    if (err != ESP_OK) return err;

    s->cfg = dev->cfg;
    s->gpio_control = dev->gpio_control_shadow;
    s->gpio_write = dev->gpio_write_shadow;
    s->diag_last_status = dev->diag_last_status;
    s->diag_sticky = dev->diag_sticky;
    s->diag_err_count = dev->diag_err_count;
    s->diag_status_reads = dev->diag_status_reads;
    s->valid = true;
    return ESP_OK;
}

esp_err_t sb_adaq_restore(adaq7769_t *dev, const sb_adaq_shadow_t *s)
{
    if (!s->was_ok) return ESP_OK;
    if (!s->valid) return ESP_ERR_INVALID_STATE;

    // The part came out of reset with the CRC option off, whatever the driver
    // last knew; reset + identify exactly as at boot (identify re-enables it).
    adaq_ll_set_crc(&dev->ll, false, false);
    esp_err_t err = adaq7769_soft_reset(dev);
    if (err != ESP_OK) return err;
    err = adaq7769_identify(dev);
    if (err != ESP_OK) return err;

    dev->cfg = s->cfg;
    if ((err = adaq7769_set_clock_source(dev, s->cfg.clock_sel)) != ESP_OK) return err;   // POWER_CLOCK in full
    if ((err = adaq7769_set_reference(dev, s->cfg.ref_buf_pos, s->cfg.ref_buf_neg,
                                      s->cfg.lin_boost)) != ESP_OK) return err;
    if (s->cfg.filter == ADAQ_FILTER_SINC3) {
        err = adaq7769_set_sinc3(dev, s->cfg.sinc3_dec, s->cfg.reject_50_60);
    } else {
        err = adaq7769_set_filter(dev, s->cfg.filter, s->cfg.dec_rate);
    }
    if (err != ESP_OK) return err;
    if ((err = adaq7769_set_read_format(dev, s->cfg.cont_read, s->cfg.status_append,
                                        s->cfg.crc_append, s->cfg.crc_xor,
                                        s->cfg.conv16)) != ESP_OK) return err;
    if ((err = adaq7769_set_conv_mode(dev, s->cfg.conv_mode)) != ESP_OK) return err;

    // PGA gain and the other GPIO lines, exactly as they were.
    if ((err = adaq_ll_write_reg(&dev->ll, ADAQ_REG_GPIO_CONTROL, s->gpio_control)) != ESP_OK) return err;
    if ((err = adaq_ll_write_reg(&dev->ll, ADAQ_REG_GPIO_WRITE, s->gpio_write)) != ESP_OK) return err;
    dev->gpio_control_shadow = s->gpio_control;
    dev->gpio_write_shadow = s->gpio_write;

    if ((err = adaq7769_set_offset_cal(dev, s->offset24)) != ESP_OK) return err;
    if ((err = adaq7769_set_gain_cal(dev, s->gain24)) != ESP_OK) return err;

    // The restore must be provable: read the calibration registers back.
    int32_t off = 0;
    uint32_t gain = 0;
    if ((err = adaq7769_get_offset_cal(dev, &off)) != ESP_OK) return err;
    if ((err = adaq7769_get_gain_cal(dev, &gain)) != ESP_OK) return err;
    if (off != s->offset24 || gain != s->gain24) return ESP_ERR_INVALID_RESPONSE;

    // Fault history is a record of what happened, not part of the configuration.
    dev->diag_last_status = s->diag_last_status;
    dev->diag_sticky = s->diag_sticky;
    dev->diag_err_count = s->diag_err_count;
    dev->diag_status_reads = s->diag_status_reads;
    dev->present = true;
    return ESP_OK;
}

esp_err_t sb_adaq_check_conversion(adaq7769_t *dev, bool was_ok)
{
    if (!was_ok) return ESP_OK;
    int32_t discarded = 0;
    esp_err_t err = adaq7769_read_sample(dev, &discarded);
    if (err != ESP_OK) return err;
    uint8_t st = 0;
    err = adaq7769_read_status(dev, &st);
    if (err != ESP_OK) return err;
    // Saturation and "filter not settled" are expected with open inputs right after a
    // reset; a real converter / clock / digital fault is not.
    if (st & (ADAQ_ST_ADC_ERROR | ADAQ_ST_DIG_ERROR | ADAQ_ST_ERR_EXT_CLK_QUAL)) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    return ESP_OK;
}
