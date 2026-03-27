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
    void writeRegister(uint8_t addr, uint16_t data);
    bool readRegister(uint8_t addr, uint16_t* data);
    bool updateRegister(uint8_t addr, uint16_t mask, uint16_t val);

private:
    gpio_num_t _pin_sdo;
    gpio_num_t _pin_sdi;
    gpio_num_t _pin_sync;
    gpio_num_t _pin_sclk;
    uint8_t    _dev_addr;

    spi_device_handle_t _spi_dev;
    SemaphoreHandle_t   _mutex;

    uint8_t computeCRC8(const uint8_t* frame) const;
    void transferFrame(const uint8_t* tx_frame, uint8_t* rx_frame);

    inline void assertSync()   { gpio_set_level(_pin_sync, 0); }
    inline void deassertSync() { gpio_set_level(_pin_sync, 1); }
};
