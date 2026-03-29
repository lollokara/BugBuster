// =============================================================================
// ad74416h_spi.cpp - AD74416H SPI driver (ESP-IDF native)
// =============================================================================

#include "ad74416h_spi.h"
#include "esp_log.h"
#include <string.h>

AD74416H_SPI::AD74416H_SPI(gpio_num_t pin_sdo, gpio_num_t pin_sdi,
                             gpio_num_t pin_sync, gpio_num_t pin_sclk,
                             uint8_t dev_addr)
    : _pin_sdo(pin_sdo),
      _pin_sdi(pin_sdi),
      _pin_sync(pin_sync),
      _pin_sclk(pin_sclk),
      _dev_addr(dev_addr & 0x03),
      _spi_dev(NULL),
      _mutex(NULL)
{
}

void AD74416H_SPI::begin()
{
    _mutex = xSemaphoreCreateMutex();
    configASSERT(_mutex);

    // SYNC pin: manual chip select (active low), idle HIGH
    pin_mode_output(_pin_sync);
    deassertSync();

    // Initialize SPI bus (SPI2_HOST = HSPI on ESP32-S3)
    spi_bus_config_t bus_cfg = {};
    bus_cfg.mosi_io_num     = _pin_sdi;
    bus_cfg.miso_io_num     = _pin_sdo;
    bus_cfg.sclk_io_num     = _pin_sclk;
    bus_cfg.quadwp_io_num   = -1;
    bus_cfg.quadhd_io_num   = -1;
    bus_cfg.max_transfer_sz = SPI_FRAME_BYTES;

    ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &bus_cfg, SPI_DMA_DISABLED));

    // Add device: Mode 2 (CPOL=1, CPHA=0), manual CS, 1 MHz
    spi_device_interface_config_t dev_cfg = {};
    dev_cfg.clock_speed_hz = SPI_CLOCK_HZ;
    dev_cfg.mode           = 2;            // CPOL=1, CPHA=0
    dev_cfg.spics_io_num   = -1;           // Manual CS via SYNC pin
    dev_cfg.queue_size     = 1;
    dev_cfg.flags          = 0;

    ESP_ERROR_CHECK(spi_bus_add_device(SPI2_HOST, &dev_cfg, &_spi_dev));
}

bool AD74416H_SPI::setClockSpeed(uint32_t hz)
{
    if (hz < 100000 || hz > 20000000) return false;  // 100kHz to 20MHz

    // Remove old device, add new one with updated clock
    spi_bus_remove_device(_spi_dev);

    spi_device_interface_config_t dev_cfg = {};
    dev_cfg.clock_speed_hz = hz;
    dev_cfg.mode           = 2;  // CPOL=1, CPHA=0
    dev_cfg.spics_io_num   = -1; // Manual CS
    dev_cfg.queue_size     = 4;

    esp_err_t err = spi_bus_add_device(SPI2_HOST, &dev_cfg, &_spi_dev);
    if (err != ESP_OK) {
        ESP_LOGE("spi", "Failed to re-add SPI device at %luHz: %s", (unsigned long)hz, esp_err_to_name(err));
        return false;
    }

    _clock_hz = hz;
    ESP_LOGI("spi", "SPI clock changed to %lu Hz (%.1f MHz)", (unsigned long)hz, hz / 1e6f);
    return true;
}

uint8_t AD74416H_SPI::computeCRC8(const uint8_t* frame) const
{
    uint8_t crc = CRC8_INIT;
    for (uint8_t byte_idx = 0; byte_idx < 4; byte_idx++) {
        crc ^= frame[byte_idx];
        for (uint8_t bit = 0; bit < 8; bit++) {
            if (crc & 0x80) {
                crc = (uint8_t)((crc << 1) ^ CRC8_POLYNOMIAL);
            } else {
                crc = (uint8_t)(crc << 1);
            }
        }
    }
    return crc;
}

void AD74416H_SPI::transferFrame(const uint8_t* tx_frame, uint8_t* rx_frame)
{
    spi_transaction_t txn = {};
    txn.length    = SPI_FRAME_BYTES * 8;  // bits
    txn.tx_buffer = tx_frame;
    txn.rx_buffer = rx_frame;

    // If no RX needed, use tx-only mode
    if (rx_frame == NULL) {
        txn.flags = SPI_TRANS_USE_TXDATA;
        memcpy(txn.tx_data, tx_frame, 4);  // Only 4 bytes fit in tx_data
        // For 5-byte frame, use buffer mode
        txn.flags = 0;
        txn.tx_buffer = tx_frame;

        uint8_t dummy_rx[SPI_FRAME_BYTES];
        txn.rx_buffer = dummy_rx;
    }

    assertSync();
    spi_device_polling_transmit(_spi_dev, &txn);
    deassertSync();
}

