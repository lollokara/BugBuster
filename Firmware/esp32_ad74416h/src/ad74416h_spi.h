#pragma once

#include <Arduino.h>
#include <SPI.h>
#include <stdint.h>
#include <stdbool.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include "config.h"
#include "ad74416h_regs.h"

// =============================================================================
// ad74416h_spi.h - Low-level SPI driver for the AD74416H
//
// SPI Frame Format (40-bit, MSB first, Mode 2: CPOL=1 CPHA=0):
//
//  Byte 4 (D39..D32):  [D39:D38]=00 (write), [D37:D36]=dev_addr, [D35:D32]=0000 (reserved)
//  Byte 3 (D31..D24):  Register address [7:0]
//  Byte 2 (D23..D16):  Data high byte [15:8]
//  Byte 1 (D15..D8 ):  Data low  byte [7:0]
//  Byte 0 (D7 ..D0 ):  CRC-8 over bytes [4..1] (D39..D8)
//
// Read Sequence (2-stage readback):
//   Stage 1: Write register address to READ_SELECT (0x6E)
//   Stage 2: Send NOP (0x00 / any write); response on SDO:
//            [D39]=1, [D38]=0, [D37:D36]=addr, [D35:D32]=status,
//            [D31:D24]=reg_addr, [D23:D8]=data, [D7:D0]=CRC8
//
// CRC-8: Polynomial 0x07 (x^8+x^2+x+1), init=0x00, computed over D39..D8 (4 bytes)
// =============================================================================

// Frame byte indices (index 0 = first byte transmitted = most significant)
#define SPI_FRAME_BYTES         5
#define SPI_FRAME_BYTE_CTRL     0   // D39..D32 - R/W, dev addr, reserved
#define SPI_FRAME_BYTE_ADDR     1   // D31..D24 - Register address
#define SPI_FRAME_BYTE_DATA_HI  2   // D23..D16 - Data MSB
#define SPI_FRAME_BYTE_DATA_LO  3   // D15..D8  - Data LSB
#define SPI_FRAME_BYTE_CRC      4   // D7..D0   - CRC-8

// Control byte bit fields
#define SPI_CTRL_RW_SHIFT       7   // Bit 7 of byte 0: 0=write, 1=read (in response)
#define SPI_CTRL_DEVADDR_SHIFT  4   // Bits [5:4] of byte 0: device address [1:0]
#define SPI_CTRL_WRITE          0x00
#define SPI_CTRL_READ_FLAG      0x80  // Set in response frame byte 0

// CRC-8 parameters
#define CRC8_POLYNOMIAL         0x07
#define CRC8_INIT               0x00

// Status nibble mask in read-response control byte (bits [3:0] of byte 0)
#define SPI_RESP_STATUS_MASK    0x0F

// =============================================================================
// Class AD74416H_SPI
// =============================================================================

class AD74416H_SPI {
public:
    /**
     * @brief Construct a new AD74416H_SPI driver.
     *
     * @param pin_sdo   MISO pin (GPIO 8)
     * @param pin_sdi   MOSI pin (GPIO 9)
     * @param pin_sync  Chip-select pin, active low (GPIO 10)
     * @param pin_sclk  SPI clock pin (GPIO 11)
     * @param dev_addr  Device address [1:0] (AD0/AD1 state, default 0)
     */
    AD74416H_SPI(uint8_t pin_sdo  = PIN_SDO,
                 uint8_t pin_sdi  = PIN_SDI,
                 uint8_t pin_sync = PIN_SYNC,
                 uint8_t pin_sclk = PIN_SCLK,
                 uint8_t dev_addr = AD74416H_DEV_ADDR);

    /**
     * @brief Initialise the SPI bus and configure GPIO pins.
     *        Call once from setup().
     */
    void begin();

    /**
     * @brief Write a 16-bit value to a register.
     *
     * @param addr  8-bit register address
     * @param data  16-bit value to write
     */
    void writeRegister(uint8_t addr, uint16_t data);

    /**
     * @brief Read a 16-bit value from a register using 2-stage readback.
     *
     * Stage 1 writes the register address to READ_SELECT.
     * Stage 2 sends NOP; the AD74416H returns the register data on SDO.
     * The CRC of the response frame is validated.
     *
     * @param addr  8-bit register address to read
     * @param data  Pointer to store the 16-bit result
     * @return true   if CRC validated successfully
     * @return false  if CRC mismatch (data may be unreliable)
     */
    bool readRegister(uint8_t addr, uint16_t* data);

    /**
     * @brief Read-modify-write a register (bitfield update).
     *
     * Reads the current register value, clears bits indicated by mask,
     * ORs in (val & mask), then writes back.
     *
     * @param addr  Register address
     * @param mask  Bit-mask of fields to update (1 = update this bit)
     * @param val   New values for the masked bits (pre-shifted)
     * @return true   if the read stage CRC was valid
     * @return false  if CRC mismatch during read
     */
    bool updateRegister(uint8_t addr, uint16_t mask, uint16_t val);

private:
    uint8_t _pin_sdo;
    uint8_t _pin_sdi;
    uint8_t _pin_sync;
    uint8_t _pin_sclk;
    uint8_t _dev_addr;

    SPIClass* _spi;
    SemaphoreHandle_t _mutex;   // Protects multi-frame SPI sequences

    /**
     * @brief Compute CRC-8 (poly 0x07, init 0x00) over the first 4 bytes of
     *        a 5-byte SPI frame (D39..D8).
     *
     * @param frame Pointer to 5-byte frame buffer
     * @return uint8_t Computed CRC
     */
    uint8_t computeCRC8(const uint8_t* frame) const;

    /**
     * @brief Assert SYNC (drive low) to begin an SPI frame.
     */
    inline void assertSync()   { digitalWrite(_pin_sync, LOW);  }

    /**
     * @brief Deassert SYNC (drive high) to end an SPI frame.
     */
    inline void deassertSync() { digitalWrite(_pin_sync, HIGH); }

    /**
     * @brief Transfer a single 5-byte frame and receive a 5-byte response.
     *
     * Asserts SYNC, clocks 5 bytes out/in, then deasserts SYNC.
     *
     * @param tx_frame  Transmit buffer (5 bytes)
     * @param rx_frame  Receive buffer  (5 bytes); may be nullptr for write-only
     */
    void transferFrame(const uint8_t* tx_frame, uint8_t* rx_frame);
};
