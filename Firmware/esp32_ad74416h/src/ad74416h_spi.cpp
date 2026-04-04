// =============================================================================
// ad74416h_spi.cpp - AD74416H SPI driver (ESP-IDF native)
// =============================================================================

#include "ad74416h_spi.h"
#include "esp_log.h"
#include <string.h>

// Shared SPI2 bus mutex defined in adgs2414d.cpp.
// AD74416H and ADGS2414D must serialize on the same lock because they share
// MOSI/MISO/SCLK with different chip selects.
//
// This single mutex replaces the previous two-mutex scheme (_mutex +
// g_spi_bus_mutex).  Collapsing to one lock eliminates a subtle ordering
// hazard (ABBA potential) and guarantees that the two-phase read sequence
// (READ_SELECT + NOP) is atomic with respect to ALL other bus users, not just
// other AD74416H callers.
extern SemaphoreHandle_t g_spi_bus_mutex;

// Bus-lock timeout.  500 ms is generous; any longer indicates a stuck task.
static constexpr TickType_t BUS_TIMEOUT = pdMS_TO_TICKS(500);

AD74416H_SPI::AD74416H_SPI(gpio_num_t pin_sdo, gpio_num_t pin_sdi,
                             gpio_num_t pin_sync, gpio_num_t pin_sclk,
                             uint8_t dev_addr)
    : _pin_sdo(pin_sdo),
      _pin_sdi(pin_sdi),
      _pin_sync(pin_sync),
      _pin_sclk(pin_sclk),
      _dev_addr(dev_addr & 0x03),
      _spi_dev(NULL)
{
}