void AD74416H_SPI::writeRegister(uint8_t addr, uint16_t data)
{
    uint8_t frame[SPI_FRAME_BYTES];
    frame[SPI_FRAME_BYTE_CTRL]    = (uint8_t)((_dev_addr & 0x03) << SPI_CTRL_DEVADDR_SHIFT);
    frame[SPI_FRAME_BYTE_ADDR]    = addr;
    frame[SPI_FRAME_BYTE_DATA_HI] = (uint8_t)((data >> 8) & 0xFF);
    frame[SPI_FRAME_BYTE_DATA_LO] = (uint8_t)(data & 0xFF);
    frame[SPI_FRAME_BYTE_CRC]     = computeCRC8(frame);

    xSemaphoreTake(_mutex, portMAX_DELAY);
    transferFrame(frame, NULL);
    xSemaphoreGive(_mutex);
}

bool AD74416H_SPI::readRegister(uint8_t addr, uint16_t* data)
{
    xSemaphoreTake(_mutex, portMAX_DELAY);

    // Stage 1: Write register address to READ_SELECT
    {
        uint8_t frame[SPI_FRAME_BYTES];
        frame[SPI_FRAME_BYTE_CTRL]    = (uint8_t)((_dev_addr & 0x03) << SPI_CTRL_DEVADDR_SHIFT);
        frame[SPI_FRAME_BYTE_ADDR]    = REG_READ_SELECT;
        frame[SPI_FRAME_BYTE_DATA_HI] = 0x00;
        frame[SPI_FRAME_BYTE_DATA_LO] = addr;
        frame[SPI_FRAME_BYTE_CRC]     = computeCRC8(frame);
        transferFrame(frame, NULL);
    }

    // Stage 2: NOP to clock in response
    uint8_t tx_frame[SPI_FRAME_BYTES] = {0};
    uint8_t rx_frame[SPI_FRAME_BYTES] = {0};
    tx_frame[SPI_FRAME_BYTE_CTRL] = (uint8_t)((_dev_addr & 0x03) << SPI_CTRL_DEVADDR_SHIFT);
    tx_frame[SPI_FRAME_BYTE_ADDR] = REG_NOP;
    tx_frame[SPI_FRAME_BYTE_CRC]  = computeCRC8(tx_frame);
    transferFrame(tx_frame, rx_frame);

    xSemaphoreGive(_mutex);

    // Validate CRC
    uint8_t expected_crc = computeCRC8(rx_frame);
    if (rx_frame[SPI_FRAME_BYTE_CRC] != expected_crc) {
        if (data != NULL) *data = 0xFFFF;
        return false;
    }

    if (data != NULL) {
        *data = (uint16_t)(((uint16_t)rx_frame[SPI_FRAME_BYTE_DATA_HI] << 8) |
                            (uint16_t)rx_frame[SPI_FRAME_BYTE_DATA_LO]);
    }
    return true;
}

bool AD74416H_SPI::updateRegister(uint8_t addr, uint16_t mask, uint16_t val)
{
    xSemaphoreTake(_mutex, portMAX_DELAY);

    // Read
    {
        uint8_t frame[SPI_FRAME_BYTES];
        frame[SPI_FRAME_BYTE_CTRL]    = (uint8_t)((_dev_addr & 0x03) << SPI_CTRL_DEVADDR_SHIFT);
        frame[SPI_FRAME_BYTE_ADDR]    = REG_READ_SELECT;
        frame[SPI_FRAME_BYTE_DATA_HI] = 0x00;
        frame[SPI_FRAME_BYTE_DATA_LO] = addr;
        frame[SPI_FRAME_BYTE_CRC]     = computeCRC8(frame);
        transferFrame(frame, NULL);
    }

    uint8_t tx_nop[SPI_FRAME_BYTES] = {0};
    uint8_t rx_frame[SPI_FRAME_BYTES] = {0};
    tx_nop[SPI_FRAME_BYTE_CTRL] = (uint8_t)((_dev_addr & 0x03) << SPI_CTRL_DEVADDR_SHIFT);
    tx_nop[SPI_FRAME_BYTE_ADDR] = REG_NOP;
    tx_nop[SPI_FRAME_BYTE_CRC]  = computeCRC8(tx_nop);
    transferFrame(tx_nop, rx_frame);

    uint8_t expected_crc = computeCRC8(rx_frame);
    if (rx_frame[SPI_FRAME_BYTE_CRC] != expected_crc) {
        xSemaphoreGive(_mutex);
        return false;
    }

    uint16_t current = (uint16_t)(((uint16_t)rx_frame[SPI_FRAME_BYTE_DATA_HI] << 8) |
                                   (uint16_t)rx_frame[SPI_FRAME_BYTE_DATA_LO]);

    // Modify and write
    uint16_t updated = (current & ~mask) | (val & mask);
    {
        uint8_t frame[SPI_FRAME_BYTES];
        frame[SPI_FRAME_BYTE_CTRL]    = (uint8_t)((_dev_addr & 0x03) << SPI_CTRL_DEVADDR_SHIFT);
        frame[SPI_FRAME_BYTE_ADDR]    = addr;
        frame[SPI_FRAME_BYTE_DATA_HI] = (uint8_t)((updated >> 8) & 0xFF);
        frame[SPI_FRAME_BYTE_DATA_LO] = (uint8_t)(updated & 0xFF);
        frame[SPI_FRAME_BYTE_CRC]     = computeCRC8(frame);
        transferFrame(frame, NULL);
    }

    xSemaphoreGive(_mutex);
    return true;
}
