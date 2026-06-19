// =============================================================================
// ad741x.c — AD7414 / AD7415 I2C temperature-sensor driver
// =============================================================================

#include "ad741x.h"
#include <string.h>
#include "esp_log.h"

static const char *TAG = "ad741x";

esp_err_t ad741x_attach(ad741x_t *s, i2c_master_bus_handle_t bus,
                        uint8_t addr, uint32_t scl_hz, bool has_alert)
{
    memset(s, 0, sizeof(*s));
    s->addr      = addr;
    s->has_alert = has_alert;

    i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = addr,
        .scl_speed_hz    = scl_hz,
    };
    esp_err_t err = i2c_master_bus_add_device(bus, &cfg, &s->dev);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "add device 0x%02X failed: %s", addr, esp_err_to_name(err));
        return err;
    }

    // Probe: read the config register.
    uint8_t cfgval = 0;
    err = ad741x_read_config(s, &cfgval);
    s->present = (err == ESP_OK);
    if (!s->present) {
        ESP_LOGW(TAG, "sensor 0x%02X not responding", addr);
    }
    return err;
}

esp_err_t ad741x_read_raw(ad741x_t *s, int16_t *raw10)
{
    uint8_t ptr = AD741X_REG_TEMP;
    uint8_t rx[2] = {0};
    esp_err_t err = i2c_master_transmit_receive(s->dev, &ptr, 1, rx, 2, 100);
    if (err != ESP_OK) {
        return err;
    }
    int16_t v = (int16_t)(((uint16_t)rx[0] << 2) | (rx[1] >> 6));  // 10-bit
    if (v & 0x200) {
        v -= 1024;   // sign-extend 10-bit twos complement
    }
    *raw10 = v;
    return ESP_OK;
}

esp_err_t ad741x_read_celsius(ad741x_t *s, float *celsius)
{
    int16_t raw;
    esp_err_t err = ad741x_read_raw(s, &raw);
    if (err != ESP_OK) {
        return err;
    }
    *celsius = (float)raw * 0.25f;   // 1 LSB = 0.25 C
    return ESP_OK;
}

esp_err_t ad741x_write_config(ad741x_t *s, uint8_t config)
{
    uint8_t tx[2] = { AD741X_REG_CONFIG, config };
    return i2c_master_transmit(s->dev, tx, 2, 100);
}

esp_err_t ad741x_read_config(ad741x_t *s, uint8_t *config)
{
    uint8_t ptr = AD741X_REG_CONFIG;
    return i2c_master_transmit_receive(s->dev, &ptr, 1, config, 1, 100);
}

esp_err_t ad741x_oneshot(ad741x_t *s, uint8_t base_config)
{
    return ad741x_write_config(s, (uint8_t)(base_config | AD741X_CFG_ONESHOT));
}

esp_err_t ad741x_set_limits(ad741x_t *s, int8_t t_high_c, int8_t t_low_c)
{
    if (!s->has_alert) {
        return ESP_ERR_NOT_SUPPORTED;   // AD7415 has no limit registers
    }
    uint8_t hi[2] = { AD741X_REG_THIGH, (uint8_t)t_high_c };
    esp_err_t err = i2c_master_transmit(s->dev, hi, 2, 100);
    if (err != ESP_OK) {
        return err;
    }
    uint8_t lo[2] = { AD741X_REG_TLOW, (uint8_t)t_low_c };
    return i2c_master_transmit(s->dev, lo, 2, 100);
}
