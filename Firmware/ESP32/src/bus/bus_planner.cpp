// =============================================================================
// bus_planner.cpp — On-device IO-terminal bus routing engine (Phase 4).
//
// SYNC: keep aligned with python/bugbuster/hal.py DEFAULT_ROUTING +
//       bus.py _plan/_apply_route.  When the PCB routing table or host-side
//       planner logic changes, update this file to match.
//
// Mirrors:
//   hal.py:183–343  — switch masks + DEFAULT_ROUTING
//   bus.py:121–134  — _group_clear_mask / _mux_state_for
//   bus.py:512–526  — _apply_route
// =============================================================================

#include "bus_planner.h"

#include <string.h>
#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "config.h"       // pin_write, PIN_LSHIFT_OE
#include "pca9535.h"      // pca9535_set_control, PcaControl
#include "ds4424.h"       // ds4424_set_voltage
#include "adgs2414d.h"    // adgs_set_api_all_safe, ADGS_API_MAIN_DEVICES
#include "ext_bus.h"      // ext_i2c_setup, ext_spi_setup

// ---------------------------------------------------------------------------
// Planner mutex — serialises plan+apply so a host BBP setup cannot interleave
// with a script setup.  Lazily initialised on first use.
// ---------------------------------------------------------------------------
static SemaphoreHandle_t s_planner_mutex = NULL;

static SemaphoreHandle_t planner_mutex(void)
{
    if (s_planner_mutex == NULL) {
        s_planner_mutex = xSemaphoreCreateMutex();
    }
    return s_planner_mutex;
}

// ---------------------------------------------------------------------------
// MUX switch bit constants (mirrors hal.py:183–190)
// ---------------------------------------------------------------------------
// Group A (position 1, analog capable — 4 options)
#define SW_A_ESP_HIGH  0x01u   // S1 — Group A ESP high drive
#define SW_A_ESP_LOW   0x04u   // S3 — Group A ESP low drive
// Group B (position 2 — digital only)
#define SW_B_ESP_HIGH  0x10u   // S5 — Group B ESP high drive
// Group C (position 3 — digital only)
#define SW_C_ESP_HIGH  0x40u   // S7 — Group C ESP high drive

// Group clear masks (mirrors bus.py:121–126)
#define GROUP_CLEAR_1  0x0Fu   // Position 1: bits 0-3
#define GROUP_CLEAR_2  0x30u   // Position 2: bits 4-5
#define GROUP_CLEAR_3  0xC0u   // Position 3: bits 6-7

// IDAC channel assignments (from ds4424.h comments)
//   ch0 = VLOGIC (level-shifter voltage)
//   ch1 = VADJ1  (supplies IO 1–6)
//   ch2 = VADJ2  (supplies IO 7–12)
#define IDAC_CH_VLOGIC  0u
#define IDAC_CH_VADJ1   1u
#define IDAC_CH_VADJ2   2u

// No-pin sentinel used by ext_bus (matches ext_bus.cpp convention)
#define EXT_BUS_NO_PIN  0xFFu

// ---------------------------------------------------------------------------
// Routing table entry
// ---------------------------------------------------------------------------
typedef struct {
    uint8_t   io_num;        // IO terminal number 1..12
    uint8_t   position;      // MUX group position: 1=Group A, 2=Group B, 3=Group C
    uint8_t   mux_device;    // ADGS2414D device index 0..3
    uint8_t   esp_gpio;      // ESP32 GPIO driven through the MUX
    uint8_t   efuse_ctrl;    // PcaControl enum value for this IO's e-fuse
    uint8_t   supply_ctrl;   // PcaControl enum value for this IO's VADJ supply
    uint8_t   supply_idac;   // DS4424 channel for the supply (1 or 2)
    uint8_t   digital_high_mask; // Switch mask for digital high-drive mode
    uint8_t   group_clear;   // Mask to clear this position's switches
} BusRouteEntry;

