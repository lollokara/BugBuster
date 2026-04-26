#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// ── Channel binding (Phase 3) ────────────────────────────────────────────────
bool bugbuster_mp_channel_set_function(uint8_t channel, int func);
bool bugbuster_mp_channel_set_voltage(uint8_t channel, float voltage, bool bipolar);
bool bugbuster_mp_channel_read_voltage(uint8_t channel, float *value_out);
bool bugbuster_mp_channel_set_do(uint8_t channel, bool value);

// ── I2C binding (Phase 4) ────────────────────────────────────────────────────
bool bugbuster_mp_i2c_setup(uint8_t sda_io, uint8_t scl_io, uint32_t freq_hz,
                             bool internal_pullups, float supply_v, float vlogic_v,
                             char *err, size_t err_len);
bool bugbuster_mp_i2c_scan(uint8_t start, uint8_t stop, bool skip_reserved,
                            uint16_t timeout_ms, uint8_t *out_addrs, size_t max,
                            size_t *count);
bool bugbuster_mp_i2c_write(uint8_t addr, const uint8_t *data, size_t len,
                             uint16_t timeout_ms);
bool bugbuster_mp_i2c_read(uint8_t addr, uint8_t *data, size_t len,
                            uint16_t timeout_ms);
bool bugbuster_mp_i2c_write_read(uint8_t addr, const uint8_t *wr, size_t wr_len,
                                  uint8_t *rd, size_t rd_len, uint16_t timeout_ms);

// ── SPI binding (Phase 4) ────────────────────────────────────────────────────
bool bugbuster_mp_spi_setup(uint8_t sck_io, uint8_t mosi_io, uint8_t miso_io,
                             uint8_t cs_io, uint32_t freq_hz, uint8_t mode,
                             float supply_v, float vlogic_v,
                             char *err, size_t err_len);
bool bugbuster_mp_spi_transfer(const uint8_t *tx, size_t tx_len,
                                uint8_t *rx, size_t *inout_rx_len,
                                uint16_t timeout_ms);

// ── Network binding (V2-E) — implemented in net_bridge.cpp ──────────────────
// (declarations live in net_bridge.h; this comment marks the boundary so
//  future phases know where to add analogous bridge groups.)

#ifdef __cplusplus
}
#endif
