// =============================================================================
// modbugbuster_bridge.cpp — C ABI bridge from MicroPython bindings to C++ tasks.
// =============================================================================

#include "modbugbuster_bridge.h"

#include "tasks.h"
#include "bus_planner.h"
#include "ext_bus.h"

extern "C" bool bugbuster_mp_channel_set_function(uint8_t channel, int func)
{
    if (channel >= 4) return false;
    tasks_apply_channel_function(channel, (ChannelFunction)func);
    return true;
}

extern "C" bool bugbuster_mp_channel_set_voltage(uint8_t channel, float voltage, bool bipolar)
{
    if (channel >= 4) return false;
    return tasks_apply_dac_voltage(channel, voltage, bipolar);
}

extern "C" bool bugbuster_mp_channel_read_voltage(uint8_t channel, float *value_out)
{
    if (channel >= 4 || !value_out || g_stateMutex == NULL) return false;
    if (xSemaphoreTake(g_stateMutex, pdMS_TO_TICKS(50)) != pdTRUE) return false;
    *value_out = g_deviceState.channels[channel].adcValue;
    xSemaphoreGive(g_stateMutex);
    return true;
}

extern "C" bool bugbuster_mp_channel_set_do(uint8_t channel, bool value)
{
    if (channel >= 4) return false;

    Command cmd = {};
    cmd.type = CMD_DO_SET;
    cmd.channel = channel;
    cmd.boolVal = value;
    return sendCommand(cmd);
}

// =============================================================================
// I2C bridge (Phase 4) — thin wrappers over bus_planner + ext_bus
// =============================================================================

extern "C" bool bugbuster_mp_i2c_setup(uint8_t sda_io, uint8_t scl_io,
                                        uint32_t freq_hz, bool internal_pullups,
                                        float supply_v, float vlogic_v,
                                        char *err, size_t err_len)
{
    return bus_planner_apply_i2c(sda_io, scl_io, freq_hz, internal_pullups,
                                  supply_v, vlogic_v, err, err_len);
}

extern "C" bool bugbuster_mp_i2c_scan(uint8_t start, uint8_t stop,
                                       bool skip_reserved, uint16_t timeout_ms,
                                       uint8_t *out_addrs, size_t max, size_t *count)
{
    return ext_i2c_scan(start, stop, skip_reserved, out_addrs, max, count, timeout_ms);
}

extern "C" bool bugbuster_mp_i2c_write(uint8_t addr, const uint8_t *data,
                                        size_t len, uint16_t timeout_ms)
{
    return ext_i2c_write(addr, data, len, timeout_ms);
}

extern "C" bool bugbuster_mp_i2c_read(uint8_t addr, uint8_t *data,
                                       size_t len, uint16_t timeout_ms)
{
    return ext_i2c_read(addr, data, len, timeout_ms);
}

extern "C" bool bugbuster_mp_i2c_write_read(uint8_t addr,
                                             const uint8_t *wr, size_t wr_len,
                                             uint8_t *rd, size_t rd_len,
                                             uint16_t timeout_ms)
{
    return ext_i2c_write_read(addr, wr, wr_len, rd, rd_len, timeout_ms);
}

// =============================================================================
// SPI bridge (Phase 4) — thin wrappers over bus_planner + ext_bus
// =============================================================================

extern "C" bool bugbuster_mp_spi_setup(uint8_t sck_io, uint8_t mosi_io,
                                        uint8_t miso_io, uint8_t cs_io,
                                        uint32_t freq_hz, uint8_t mode,
                                        float supply_v, float vlogic_v,
                                        char *err, size_t err_len)
{
    return bus_planner_apply_spi(sck_io, mosi_io, miso_io, cs_io,
                                  freq_hz, mode, supply_v, vlogic_v,
                                  err, err_len);
}

extern "C" bool bugbuster_mp_spi_transfer(const uint8_t *tx, size_t tx_len,
                                           uint8_t *rx, size_t *inout_rx_len,
                                           uint16_t timeout_ms)
{
    return ext_spi_transfer(tx, tx_len, rx, inout_rx_len, timeout_ms);
}