// ---------------------------------------------------------------------------
// DEFAULT_ROUTING table (mirrors hal.py:285–343, PCB-swap notes preserved)
//
// SYNC: keep aligned with python/bugbuster/hal.py DEFAULT_ROUTING
// ---------------------------------------------------------------------------
static const BusRouteEntry IO_ROUTES[12] = {
    // ── BLOCK 1, IO_BLOCK 1 — device 0 (U10), VADJ1, EFUSE1 ────────────
    // IO 1: position 3 (Group C), ESP GPIO 4
    { 1,  3, 0,  4, PCA_CTRL_EFUSE1_EN, PCA_CTRL_VADJ1_EN, IDAC_CH_VADJ1,
      SW_C_ESP_HIGH, GROUP_CLEAR_3 },
    // IO 2: position 2 (Group B), ESP GPIO 2
    { 2,  2, 0,  2, PCA_CTRL_EFUSE1_EN, PCA_CTRL_VADJ1_EN, IDAC_CH_VADJ1,
      SW_B_ESP_HIGH, GROUP_CLEAR_2 },
    // IO 3: position 1 (Group A analog), ESP GPIO 1
    { 3,  1, 0,  1, PCA_CTRL_EFUSE1_EN, PCA_CTRL_VADJ1_EN, IDAC_CH_VADJ1,
      SW_A_ESP_HIGH, GROUP_CLEAR_1 },

    // ── BLOCK 1, IO_BLOCK 2 — device 1 (U11), VADJ1, EFUSE2 ────────────
    // IO 4: position 3 (Group C), ESP GPIO 7
    { 4,  3, 1,  7, PCA_CTRL_EFUSE2_EN, PCA_CTRL_VADJ1_EN, IDAC_CH_VADJ1,
      SW_C_ESP_HIGH, GROUP_CLEAR_3 },
    // IO 5: position 2 (Group B), ESP GPIO 6
    { 5,  2, 1,  6, PCA_CTRL_EFUSE2_EN, PCA_CTRL_VADJ1_EN, IDAC_CH_VADJ1,
      SW_B_ESP_HIGH, GROUP_CLEAR_2 },
    // IO 6: position 1 (Group A analog), ESP GPIO 5
    { 6,  1, 1,  5, PCA_CTRL_EFUSE2_EN, PCA_CTRL_VADJ1_EN, IDAC_CH_VADJ1,
      SW_A_ESP_HIGH, GROUP_CLEAR_1 },

    // ── BLOCK 2, IO_BLOCK 3 — device 3 (U17), VADJ2, EFUSE4 ────────────
    // PCB swap: physical connector 3 is wired to EFUSE4 (silkscreen EFUSE3↔EFUSE4 crossed)
    // IO 7: position 3 (Group C), ESP GPIO 8
    { 7,  3, 3,  8, PCA_CTRL_EFUSE4_EN, PCA_CTRL_VADJ2_EN, IDAC_CH_VADJ2,
      SW_C_ESP_HIGH, GROUP_CLEAR_3 },
    // IO 8: position 2 (Group B), ESP GPIO 9
    { 8,  2, 3,  9, PCA_CTRL_EFUSE4_EN, PCA_CTRL_VADJ2_EN, IDAC_CH_VADJ2,
      SW_B_ESP_HIGH, GROUP_CLEAR_2 },
    // IO 9: position 1 (Group A analog), ESP GPIO 10
    { 9,  1, 3, 10, PCA_CTRL_EFUSE4_EN, PCA_CTRL_VADJ2_EN, IDAC_CH_VADJ2,
      SW_A_ESP_HIGH, GROUP_CLEAR_1 },

    // ── BLOCK 2, IO_BLOCK 4 — device 2 (U16), VADJ2, EFUSE3 ────────────
    // PCB swap: physical connector 4 is wired to EFUSE3
    // IO 10: position 3 (Group C), ESP GPIO 11
    { 10, 3, 2, 11, PCA_CTRL_EFUSE3_EN, PCA_CTRL_VADJ2_EN, IDAC_CH_VADJ2,
      SW_C_ESP_HIGH, GROUP_CLEAR_3 },
    // IO 11: position 2 (Group B), ESP GPIO 12
    { 11, 2, 2, 12, PCA_CTRL_EFUSE3_EN, PCA_CTRL_VADJ2_EN, IDAC_CH_VADJ2,
      SW_B_ESP_HIGH, GROUP_CLEAR_2 },
    // IO 12: position 1 (Group A analog), ESP GPIO 13
    { 12, 1, 2, 13, PCA_CTRL_EFUSE3_EN, PCA_CTRL_VADJ2_EN, IDAC_CH_VADJ2,
      SW_A_ESP_HIGH, GROUP_CLEAR_1 },
};

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static const BusRouteEntry *find_route(uint8_t io_num)
{
    if (io_num < 1 || io_num > 12) return NULL;
    return &IO_ROUTES[io_num - 1];
}

