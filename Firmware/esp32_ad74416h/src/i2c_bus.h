#pragma once

// =============================================================================
// i2c_bus.h - Shared I2C bus driver for BugBuster
//
// Manages ESP32 I2C master bus with mutex protection.
// Used by DS4424, HUSB238, and PCA9535 drivers.
// =============================================================================

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

bool i2c_bus_init(void);
bool i2c_bus_ready(void);
bool i2c_bus_write(uint8_t addr, const uint8_t *data, size_t len, uint32_t timeout_ms);
bool i2c_bus_write_read(uint8_t addr, const uint8_t *wr_data, size_t wr_len,
                         uint8_t *rd_data, size_t rd_len, uint32_t timeout_ms);
bool i2c_bus_read(uint8_t addr, uint8_t *data, size_t len, uint32_t timeout_ms);
bool i2c_bus_probe(uint8_t addr);

#ifdef __cplusplus
}
#endif
