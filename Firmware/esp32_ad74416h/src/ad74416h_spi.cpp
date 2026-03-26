#include "ad74416h_spi.h"

// =============================================================================
// ad74416h_spi.cpp - AD74416H low-level SPI driver implementation
// =============================================================================

// ---------------------------------------------------------------------------
// Constructor
// ---------------------------------------------------------------------------
AD74416H_SPI::AD74416H_SPI(uint8_t pin_sdo, uint8_t pin_sdi,
                             uint8_t pin_sync, uint8_t pin_sclk,
                             uint8_t dev_addr)
    : _pin_sdo(pin_sdo),
      _pin_sdi(pin_sdi),
      _pin_sync(pin_sync),
      _pin_sclk(pin_sclk),
      _dev_addr(dev_addr & 0x03),
      _spi(nullptr)
{
}

// ---------------------------------------------------------------------------
// begin() - Initialise SPI bus and GPIO
// ---------------------------------------------------------------------------
void AD74416H_SPI::begin()
{
    // SYNC is active low; idle state is HIGH
    pinMode(_pin_sync, OUTPUT);
    deassertSync();

    // Initialise the VSPI/SPI bus with explicit pin assignment
    // ESP32-S3 Arduino core supports SPI.begin(sclk, miso, mosi, ss=-1)
    _spi = &SPI;
    _spi->begin(_pin_sclk, _pin_sdo, _pin_sdi, -1);  // -1 = we control CS manually
}

// ---------------------------------------------------------------------------
// computeCRC8() - CRC-8 with polynomial 0x07, init 0x00
//
// Computed over the first 4 bytes of the 40-bit frame (D39..D8).
// The CRC is appended as the 5th byte (D7..D0).
// ---------------------------------------------------------------------------
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
// transferFrame() - Clock one 5-byte SPI frame out and receive response
// ---------------------------------------------------------------------------
void AD74416H_SPI::transferFrame(const uint8_t* tx_frame, uint8_t* rx_frame)
{
    _spi->beginTransaction(SPISettings(SPI_CLOCK_HZ, MSBFIRST, SPI_MODE2));
    assertSync();

    for (uint8_t i = 0; i < SPI_FRAME_BYTES; i++) {
        uint8_t rx_byte = _spi->transfer(tx_frame[i]);
        if (rx_frame != nullptr) {
            rx_frame[i] = rx_byte;
        }
    }

    deassertSync();
    _spi->endTransaction();
}

// ---------------------------------------------------------------------------
// writeRegister() - Build and send a write frame
//
// Frame layout:
//  byte[0]: [7:6]=00 (write), [5:4]=dev_addr, [3:0]=0000 (reserved)
//  byte[1]: register address
//  byte[2]: data[15:8]
//  byte[3]: data[7:0]
//  byte[4]: CRC-8
// ---------------------------------------------------------------------------
void AD74416H_SPI::writeRegister(uint8_t addr, uint16_t data)
{
    uint8_t frame[SPI_FRAME_BYTES];

    // Byte 0: R/W=0 (write), device address in bits [5:4], reserved bits clear
    frame[SPI_FRAME_BYTE_CTRL]    = (uint8_t)((_dev_addr & 0x03) << SPI_CTRL_DEVADDR_SHIFT);
    frame[SPI_FRAME_BYTE_ADDR]    = addr;
    frame[SPI_FRAME_BYTE_DATA_HI] = (uint8_t)((data >> 8) & 0xFF);
    frame[SPI_FRAME_BYTE_DATA_LO] = (uint8_t)(data & 0xFF);
    frame[SPI_FRAME_BYTE_CRC]     = computeCRC8(frame);

    transferFrame(frame, nullptr);
}

// ---------------------------------------------------------------------------
// readRegister() - Two-stage SPI readback with CRC validation
//
// Stage 1: Write address of target register to READ_SELECT (0x6E)
// Stage 2: Send NOP (all zeros); AD74416H returns register data on SDO
//
// Response frame SDO byte layout:
//  byte[0]: [7]=1 (read flag), [6]=0, [5:4]=dev_addr, [3:0]=status nibble
//  byte[1]: register address that was read
//  byte[2]: data[15:8]
//  byte[3]: data[7:0]
//  byte[4]: CRC-8 of bytes[0..3]
// ---------------------------------------------------------------------------
bool AD74416H_SPI::readRegister(uint8_t addr, uint16_t* data)
{
    // --- Stage 1: Write register address to READ_SELECT ---
    writeRegister(REG_READ_SELECT, (uint16_t)addr);

    // --- Stage 2: Send NOP; clock in response ---
    uint8_t tx_frame[SPI_FRAME_BYTES] = {0};
    uint8_t rx_frame[SPI_FRAME_BYTES] = {0};

    // Build a NOP write frame (addr=0x00, data=0x0000) with valid CRC
    tx_frame[SPI_FRAME_BYTE_CTRL]    = (uint8_t)((_dev_addr & 0x03) << SPI_CTRL_DEVADDR_SHIFT);
    tx_frame[SPI_FRAME_BYTE_ADDR]    = REG_NOP;
    tx_frame[SPI_FRAME_BYTE_DATA_HI] = 0x00;
    tx_frame[SPI_FRAME_BYTE_DATA_LO] = 0x00;
    tx_frame[SPI_FRAME_BYTE_CRC]     = computeCRC8(tx_frame);

    transferFrame(tx_frame, rx_frame);

    // --- Validate CRC of response ---
    uint8_t expected_crc = computeCRC8(rx_frame);
    if (rx_frame[SPI_FRAME_BYTE_CRC] != expected_crc) {
        // CRC mismatch - data unreliable
        if (data != nullptr) {
            *data = 0xFFFF;  // Sentinel value indicating error
        }
        return false;
    }

    // Extract 16-bit data from response bytes [2] and [3]
    if (data != nullptr) {
        *data = (uint16_t)(((uint16_t)rx_frame[SPI_FRAME_BYTE_DATA_HI] << 8) |
                            (uint16_t)rx_frame[SPI_FRAME_BYTE_DATA_LO]);
    }
    return true;
}

// ---------------------------------------------------------------------------
// updateRegister() - Read-modify-write a register
// ---------------------------------------------------------------------------
bool AD74416H_SPI::updateRegister(uint8_t addr, uint16_t mask, uint16_t val)
{
    uint16_t current = 0;
    bool ok = readRegister(addr, &current);
    if (!ok) {
        return false;
    }
    uint16_t updated = (current & ~mask) | (val & mask);
    writeRegister(addr, updated);
    return true;
}
