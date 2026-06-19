// =============================================================================
// ds4424_p4.c — DS4424 4-channel I2C IDAC (ESP32-P4 port)
// =============================================================================

#include "ds4424_p4.h"
#include <string.h>
#include "esp_log.h"

static const char *TAG = "ds4424";

static const uint8_t s_reg[DS4424_NUM_CHANNELS] = {
    DS4424_REG_OUT0, DS4424_REG_OUT1, DS4424_REG_OUT2, DS4424_REG_OUT3
};

esp_err_t ds4424_attach(ds4424_t *d, i2c_master_bus_handle_t bus,
                        uint8_t addr, uint32_t scl_hz)
{
    memset(d, 0, sizeof(*d));
    d->addr = addr;

    i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = addr,
        .scl_speed_hz    = scl_hz,
    };
    esp_err_t err = i2c_master_bus_add_device(bus, &cfg, &d->dev);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "add device 0x%02X failed: %s", addr, esp_err_to_name(err));
        return err;
    }

    // Probe and zero all channels.
    err = ds4424_set_code(d, 0, 0);
    d->present = (err == ESP_OK);
    if (d->present) {
        for (uint8_t ch = 1; ch < DS4424_NUM_CHANNELS; ++ch) {
            ds4424_set_code(d, ch, 0);
        }
    } else {
        ESP_LOGW(TAG, "DS4424 0x%02X not responding", addr);
    }
    return err;
}

esp_err_t ds4424_set_code(ds4424_t *d, uint8_t ch, int8_t code)
{
    if (ch >= DS4424_NUM_CHANNELS) {
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t reg_val;
    if (code == 0) {
        reg_val = 0x00;
    } else if (code > 0) {
        reg_val = (uint8_t)(0x80 | (uint8_t)code);   // source current
    } else {
        reg_val = (uint8_t)(-code);                  // sink current
    }
    uint8_t buf[2] = { s_reg[ch], reg_val };
    esp_err_t err = i2c_master_transmit(d->dev, buf, 2, 50);
    if (err == ESP_OK) {
        d->code[ch] = code;
    }
    return err;
}

esp_err_t ds4424_get_code(ds4424_t *d, uint8_t ch, int8_t *code)
{
    if (ch >= DS4424_NUM_CHANNELS) {
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t reg_addr = s_reg[ch];
    uint8_t val = 0;
    esp_err_t err = i2c_master_transmit_receive(d->dev, &reg_addr, 1, &val, 1, 50);
    if (err != ESP_OK) {
        return err;
    }
    int8_t c = (val & 0x80) ? (int8_t)(val & 0x7F) : (int8_t)(-(int)(val & 0x7F));
    *code = c;
    return ESP_OK;
}
