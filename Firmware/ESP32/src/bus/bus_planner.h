#pragma once

// =============================================================================
// bus_planner.h — On-device IO-terminal bus routing engine (Phase 4).
//
// SYNC: keep aligned with python/bugbuster/hal.py DEFAULT_ROUTING +
//       bus.py _plan/_apply_route.  When the PCB routing table or host-side
//       planner logic changes, update this file to match.
// =============================================================================

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Set up an I2C bus on the given IO terminal numbers.
 *
 * Validates IOs, computes MUX state, enables power (VADJ, e-fuse, level-
 * shifter OE, VLOGIC), programs the MUX, and calls ext_i2c_setup().
 * All steps are protected by an internal planner mutex.
 *
 * @param sda_io          IO terminal 1..12 for SDA
 * @param scl_io          IO terminal 1..12 for SCL
 * @param freq_hz         I2C frequency in Hz
 * @param internal_pullups true = enable ESP32 internal pull-ups
 * @param supply_v        Target supply voltage (VADJ), max 5.0 V
 * @param vlogic_v        Target VLOGIC voltage
 * @param err             Error message buffer (may be NULL)
 * @param err_len         Size of error buffer
 * @return true on success; false with err filled on failure
 */
bool bus_planner_apply_i2c(uint8_t sda_io, uint8_t scl_io,
                            uint32_t freq_hz, bool internal_pullups,
                            float supply_v, float vlogic_v,
                            bool allow_split_supplies,
                            char *err, size_t err_len);

/**
 * @brief Set up a SPI bus on the given IO terminal numbers.
 *
 * Use 0 for mosi_io_or_0, miso_io_or_0, cs_io_or_0 when that signal is
 * not needed (ext_bus uses 0xFF internally as the "no-pin" sentinel;
 * this wrapper converts 0 → 0xFF before calling ext_spi_setup).
 *
 * @param sck_io          IO terminal 1..12 for SCK
 * @param mosi_io_or_0    IO terminal 1..12 for MOSI, or 0 = unused
 * @param miso_io_or_0    IO terminal 1..12 for MISO, or 0 = unused
 * @param cs_io_or_0      IO terminal 1..12 for CS,   or 0 = unused
 * @param freq_hz         SPI frequency in Hz
 * @param mode            SPI mode 0..3
 * @param supply_v        Target supply voltage (VADJ), max 5.0 V
 * @param vlogic_v        Target VLOGIC voltage
 * @param err             Error message buffer (may be NULL)
 * @param err_len         Size of error buffer
 * @return true on success
 */
bool bus_planner_apply_spi(uint8_t sck_io,
                            uint8_t mosi_io_or_0, uint8_t miso_io_or_0,
                            uint8_t cs_io_or_0,
                            uint32_t freq_hz, uint8_t mode,
                            float supply_v, float vlogic_v,
                            bool allow_split_supplies,
                            char *err, size_t err_len);

/**
 * @brief Route a single IO terminal as a digital input (no ext_bus setup).
 *
 * Enables MUX power, level-shifter OE, VLOGIC (3.3 V), VADJ supply and
 * e-fuse for the given IO, then programs the MUX switch in digital-high-
 * drive mode so the ESP32 GPIO can read the external signal.
 *
 * @param io_num  IO terminal 1..12
 * @param err     Error message buffer (may be NULL)
 * @param err_len Size of error buffer
 * @return true on success; false with err filled on failure
 */
bool bus_planner_route_digital_input(uint8_t io_num, char *err, size_t err_len);

#ifdef __cplusplus
}
#endif
