// =============================================================================
// adaq7769_ll.c — Low-level SPI transport for the ADAQ7769-1
// =============================================================================

#include "adaq7769_ll.h"
#include <string.h>
#include "esp_log.h"

static const char *TAG = "adaq_ll";

// SPI Mode 3: SCLK idles high, data clocked out on falling edge, sampled on
// rising edge (datasheet "Digital Interface").
#define ADAQ_SPI_MODE   3

// -----------------------------------------------------------------------------
// CRC-8: x^8 + x^2 + x + 1  (0x07), MSB-first.
// -----------------------------------------------------------------------------
uint8_t adaq_ll_crc8(const uint8_t *data, size_t len, uint8_t init)
{
    uint8_t crc = init;
    for (size_t i = 0; i < len; ++i) {
        crc ^= data[i];
        for (int b = 0; b < 8; ++b) {
            if (crc & 0x80u) {
                crc = (uint8_t)((crc << 1) ^ ADAQ_CRC8_POLY);
            } else {
                crc = (uint8_t)(crc << 1);
            }
        }
    }
    return crc;
}

void adaq_ll_set_crc(adaq_ll_t *ll, bool enabled, bool xor_mode)
{
    ll->crc_enabled = enabled;
    ll->crc_xor     = xor_mode;
}

// -----------------------------------------------------------------------------
// Bus / device lifecycle
// -----------------------------------------------------------------------------
esp_err_t adaq_ll_bus_init(spi_host_device_t host,
                           gpio_num_t sclk, gpio_num_t mosi, gpio_num_t miso,
                           int max_xfer_bytes)
{
    spi_bus_config_t buscfg = {
        .mosi_io_num     = mosi,
        .miso_io_num     = miso,
        .sclk_io_num     = sclk,
        .quadwp_io_num   = -1,
        .quadhd_io_num   = -1,
        .max_transfer_sz = max_xfer_bytes,
    };
    esp_err_t err = spi_bus_initialize(host, &buscfg, SPI_DMA_CH_AUTO);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "spi_bus_initialize(host=%d) failed: %s", host, esp_err_to_name(err));
    }
    return err;
}

esp_err_t adaq_ll_add_device(adaq_ll_t *ll, spi_host_device_t host,
                             gpio_num_t cs_pin, uint32_t cfg_hz, uint32_t data_hz)
{
    memset(ll, 0, sizeof(*ll));
    ll->host    = host;
    ll->cs_pin  = cs_pin;
    ll->cfg_hz  = cfg_hz;
    ll->data_hz = data_hz;

    // Config handle: conservative clock, generous queue depth of 1 (polling).
    spi_device_interface_config_t cfg = {
        .mode           = ADAQ_SPI_MODE,
        .clock_speed_hz = (int)cfg_hz,
        .spics_io_num   = cs_pin,
        .queue_size     = 1,
        .flags          = 0,
    };
    esp_err_t err = spi_bus_add_device(host, &cfg, &ll->dev_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "add cfg device (cs=%d) failed: %s", cs_pin, esp_err_to_name(err));
        return err;
    }

    // Data handle: fast clock, deeper queue for the streaming layer.
    spi_device_interface_config_t datcfg = {
        .mode           = ADAQ_SPI_MODE,
        .clock_speed_hz = (int)data_hz,
        .spics_io_num   = cs_pin,
        .queue_size     = 8,
        .flags          = 0,
    };
    err = spi_bus_add_device(host, &datcfg, &ll->dev_data);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "add data device (cs=%d) failed: %s", cs_pin, esp_err_to_name(err));
        spi_bus_remove_device(ll->dev_cfg);
        ll->dev_cfg = NULL;
        return err;
    }
    return ESP_OK;
}

