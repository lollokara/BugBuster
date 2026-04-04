#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "config.h"
#include "ad74416h_regs.h"

// =============================================================================
// ad74416h_spi.h - Low-level SPI driver for the AD74416H (ESP-IDF native)
//
// SPI Frame Format (40-bit, MSB first, Mode 2: CPOL=1 CPHA=0):
//  Byte 0 (D39..D32): [D39:D38]=00 (write), [D37:D36]=dev_addr, [D35:D32]=reserved
//  Byte 1 (D31..D24): Register address
//  Byte 2 (D23..D16): Data high byte
//  Byte 3 (D15..D8):  Data low byte
//  Byte 4 (D7..D0):   CRC-8
//
// CRC-8: Polynomial 0x07, init 0x00, computed over D39..D8 (4 bytes)
//
// Locking strategy:
//   A single global recursive mutex (g_spi_bus_mutex, defined in adgs2414d.cpp)
//   serializes all SPI bus access.  AD74416H_SPI methods that require atomicity
//   across multiple frames (readRegister, updateRegister) hold the mutex for the
//   entire multi-frame sequence.  There is no per-device mutex; the bus mutex
//   already prevents interleaving because both AD74416H and ADGS2414D share the
//   same physical MOSI/MISO/SCLK lines.
// =============================================================================

#define SPI_FRAME_BYTES         5
#define SPI_FRAME_BYTE_CTRL     0
#define SPI_FRAME_BYTE_ADDR     1
#define SPI_FRAME_BYTE_DATA_HI  2
#define SPI_FRAME_BYTE_DATA_LO  3
#define SPI_FRAME_BYTE_CRC      4

#define SPI_CTRL_DEVADDR_SHIFT  4
#define CRC8_POLYNOMIAL         0x07
#define CRC8_INIT               0x00

class AD74416H_SPI {
public:
    AD74416H_SPI(gpio_num_t pin_sdo  = PIN_SDO,
                 gpio_num_t pin_sdi  = PIN_SDI,
                 gpio_num_t pin_sync = PIN_SYNC,
                 gpio_num_t pin_sclk = PIN_SCLK,
                 uint8_t dev_addr    = AD74416H_DEV_ADDR);

    void begin();

    /**
     * @brief Write a 16-bit value to a register.
     * @return true on success, false if the bus mutex timed out.
     */
    bool writeRegister(uint8_t addr, uint16_t data);

    /**
     * @brief Read a 16-bit register (two-phase: READ_SELECT + NOP).
     *
     * On failure (bus timeout or CRC error) *data is set to 0xFFFF.
     * @return true on success, false on failure.
     */
    bool readRegister(uint8_t addr, uint16_t* data);

    /**
     * @brief Atomic read-modify-write.
     * @return true on success, false on read failure or bus timeout.
     */
    bool updateRegister(uint8_t addr, uint16_t mask, uint16_t val);

    spi_device_handle_t getDeviceHandle() const { return _spi_dev; }

    /**
     * @brief Change SPI clock speed at runtime.
     * @param hz  New clock speed in Hz (max 20 MHz for AD74416H)
     * @return true on success
     */
    bool setClockSpeed(uint32_t hz);

    /** @brief Get current SPI clock speed in Hz. */
    uint32_t getClockSpeed() const { return _clock_hz; }

private:
    uint32_t _clock_hz = SPI_CLOCK_HZ;
    gpio_num_t _pin_sdo;
    gpio_num_t _pin_sdi;
    gpio_num_t _pin_sync;
    gpio_num_t _pin_sclk;
    uint8_t    _dev_addr;

    spi_device_handle_t _spi_dev;

    uint8_t computeCRC8(const uint8_t* frame) const;

    /**
     * @brief Transmit one 5-byte SPI frame.
     *
     * Caller MUST already hold g_spi_bus_mutex.  This is a raw helper;
     * public methods handle locking.
     *
     * @param tx_frame  5-byte frame to send
     * @param rx_frame  5-byte buffer for received data (may be NULL for write-only)
     */
    void transferFrame(const uint8_t* tx_frame, uint8_t* rx_frame);

    inline void assertSync()   { gpio_set_level(_pin_sync, 0); }
    inline void deassertSync() { gpio_set_level(_pin_sync, 1); }
};
