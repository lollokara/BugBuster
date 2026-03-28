#pragma once

// =============================================================================
// i2c_bus.h - Shared I2C bus driver for BugBuster
//
// Manages ESP32 I2C master bus with mutex protection.
// Used by DS4424, HUSB238, and PCA9535 drivers.
// =============================================================================

#include <stdint.h>
#include <stdbool.h>
#include "driver/i2c_master.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize the I2C bus. Safe to call multiple times (only inits once).
 * @return true on success
 */
bool i2c_bus_init(void);

/**
 * @brief Check if I2C bus is initialized.
 */
bool i2c_bus_ready(void);

/**
 * @brief Write bytes to an I2C device.
 * @param addr   7-bit I2C slave address
 * @param data   Bytes to write
 * @param len    Number of bytes
 * @param timeout_ms  Timeout in ms
 * @return true on success (ACK received)
 */
bool i2c_bus_write(uint8_t addr, const uint8_t *data, size_t len, uint32_t timeout_ms);

/**
 * @brief Write then read from an I2C device (write-restart-read).
 * @param addr      7-bit I2C slave address
 * @param wr_data   Bytes to write (register address, etc.)
 * @param wr_len    Number of bytes to write
 * @param rd_data   Buffer for read data
 * @param rd_len    Number of bytes to read
 * @param timeout_ms  Timeout in ms
 * @return true on success
 */
bool i2c_bus_write_read(uint8_t addr, const uint8_t *wr_data, size_t wr_len,
                         uint8_t *rd_data, size_t rd_len, uint32_t timeout_ms);

/**
 * @brief Read bytes from an I2C device.
 * @param addr   7-bit I2C slave address
 * @param data   Buffer for read data
 * @param len    Number of bytes to read
 * @param timeout_ms  Timeout in ms
 * @return true on success
 */
bool i2c_bus_read(uint8_t addr, uint8_t *data, size_t len, uint32_t timeout_ms);

/**
 * @brief Probe an I2C address to check if a device is present.
 * @param addr  7-bit I2C slave address
 * @return true if device ACKs
 */
bool i2c_bus_probe(uint8_t addr);

#ifdef __cplusplus
}
#endif