// -----------------------------------------------------------------------------
// Register access
// -----------------------------------------------------------------------------
esp_err_t adaq_ll_write_reg(adaq_ll_t *ll, uint8_t addr, uint8_t val)
{
    uint8_t tx[3];
    uint8_t n = 0;
    tx[n++] = ADAQ_INSTR_WRITE(addr);
    tx[n++] = val;
    if (ll->crc_enabled) {
        // Writes always use polynomial CRC over the frame (init 0x00).
        tx[n] = adaq_ll_crc8(tx, n, 0x00);
        n++;
    }
    spi_transaction_t t = {0};
    t.length    = (size_t)n * 8;
    t.tx_buffer = tx;
    return spi_device_polling_transmit(ll->dev_cfg, &t);
}

esp_err_t adaq_ll_read_reg(adaq_ll_t *ll, uint8_t addr, uint8_t *val)
{
    uint8_t tx[3] = {0};
    uint8_t rx[3] = {0};
    uint8_t n = 2;                 // instruction + 1 data byte
    tx[0] = ADAQ_INSTR_READ(addr);
    if (ll->crc_enabled) {
        n = 3;                     // + appended CRC/XOR byte
    }
    spi_transaction_t t = {0};
    t.length    = (size_t)n * 8;
    t.rxlength  = (size_t)n * 8;
    t.tx_buffer = tx;
    t.rx_buffer = rx;
    esp_err_t err = spi_device_polling_transmit(ll->dev_cfg, &t);
    if (err != ESP_OK) {
        return err;
    }
    if (ll->crc_enabled) {
        uint8_t frame[2] = { tx[0], rx[1] };
        uint8_t expect = ll->crc_xor
                             ? (uint8_t)(frame[0] ^ frame[1])
                             : adaq_ll_crc8(frame, 2, 0x00);
        if (expect != rx[2]) {
            ESP_LOGW(TAG, "reg 0x%02X CRC mismatch (got 0x%02X want 0x%02X)",
                     addr, rx[2], expect);
            return ESP_ERR_INVALID_CRC;
        }
    }
    *val = rx[1];
    return ESP_OK;
}

esp_err_t adaq_ll_read_adc24(adaq_ll_t *ll, int32_t *sample)
{
    uint8_t tx[5] = {0};
    uint8_t rx[5] = {0};
    uint8_t n = 4;                 // instruction + 3 data bytes
    tx[0] = ADAQ_INSTR_READ(ADAQ_REG_ADC_DATA);
    if (ll->crc_enabled) {
        n = 5;
    }
    spi_transaction_t t = {0};
    t.length    = (size_t)n * 8;
    t.rxlength  = (size_t)n * 8;
    t.tx_buffer = tx;
    t.rx_buffer = rx;
    esp_err_t err = spi_device_polling_transmit(ll->dev_data, &t);
    if (err != ESP_OK) {
        return err;
    }
    if (ll->crc_enabled) {
        uint8_t frame[4] = { tx[0], rx[1], rx[2], rx[3] };
        uint8_t expect = ll->crc_xor
                             ? (uint8_t)(frame[0] ^ frame[1] ^ frame[2] ^ frame[3])
                             : adaq_ll_crc8(frame, 4, 0x00);
        if (expect != rx[4]) {
            return ESP_ERR_INVALID_CRC;
        }
    }
    uint32_t raw = ((uint32_t)rx[1] << 16) | ((uint32_t)rx[2] << 8) | rx[3];
    *sample = adaq_sign_extend24(raw);
    return ESP_OK;
}

esp_err_t adaq_ll_write_raw(adaq_ll_t *ll, const uint8_t *tx, size_t len)
{
    spi_transaction_t t = {0};
    t.length    = len * 8;
    t.tx_buffer = tx;
    return spi_device_polling_transmit(ll->dev_cfg, &t);
}

esp_err_t adaq_ll_contread_word(adaq_ll_t *ll, uint8_t *rx, size_t len_bytes)
{
    spi_transaction_t t = {0};
    t.length    = len_bytes * 8;
    t.rxlength  = len_bytes * 8;
    t.tx_buffer = NULL;
    t.rx_buffer = rx;
    return spi_device_polling_transmit(ll->dev_data, &t);
}
