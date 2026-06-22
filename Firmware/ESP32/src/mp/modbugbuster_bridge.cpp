// =============================================================================
// modbugbuster_bridge.cpp — C ABI bridge from MicroPython bindings to C++ tasks.
// =============================================================================

#include "modbugbuster_bridge.h"

#include "tasks.h"
#include "bus_planner.h"
#include "ext_bus.h"
#include "scripting.h"
#include "io_owner.h"
#include "pd_vadj_guard.h"
#include "hat.h"
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

extern "C" bool bugbuster_mp_vadj_pd_warning(uint8_t rail, float requested_v,
                                             char *warning, size_t warning_len)
{
    return pd_vadj_guard_warning(rail, requested_v, warning, warning_len);
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

extern "C" bool bugbuster_mp_i2c_close(void)
{
    return ext_i2c_close();
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

extern "C" bool bugbuster_mp_spi_close(void)
{
    return ext_spi_close();
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

// =============================================================================
// HAT v2 bridge — script-friendly wrappers over the existing ESP32 HAT API.
// =============================================================================

extern "C" bool bugbuster_mp_hat_status(bugbuster_mp_hat_status_t *out)
{
    if (!out) return false;
    const HatState *st = hat_get_state();
    if (!st) return false;

    out->detected = st->detected;
    out->connected = st->connected;
    out->type = (uint8_t)st->type;
    out->detect_voltage = st->detect_voltage;
    out->fw_major = st->fw_version_major;
    out->fw_minor = st->fw_version_minor;
    out->config_confirmed = st->config_confirmed;
    out->io_voltage_mv = st->io_voltage_mv;
    out->caps_valid = st->caps_valid;
    out->la_route = st->la_route;
    out->dap_connected = st->dap_connected;
    out->target_detected = st->target_detected;
    out->target_dpidr = st->target_dpidr;
    out->last_ok_ms = st->last_ok_ms;
    out->last_timeout_ms = st->last_timeout_ms;
    out->consecutive_timeouts = st->consecutive_timeouts;
    out->degraded = st->degraded;
    for (size_t i = 0; i < 4; i++) out->pin_config[i] = (uint8_t)st->pin_config[i];
    return true;
}

extern "C" bool bugbuster_mp_hat_caps(bugbuster_mp_hat_caps_t *out)
{
    if (!out) return false;
    HatCaps caps = {};
    if (!hat_get_caps(&caps)) return false;
    out->hw_revision = caps.hw_revision;
    out->flags = caps.flags;
    out->rail_count = caps.rail_count;
    out->led_count = caps.led_count;
    out->shifted_io_count = caps.shifted_io_count;
    out->la_routes = caps.la_routes;
    out->fw_major = caps.fw_major;
    out->fw_minor = caps.fw_minor;
    return true;
}

extern "C" bool bugbuster_mp_hat_rails(bugbuster_mp_hat_rail_t *out, size_t max, size_t *count)
{
    if (!out || !count || max < HAT_RAIL_COUNT) return false;
    HatRailStatus rails[HAT_RAIL_COUNT] = {};
    uint8_t n = 0;
    if (!hat_get_rail_status(rails, &n)) return false;
    if (n > max) n = (uint8_t)max;
    for (uint8_t i = 0; i < n; i++) {
        out[i].rail_id = rails[i].rail_id;
        out[i].enabled = rails[i].enabled;
        out[i].voltage_mv = rails[i].voltage_mv;
        out[i].current_ma = rails[i].current_ma;
        out[i].status = rails[i].status;
    }
    *count = n;
    return true;
}

extern "C" bool bugbuster_mp_hat_set_rail_enable(uint8_t rail_id, bool enable)
{
    return hat_set_rail_enable(rail_id, enable);
}

extern "C" bool bugbuster_mp_hat_set_rail_voltage(uint8_t rail_id, uint16_t mv)
{
    return hat_set_rail_voltage(rail_id, mv);
}

extern "C" bool bugbuster_mp_hat_led(uint8_t led_id, uint8_t color_code)
{
    return hat_set_led_state(led_id, color_code);
}

extern "C" bool bugbuster_mp_hat_io_bank(uint8_t dirs, uint8_t ups, uint8_t dns, uint8_t vals)
{
    return hat_set_io_bank(dirs, ups, dns, vals);
}

extern "C" bool bugbuster_mp_hat_level_shift(bool oe, bool dir, bool *oe_out, bool *dir_out)
{
    return hat_set_level_shift(oe, dir, oe_out, dir_out);
}

extern "C" bool bugbuster_mp_hat_calibrate_start(uint8_t rail_id, uint8_t *status_out)
{
    return hat_calibrate_start(rail_id, status_out);
}

extern "C" bool bugbuster_mp_hat_calibrate_status(bugbuster_mp_hat_cal_status_t *out)
{
    if (!out) return false;
    return hat_calibrate_status(&out->state, &out->progress, &out->rail_id,
                                &out->last_error, &out->persist_state,
                                &out->stage, &out->point, &out->code,
                                &out->measured_mv, &out->min_mv,
                                &out->max_mv, &out->max_gap_mv,
                                &out->max_error_mv, &out->validation_flags);
}

extern "C" bool bugbuster_mp_hat_calibrate_import(uint8_t rail_id, uint8_t count, const uint8_t *points_data, size_t data_len)
{
    return hat_calibrate_import(rail_id, count, points_data, data_len);
}

extern "C" bool bugbuster_mp_hat_setup_swd(uint16_t target_voltage_mv, uint8_t connector)
{
    if (connector > HAT_CONNECTOR_B) return false;
    return hat_setup_swd(target_voltage_mv, (HatConnector)connector);
}
