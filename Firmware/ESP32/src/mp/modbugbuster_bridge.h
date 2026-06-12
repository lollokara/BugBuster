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

// ── Power/PD helpers ──────────────────────────────────────────────────────────
bool bugbuster_mp_vadj_pd_warning(uint8_t rail, float requested_v,
                                  char *warning, size_t warning_len);

// ── I2C binding (Phase 4) ────────────────────────────────────────────────────
bool bugbuster_mp_i2c_setup(uint8_t sda_io, uint8_t scl_io, uint32_t freq_hz,
                             bool internal_pullups, float supply_v, float vlogic_v,
                             bool allow_split_supplies,
                             char *err, size_t err_len);
bool bugbuster_mp_i2c_close(void);
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
                             bool allow_split_supplies,
                             char *err, size_t err_len);
bool bugbuster_mp_spi_transfer(const uint8_t *tx, size_t tx_len,
                                uint8_t *rx, size_t *inout_rx_len,
                                uint16_t timeout_ms);
bool bugbuster_mp_spi_close(void);

// ── Network binding (V2-E) — implemented in net_bridge.cpp ──────────────────
// (declarations live in net_bridge.h; this comment marks the boundary so
//  future phases know where to add analogous bridge groups.)

// ── IO-ownership binding (PR-5) — implemented in modbugbuster_bridge.cpp ────
// claim_result: 0=OK, 1=held-by-other, 2=not-owned, 3=invalid-slot, <0=error
int  bugbuster_mp_io_acquire(const uint8_t *slots, size_t n_slots,
                              uint32_t lease_ms, uint32_t purpose_tag);
int  bugbuster_mp_io_release(const uint8_t *slots, size_t n_slots);

// Fills out[i] for i in 0..15.  Each row is:
//   { kind(u8), session_id(u8), token_fp32(u32), lease_until_lo32(u32), purpose_tag(u32) }
// Returns false only if the ownership table is unavailable.
bool bugbuster_mp_io_status(uint8_t *kind_out, uint8_t *session_out,
                             uint32_t *token_out, uint32_t *lease_lo_out,
                             uint32_t *purpose_out, size_t n);

// ── HAT v2 binding — implemented in modbugbuster_bridge.cpp ─────────────────
typedef struct {
    bool detected;
    bool connected;
    uint8_t type;
    float detect_voltage;
    uint8_t fw_major;
    uint8_t fw_minor;
    bool config_confirmed;
    uint16_t io_voltage_mv;
    uint8_t hvpak_part;
    bool hvpak_ready;
    uint8_t hvpak_last_error;
    bool caps_valid;
    uint8_t la_route;
    bool dap_connected;
    bool target_detected;
    uint32_t target_dpidr;
    uint32_t last_ok_ms;
    uint32_t last_timeout_ms;
    uint8_t consecutive_timeouts;
    bool degraded;
    uint8_t pin_config[4];
} bugbuster_mp_hat_status_t;

typedef struct {
    uint8_t hw_revision;
    uint32_t flags;
    uint8_t rail_count;
    uint8_t led_count;
    uint8_t shifted_io_count;
    uint8_t la_routes;
    uint8_t fw_major;
    uint8_t fw_minor;
    bool hvpak_present;
} bugbuster_mp_hat_caps_t;

typedef struct {
    uint8_t rail_id;
    bool enabled;
    uint16_t voltage_mv;
    uint16_t current_ma;
    uint8_t status;
} bugbuster_mp_hat_rail_t;

typedef struct {
    uint8_t state;
    uint8_t progress;
    uint8_t rail_id;
    uint8_t last_error;
    uint8_t persist_state;
    uint8_t stage;
    uint8_t point;
    int8_t code;
    int32_t measured_mv;
    int32_t min_mv;
    int32_t max_mv;
    int32_t max_gap_mv;
    int32_t max_error_mv;
    uint16_t validation_flags;
} bugbuster_mp_hat_cal_status_t;

bool bugbuster_mp_hat_status(bugbuster_mp_hat_status_t *out);
bool bugbuster_mp_hat_caps(bugbuster_mp_hat_caps_t *out);
bool bugbuster_mp_hat_rails(bugbuster_mp_hat_rail_t *out, size_t max, size_t *count);
bool bugbuster_mp_hat_set_rail_enable(uint8_t rail_id, bool enable);
bool bugbuster_mp_hat_set_rail_voltage(uint8_t rail_id, uint16_t mv);
bool bugbuster_mp_hat_led(uint8_t led_id, uint8_t color_code);
bool bugbuster_mp_hat_io_bank(uint8_t dirs, uint8_t ups, uint8_t dns, uint8_t vals);
bool bugbuster_mp_hat_level_shift(bool oe, bool dir, bool *oe_out, bool *dir_out);
bool bugbuster_mp_hat_calibrate_start(uint8_t rail_id, uint8_t *status_out);
bool bugbuster_mp_hat_calibrate_status(bugbuster_mp_hat_cal_status_t *out);
bool bugbuster_mp_hat_calibrate_import(uint8_t rail_id, uint8_t count, const uint8_t *points_data, size_t data_len);
bool bugbuster_mp_hat_setup_swd(uint16_t target_voltage_mv, uint8_t connector);

// ── Auto-wrap helpers (modbugbuster_owner.c) — for Channel bindings ─────────
// Returns true if the caller is already inside a bugbuster.claim() context.
bool bugbuster_mp_in_claim_context(void);
// One-shot 5 s auto-claim for backward-compat. Returns io_claim_result_t int.
int  bugbuster_mp_auto_claim(uint8_t channel_slot, uint32_t purpose_tag);

#ifdef __cplusplus
}
#endif
