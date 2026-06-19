#pragma once

// =============================================================================
// adaq7769_ll.h — Low-level SPI transport for the ADAQ7769-1
//
// Responsibilities:
//   - Own the ESP-IDF spi_master device handles for one ADAQ (a "config" handle
//     at a conservative clock for register access, and a "data" handle at the
//     fast readout clock for sample reads).
//   - Encode/decode the SPI Mode-3 register frames (instruction byte + data).
//   - Implement the ADAQ7769-1 CRC-8 (poly x^8+x^2+x+1) used on SPI transfers.
//
// Higher layers (adaq7769.* and adaq7769_stream.*) build on these primitives.
// This layer does NOT touch RESET/DRDY/SYNC GPIOs — that is the HAL's job.
// =============================================================================

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "esp_err.h"

#include "adaq7769_regs.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    spi_host_device_t   host;
    gpio_num_t          cs_pin;
    spi_device_handle_t dev_cfg;     // slow clock, register access
    spi_device_handle_t dev_data;    // fast clock, sample readout
    uint32_t            cfg_hz;
    uint32_t            data_hz;
    bool                crc_enabled;  // mirrors INTERFACE_FORMAT.EN_SPI_CRC
    bool                crc_xor;      // mirrors INTERFACE_FORMAT.CRC_TYPE (read XOR)
} adaq_ll_t;

// -----------------------------------------------------------------------------
// Bus / device lifecycle
// -----------------------------------------------------------------------------

/**
 * @brief Initialise a shared SPI bus (call once per physical bus).
 *
 * @param host           SPI2_HOST or SPI3_HOST.
 * @param sclk,mosi,miso Bus pins.
 * @param max_xfer_bytes Largest single DMA transfer expected (sizing the bus).
 * @return ESP_OK on success.
 */
esp_err_t adaq_ll_bus_init(spi_host_device_t host,
                           gpio_num_t sclk, gpio_num_t mosi, gpio_num_t miso,
                           int max_xfer_bytes);

/**
 * @brief Attach one ADAQ device to an already-initialised bus.
 *
 * Registers two device handles on the same CS line: one at @p cfg_hz for
 * register access and one at @p data_hz for fast sample readout. Both use
 * SPI Mode 3 (CPOL=1, CPHA=1).
 */
esp_err_t adaq_ll_add_device(adaq_ll_t *ll, spi_host_device_t host,
                             gpio_num_t cs_pin, uint32_t cfg_hz, uint32_t data_hz);

// -----------------------------------------------------------------------------
// Register access (uses the cfg handle, blocking/polling)
// -----------------------------------------------------------------------------

/** @brief Write one 8-bit register. Honors CRC if adaq_ll_set_crc() enabled it. */
esp_err_t adaq_ll_write_reg(adaq_ll_t *ll, uint8_t addr, uint8_t val);

/** @brief Read one 8-bit register. *val unchanged on error. */
esp_err_t adaq_ll_read_reg(adaq_ll_t *ll, uint8_t addr, uint8_t *val);

/** @brief Read the 24-bit ADC_DATA register as a single (instructed) read. */
esp_err_t adaq_ll_read_adc24(adaq_ll_t *ll, int32_t *sample);

/** @brief Send raw bytes on the cfg handle (e.g. continuous-read exit key). */
esp_err_t adaq_ll_write_raw(adaq_ll_t *ll, const uint8_t *tx, size_t len);

/**
 * @brief One continuous-read-mode word transfer on the DATA handle.
 *
 * In continuous read mode no instruction byte is sent: the device drives the
 * (24-bit data [+ 8 status] [+ 8 CRC]) out on SCLK. @p len_bytes is the total
 * payload length (3, 4 or 5). @p tx may be NULL (idle high recommended).
 */
esp_err_t adaq_ll_contread_word(adaq_ll_t *ll, uint8_t *rx, size_t len_bytes);

// -----------------------------------------------------------------------------
// CRC helpers
// -----------------------------------------------------------------------------

/** @brief Track whether SPI CRC is active so register access can append/verify. */
void adaq_ll_set_crc(adaq_ll_t *ll, bool enabled, bool xor_mode);

/** @brief ADAQ7769-1 CRC-8 over @p len bytes, given an initial value. */
uint8_t adaq_ll_crc8(const uint8_t *data, size_t len, uint8_t init);

/** @brief Sign-extend a 24-bit twos-complement word to int32. */
static inline int32_t adaq_sign_extend24(uint32_t raw24)
{
    return (raw24 & 0x800000u) ? (int32_t)(raw24 | 0xFF000000u) : (int32_t)raw24;
}

#ifdef __cplusplus
}
#endif
