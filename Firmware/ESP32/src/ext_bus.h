#pragma once

// =============================================================================
// ext_bus.h - External target bus engine for routed IO pins
//
// Keeps target I2C off the internal board I2C bus. The Python planner applies
// power/MUX/VLOGIC routing; this module owns ESP32 peripheral timing.
// =============================================================================

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

bool ext_i2c_setup(uint8_t sda_gpio, uint8_t scl_gpio, uint32_t frequency_hz, bool internal_pullups);
bool ext_i2c_ready(void);
void ext_i2c_get_status(bool *ready, uint8_t *sda_gpio, uint8_t *scl_gpio,
                        uint32_t *frequency_hz, bool *internal_pullups);
bool ext_i2c_scan(uint8_t start_addr, uint8_t stop_addr, bool skip_reserved,
                  uint8_t *out_addrs, size_t max_addrs, size_t *out_count,
                  uint16_t timeout_ms);
bool ext_i2c_write(uint8_t addr, const uint8_t *data, size_t len, uint16_t timeout_ms);
bool ext_i2c_read(uint8_t addr, uint8_t *data, size_t len, uint16_t timeout_ms);
bool ext_i2c_write_read(uint8_t addr, const uint8_t *wr_data, size_t wr_len,
                        uint8_t *rd_data, size_t rd_len, uint16_t timeout_ms);

bool ext_spi_setup(uint8_t sck_gpio, uint8_t mosi_gpio, uint8_t miso_gpio, uint8_t cs_gpio,
                   uint32_t frequency_hz, uint8_t mode);
bool ext_spi_ready(void);
void ext_spi_get_status(bool *ready, uint8_t *sck_gpio, uint8_t *mosi_gpio,
                        uint8_t *miso_gpio, uint8_t *cs_gpio,
                        uint32_t *frequency_hz, uint8_t *mode);
bool ext_spi_transfer(const uint8_t *tx_data, size_t tx_len,
                      uint8_t *rx_data, size_t *inout_rx_len,
                      uint16_t timeout_ms);

typedef enum {
    EXT_BUS_JOB_I2C_READ       = 1,
    EXT_BUS_JOB_I2C_WRITE_READ = 2,
    EXT_BUS_JOB_SPI_TRANSFER   = 3,
} ExtBusJobKind;

typedef enum {
    EXT_BUS_JOB_EMPTY   = 0,
    EXT_BUS_JOB_QUEUED  = 1,
    EXT_BUS_JOB_RUNNING = 2,
    EXT_BUS_JOB_DONE    = 3,
    EXT_BUS_JOB_ERROR   = 4,
} ExtBusJobStatus;

bool ext_job_submit_i2c_read(uint8_t addr, uint8_t read_len, uint16_t timeout_ms, uint32_t *job_id);
bool ext_job_submit_i2c_write_read(uint8_t addr, const uint8_t *wr_data, size_t wr_len,
                                   uint8_t read_len, uint16_t timeout_ms, uint32_t *job_id);
bool ext_job_submit_spi_transfer(const uint8_t *tx_data, size_t tx_len,
                                 uint16_t timeout_ms, uint32_t *job_id);
bool ext_job_get(uint32_t job_id, uint8_t *status, uint8_t *kind,
                 uint8_t *result, size_t max_result_len, size_t *result_len);

#ifdef __cplusplus
}
#endif
