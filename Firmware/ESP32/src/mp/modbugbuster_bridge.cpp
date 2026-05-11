// =============================================================================
// modbugbuster_bridge.cpp — C ABI bridge from MicroPython bindings to C++ tasks.
// =============================================================================

#include "modbugbuster_bridge.h"

#include "tasks.h"
#include "bus_planner.h"
#include "ext_bus.h"
#include "scripting.h"
#include "io_owner.h"
#include "esp_timer.h"

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
                                        bool allow_split_supplies,
                                        char *err, size_t err_len)
{
    return bus_planner_apply_i2c(sda_io, scl_io, freq_hz, internal_pullups,
                                  supply_v, vlogic_v, allow_split_supplies, err, err_len);
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
                                        bool allow_split_supplies,
                                        char *err, size_t err_len)
{
    return bus_planner_apply_spi(sck_io, mosi_io, miso_io, cs_io,
                                  freq_hz, mode, supply_v, vlogic_v,
                                  allow_split_supplies, err, err_len);
}

extern "C" bool bugbuster_mp_spi_transfer(const uint8_t *tx, size_t tx_len,
                                           uint8_t *rx, size_t *inout_rx_len,
                                           uint16_t timeout_ms)
{
    return ext_spi_transfer(tx, tx_len, rx, inout_rx_len, timeout_ms);
}

// =============================================================================
// IO-ownership bridge (PR-5) — wraps the slot-by-slot io_owner API.
//
// The firmware io_owner.h API is per-slot: io_owner_acquire(slot, kind,
// session_id, token_fp32, lease_ms, now_ms).  We loop over all requested
// slots and return 0 (IO_OK) only when all succeed.
// =============================================================================

static uint64_t mp_now_ms(void)
{
    return (uint64_t)(esp_timer_get_time() / 1000LL);
}

extern "C" int bugbuster_mp_io_acquire(const uint8_t *slots, size_t n_slots,
                                        uint32_t lease_ms, uint32_t /*purpose_tag*/)
{
    uint8_t  session_id = scripting_get_mp_session();
    uint64_t now_ms     = mp_now_ms();

    for (size_t i = 0; i < n_slots; i++) {
        bool ok = io_owner_acquire(slots[i], IO_OWNER_SCRIPT,
                                   session_id, /*token_fp32=*/0,
                                   lease_ms, now_ms);
        if (!ok) return 1;  // IO_HELD_BY_OTHER
    }
    return 0;  // IO_OK
}

extern "C" int bugbuster_mp_io_release(const uint8_t *slots, size_t n_slots)
{
    uint8_t session_id = scripting_get_mp_session();

    if (slots == NULL || n_slots == 0) {
        // release-all: walk every slot and release any owned by this session
        for (uint8_t i = 0; i < IO_OWNER_NUM_SLOTS; i++) {
            io_owner_slot_t s = io_owner_get(i);
            if (s.kind == IO_OWNER_SCRIPT && s.session_id == session_id) {
                io_owner_release(i, session_id);
            }
        }
        return 0;
    }

    for (size_t i = 0; i < n_slots; i++) {
        io_owner_release(slots[i], session_id);
    }
    return 0;
}

extern "C" bool bugbuster_mp_io_status(uint8_t *kind_out, uint8_t *session_out,
                                        uint32_t *token_out, uint32_t *lease_lo_out,
                                        uint32_t * /*purpose_out — not in hw API*/,
                                        size_t n)
{
    if (n < 16) return false;
    io_owner_slot_t all[IO_OWNER_NUM_SLOTS];
    io_owner_get_all(all);
    for (size_t i = 0; i < 16; i++) {
        kind_out[i]     = (uint8_t)all[i].kind;
        session_out[i]  = all[i].session_id;
        token_out[i]    = all[i].token_fp32;
        lease_lo_out[i] = (uint32_t)(all[i].lease_until_ms & 0xFFFFFFFFu);
    }
    return true;
}