void AD74416H_SPI::begin()
{
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

// ---------------------------------------------------------------------------
// transferFrame — send/receive one 5-byte SPI frame.
// Caller MUST already hold g_spi_bus_mutex.
// ---------------------------------------------------------------------------
void AD74416H_SPI::transferFrame(const uint8_t* tx_frame, uint8_t* rx_frame)
{
    spi_transaction_t txn = {};
    txn.length    = SPI_FRAME_BYTES * 8;  // bits
    txn.tx_buffer = tx_frame;

    // dummy_rx lives at function scope so it survives until after the transfer.
    uint8_t dummy_rx[SPI_FRAME_BYTES];

    if (rx_frame != NULL) {
        txn.rx_buffer = rx_frame;
    } else {
        txn.rx_buffer = dummy_rx;
    }

    assertSync();
    spi_device_polling_transmit(_spi_dev, &txn);
    deassertSync();
}

// ---------------------------------------------------------------------------
// writeRegister
// ---------------------------------------------------------------------------
bool AD74416H_SPI::writeRegister(uint8_t addr, uint16_t data)
{
    uint8_t frame[SPI_FRAME_BYTES];
    frame[SPI_FRAME_BYTE_CTRL]    = (uint8_t)((_dev_addr & 0x03) << SPI_CTRL_DEVADDR_SHIFT);
    frame[SPI_FRAME_BYTE_ADDR]    = addr;
    frame[SPI_FRAME_BYTE_DATA_HI] = (uint8_t)((data >> 8) & 0xFF);
    frame[SPI_FRAME_BYTE_DATA_LO] = (uint8_t)(data & 0xFF);
    frame[SPI_FRAME_BYTE_CRC]     = computeCRC8(frame);

    if (g_spi_bus_mutex == NULL ||
        xSemaphoreTakeRecursive(g_spi_bus_mutex, BUS_TIMEOUT) != pdTRUE) {
        ESP_LOGE("spi", "writeRegister(0x%02X): bus timeout", addr);
        return false;
    }

    transferFrame(frame, NULL);

    xSemaphoreGiveRecursive(g_spi_bus_mutex);
    return true;
}

// ---------------------------------------------------------------------------
// readRegister — two-phase read held under a single mutex acquisition so the
// READ_SELECT + NOP pair cannot be interleaved by any other bus user.
// ---------------------------------------------------------------------------
bool AD74416H_SPI::readRegister(uint8_t addr, uint16_t* data)
{
    if (g_spi_bus_mutex == NULL ||
        xSemaphoreTakeRecursive(g_spi_bus_mutex, BUS_TIMEOUT) != pdTRUE) {
        ESP_LOGE("spi", "readRegister(0x%02X): bus timeout", addr);
        if (data != NULL) *data = 0xFFFF;
        return false;
    }

    // Phase 1: Write register address to READ_SELECT
    {
        uint8_t frame[SPI_FRAME_BYTES];
        frame[SPI_FRAME_BYTE_CTRL]    = (uint8_t)((_dev_addr & 0x03) << SPI_CTRL_DEVADDR_SHIFT);
        frame[SPI_FRAME_BYTE_ADDR]    = REG_READ_SELECT;
        frame[SPI_FRAME_BYTE_DATA_HI] = 0x00;
        frame[SPI_FRAME_BYTE_DATA_LO] = addr;
        frame[SPI_FRAME_BYTE_CRC]     = computeCRC8(frame);
        transferFrame(frame, NULL);
    }

    // Phase 2: NOP to clock in response
    uint8_t tx_frame[SPI_FRAME_BYTES] = {0};
    uint8_t rx_frame[SPI_FRAME_BYTES] = {0};
    tx_frame[SPI_FRAME_BYTE_CTRL] = (uint8_t)((_dev_addr & 0x03) << SPI_CTRL_DEVADDR_SHIFT);
    tx_frame[SPI_FRAME_BYTE_ADDR] = REG_NOP;
    tx_frame[SPI_FRAME_BYTE_CRC]  = computeCRC8(tx_frame);
    transferFrame(tx_frame, rx_frame);

    xSemaphoreGiveRecursive(g_spi_bus_mutex);

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

// ---------------------------------------------------------------------------
// updateRegister — atomic read-modify-write under one bus lock.
// ---------------------------------------------------------------------------
bool AD74416H_SPI::updateRegister(uint8_t addr, uint16_t mask, uint16_t val)
{
    if (g_spi_bus_mutex == NULL ||
        xSemaphoreTakeRecursive(g_spi_bus_mutex, BUS_TIMEOUT) != pdTRUE) {
        ESP_LOGE("spi", "updateRegister(0x%02X): bus timeout", addr);
        return false;
    }

    // Phase 1: READ_SELECT
    {
        uint8_t frame[SPI_FRAME_BYTES];
        frame[SPI_FRAME_BYTE_CTRL]    = (uint8_t)((_dev_addr & 0x03) << SPI_CTRL_DEVADDR_SHIFT);
        frame[SPI_FRAME_BYTE_ADDR]    = REG_READ_SELECT;
        frame[SPI_FRAME_BYTE_DATA_HI] = 0x00;
        frame[SPI_FRAME_BYTE_DATA_LO] = addr;
        frame[SPI_FRAME_BYTE_CRC]     = computeCRC8(frame);
        transferFrame(frame, NULL);
    }

    // Phase 2: NOP — read current value
    uint8_t tx_nop[SPI_FRAME_BYTES] = {0};
    uint8_t rx_frame[SPI_FRAME_BYTES] = {0};
    tx_nop[SPI_FRAME_BYTE_CTRL] = (uint8_t)((_dev_addr & 0x03) << SPI_CTRL_DEVADDR_SHIFT);
    tx_nop[SPI_FRAME_BYTE_ADDR] = REG_NOP;
    tx_nop[SPI_FRAME_BYTE_CRC]  = computeCRC8(tx_nop);
    transferFrame(tx_nop, rx_frame);

    uint8_t expected_crc = computeCRC8(rx_frame);
    if (rx_frame[SPI_FRAME_BYTE_CRC] != expected_crc) {
        xSemaphoreGiveRecursive(g_spi_bus_mutex);
        return false;
    }

    uint16_t current = (uint16_t)(((uint16_t)rx_frame[SPI_FRAME_BYTE_DATA_HI] << 8) |
                                   (uint16_t)rx_frame[SPI_FRAME_BYTE_DATA_LO]);

    // Phase 3: Modify and write back
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

    xSemaphoreGiveRecursive(g_spi_bus_mutex);
    return true;
}