// Compute 4-byte MUX state for a set of IO entries.
// Mirrors bus.py:_mux_state_for (lines 129–134).
static void compute_mux_state(const BusRouteEntry **routes, size_t count,
                               uint8_t mux_out[ADGS_API_MAIN_DEVICES])
{
    memset(mux_out, 0, ADGS_API_MAIN_DEVICES);
    for (size_t i = 0; i < count; i++) {
        const BusRouteEntry *r = routes[i];
        uint8_t dev = r->mux_device;
        mux_out[dev] = (uint8_t)((mux_out[dev] & ~r->group_clear) | r->digital_high_mask);
    }
}

static void set_err(char *err, size_t err_len, const char *msg)
{
    if (err && err_len > 0) {
        strncpy(err, msg, err_len - 1);
        err[err_len - 1] = '\0';
    }
}

// Apply power routing: mirrors bus.py:_apply_route (lines 512–525).
// Order: MUX power → level-shifter OE → VLOGIC → per-route VADJ+efuse → MUX.
static bool apply_power_and_mux(const BusRouteEntry **routes, size_t count,
                                 float supply_v, float vlogic_v,
                                 char *err, size_t err_len)
{
    // 1. MUX power
    if (!pca9535_set_control(PCA_CTRL_MUX_EN, true)) {
        set_err(err, err_len, "pca9535: MUX enable failed");
        return false;
    }

    // 2. Level-shifter OE
    pin_write(PIN_LSHIFT_OE, 1);

    // 3. VLOGIC (DS4424 ch0)
    if (!ds4424_set_voltage(IDAC_CH_VLOGIC, vlogic_v)) {
        set_err(err, err_len, "ds4424: VLOGIC set failed");
        return false;
    }

    // 4. Per-route: supply VADJ + e-fuse
    //    Walk all routes; skip duplicate supply/efuse enables (safe to enable twice).
    for (size_t i = 0; i < count; i++) {
        const BusRouteEntry *r = routes[i];

        // Enable the supply rail
        if (!pca9535_set_control((PcaControl)r->supply_ctrl, true)) {
            set_err(err, err_len, "pca9535: VADJ enable failed");
            return false;
        }
        // Set supply voltage via IDAC
        if (!ds4424_set_voltage(r->supply_idac, supply_v)) {
            set_err(err, err_len, "ds4424: VADJ voltage set failed");
            return false;
        }
        // Enable e-fuse
        if (!pca9535_set_control((PcaControl)r->efuse_ctrl, true)) {
            set_err(err, err_len, "pca9535: efuse enable failed");
            return false;
        }
    }

    // 5. Set MUX state
    uint8_t mux_states[ADGS_API_MAIN_DEVICES];
    compute_mux_state(routes, count, mux_states);
    adgs_set_api_all_safe(mux_states);

    return true;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

extern "C" bool bus_planner_apply_i2c(uint8_t sda_io, uint8_t scl_io,
                                       uint32_t freq_hz, bool internal_pullups,
                                       float supply_v, float vlogic_v,
                                       char *err, size_t err_len)
{
    // ── Validate ────────────────────────────────────────────────────────────
    if (supply_v > 5.0f) {
        set_err(err, err_len, "supply > 5.0 V not allowed");
        return false;
    }
    if (sda_io < 1 || sda_io > 12) {
        set_err(err, err_len, "sda_io out of range 1..12");
        return false;
    }
    if (scl_io < 1 || scl_io > 12) {
        set_err(err, err_len, "scl_io out of range 1..12");
        return false;
    }
    if (sda_io == scl_io) {
        set_err(err, err_len, "sda_io and scl_io must be different");
        return false;
    }

    const BusRouteEntry *sda_r = find_route(sda_io);
    const BusRouteEntry *scl_r = find_route(scl_io);
    if (!sda_r || !scl_r) {
        set_err(err, err_len, "IO route not found");
        return false;
    }

    // Check split-supply: both pins must use the same supply rail (v1: no split)
    if (sda_r->supply_ctrl != scl_r->supply_ctrl) {
        set_err(err, err_len, "I2C pins span multiple supplies; split supply not supported in v1");
        return false;
    }

    // ── Lock ────────────────────────────────────────────────────────────────
    SemaphoreHandle_t mtx = planner_mutex();
    if (mtx == NULL || xSemaphoreTake(mtx, pdMS_TO_TICKS(2000)) != pdTRUE) {
        set_err(err, err_len, "planner mutex timeout");
        return false;
    }

    bool ok = false;

    const BusRouteEntry *routes[2] = { sda_r, scl_r };
    if (!apply_power_and_mux(routes, 2, supply_v, vlogic_v, err, err_len)) {
        goto done;
    }

    // ── Configure ext_bus I2C ───────────────────────────────────────────────
    if (!ext_i2c_setup(sda_r->esp_gpio, scl_r->esp_gpio, freq_hz, internal_pullups)) {
        set_err(err, err_len, "ext_i2c_setup failed");
        goto done;
    }

    ok = true;

done:
    xSemaphoreGive(mtx);
    return ok;
}

extern "C" bool bus_planner_apply_spi(uint8_t sck_io,
                                       uint8_t mosi_io_or_0, uint8_t miso_io_or_0,
                                       uint8_t cs_io_or_0,
                                       uint32_t freq_hz, uint8_t mode,
                                       float supply_v, float vlogic_v,
                                       char *err, size_t err_len)
{
    // ── Validate ────────────────────────────────────────────────────────────
    if (supply_v > 5.0f) {
        set_err(err, err_len, "supply > 5.0 V not allowed");
        return false;
    }
    if (sck_io < 1 || sck_io > 12) {
        set_err(err, err_len, "sck_io out of range 1..12");
        return false;
    }

    // Validate optional pins
    uint8_t opt_ios[3] = { mosi_io_or_0, miso_io_or_0, cs_io_or_0 };
    for (int i = 0; i < 3; i++) {
        if (opt_ios[i] != 0 && (opt_ios[i] < 1 || opt_ios[i] > 12)) {
            set_err(err, err_len, "optional IO out of range 1..12");
            return false;
        }
    }

    // Check for duplicates among all provided (non-zero) pins
    uint8_t all_ios[4] = { sck_io, mosi_io_or_0, miso_io_or_0, cs_io_or_0 };
    for (int i = 0; i < 4; i++) {
        if (all_ios[i] == 0) continue;
        for (int j = i + 1; j < 4; j++) {
            if (all_ios[j] == 0) continue;
            if (all_ios[i] == all_ios[j]) {
                set_err(err, err_len, "duplicate IO terminal in SPI setup");
                return false;
            }
        }
    }

    const BusRouteEntry *sck_r  = find_route(sck_io);
    if (!sck_r) {
        set_err(err, err_len, "SCK IO route not found");
        return false;
    }

    // Collect all active routes and their ESP GPIOs
    const BusRouteEntry *routes[4];
    size_t n_routes = 0;
    routes[n_routes++] = sck_r;

    const BusRouteEntry *mosi_r = NULL;
    const BusRouteEntry *miso_r = NULL;
    const BusRouteEntry *cs_r   = NULL;

    if (mosi_io_or_0 != 0) {
        mosi_r = find_route(mosi_io_or_0);
        if (!mosi_r) { set_err(err, err_len, "MOSI IO route not found"); return false; }
        routes[n_routes++] = mosi_r;
    }
    if (miso_io_or_0 != 0) {
        miso_r = find_route(miso_io_or_0);
        if (!miso_r) { set_err(err, err_len, "MISO IO route not found"); return false; }
        routes[n_routes++] = miso_r;
    }
    if (cs_io_or_0 != 0) {
        cs_r = find_route(cs_io_or_0);
        if (!cs_r) { set_err(err, err_len, "CS IO route not found"); return false; }
        routes[n_routes++] = cs_r;
    }

    // Check split-supply: all active pins must use the same supply rail (v1)
    for (size_t i = 1; i < n_routes; i++) {
        if (routes[i]->supply_ctrl != routes[0]->supply_ctrl) {
            set_err(err, err_len, "SPI pins span multiple supplies; split supply not supported in v1");
            return false;
        }
    }

    // Translate IO terminals → ESP GPIOs; 0 IO → 0xFF sentinel for ext_bus
    uint8_t esp_mosi = (mosi_r != NULL) ? mosi_r->esp_gpio : (uint8_t)EXT_BUS_NO_PIN;
    uint8_t esp_miso = (miso_r != NULL) ? miso_r->esp_gpio : (uint8_t)EXT_BUS_NO_PIN;
    uint8_t esp_cs   = (cs_r   != NULL) ? cs_r->esp_gpio   : (uint8_t)EXT_BUS_NO_PIN;

    // ── Lock ────────────────────────────────────────────────────────────────
    SemaphoreHandle_t mtx = planner_mutex();
    if (mtx == NULL || xSemaphoreTake(mtx, pdMS_TO_TICKS(2000)) != pdTRUE) {
        set_err(err, err_len, "planner mutex timeout");
        return false;
    }

    bool ok = false;

    if (!apply_power_and_mux(routes, n_routes, supply_v, vlogic_v, err, err_len)) {
        goto done;
    }

    // ── Configure ext_bus SPI ───────────────────────────────────────────────
    if (!ext_spi_setup(sck_r->esp_gpio, esp_mosi, esp_miso, esp_cs, freq_hz, mode)) {
        set_err(err, err_len, "ext_spi_setup failed");
        goto done;
    }

    ok = true;

done:
    xSemaphoreGive(mtx);
    return ok;
}

// ---------------------------------------------------------------------------
// bus_planner_route_digital_input — single IO as digital input (no ext_bus)
// Used by autorun.cpp to read the IO12 hold-to-disable gate.
// ---------------------------------------------------------------------------
extern "C" bool bus_planner_route_digital_input(uint8_t io_num,
                                                 char *err, size_t err_len)
{
    if (io_num < 1 || io_num > 12) {
        set_err(err, err_len, "io_num out of range 1..12");
        return false;
    }

    const BusRouteEntry *r = find_route(io_num);
    if (!r) {
        set_err(err, err_len, "IO route not found");
        return false;
    }

    SemaphoreHandle_t mtx = planner_mutex();
    if (mtx == NULL || xSemaphoreTake(mtx, pdMS_TO_TICKS(2000)) != pdTRUE) {
        set_err(err, err_len, "planner mutex timeout");
        return false;
    }

    // Use 3.3 V for both supply and VLOGIC — safe read-only default
    const BusRouteEntry *routes[1] = { r };
    bool ok = apply_power_and_mux(routes, 1, 3.3f, 3.3f, err, err_len);

    xSemaphoreGive(mtx);
    return ok;
}
