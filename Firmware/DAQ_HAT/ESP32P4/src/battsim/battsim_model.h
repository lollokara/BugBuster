#pragma once

// =============================================================================
// battsim_model.h - battery simulator cell model (pure C, no ESP-IDF deps).
//
// Charge is carried in int64 pico-coulombs everywhere. The model only turns
// charge into SOC / voltage and supplies per-chemistry defaults; integration
// happens in battsim_integ (DUT current) and battsim.c (virtual loads).
// =============================================================================

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    BS_CHEM_LIPO    = 0,   // LiCoO2 / NMC pouch and cylindrical
    BS_CHEM_LIFEPO4 = 1,
    BS_CHEM_NIMH    = 2,
    BS_CHEM_LEAD    = 3,   // VRLA / AGM, 2 V per cell
    BS_CHEM_COUNT
} bs_chem_t;

#define BS_OCV_POINTS     21          // 0..100 % SOC in 5 % steps
#define BS_PC_PER_MAH     3600000000000LL  // 1 mAh = 3.6 C = 3.6e12 pC
#define BS_SECONDS_PER_MONTH 2592000.0  // 30 days

typedef struct {
    const char *name;
    uint16_t ocv_mv[BS_OCV_POINTS];   // resting OCV per cell, index = SOC/5 %
    uint16_t cutoff_mv;               // per-cell BMS cutoff (loaded voltage)
    uint32_t rint_mohm_ah;            // per-cell R_int x capacity (mOhm*Ah)
    uint16_t peukert_x1000;           // Peukert exponent k x1000
    uint16_t peukert_rated_h;         // hour rate the nameplate capacity is quoted at
    uint16_t sd_pct_month_x100;       // self-discharge %/month x100 at 20-25 C
    uint8_t  cells_min, cells_max;    // limits imposed by the 1.8..19.9 V supply
} bs_chem_info_t;

const bs_chem_info_t *bs_chem_info(bs_chem_t chem);

// User-visible battery definition (also the on-flash profile payload).
typedef struct {
    uint8_t  chem;               // bs_chem_t
    uint8_t  cells;              // series cells
    uint8_t  sd_enable;          // self-discharge on/off
    uint8_t  ext_enable;         // virtual external load on/off
    uint8_t  dither;             // sub-step dithering on/off
    uint8_t  rsv[3];
    uint32_t capacity_mah;       // nameplate capacity
    uint16_t start_soc_x10;      // 0..1000 = 0..100.0 %
    uint16_t cutoff_mv_cell;     // per-cell cutoff (loaded)
    uint32_t rint_uohm_cell;     // per-cell internal resistance
    uint16_t peukert_x1000;      // 1000 = disabled
    uint16_t sd_pct_month_x100;  // self-discharge %/month x100
    uint32_t ext_load_ua;        // virtual constant-current load
} bs_params_t;
_Static_assert(sizeof(bs_params_t) == 28, "bs_params_t is stored on flash");

// Fill @p with the defaults for @chem/@cells/@capacity_mah.
void bs_params_defaults(bs_params_t *p, bs_chem_t chem, uint8_t cells,
                        uint32_t capacity_mah);

// Recompute the capacity-dependent default R_int (uOhm per cell).
uint32_t bs_default_rint_uohm(bs_chem_t chem, uint32_t capacity_mah);

typedef enum {
    BS_OK = 0,
    BS_ERR_CHEM,          // unknown chemistry
    BS_ERR_CELLS,         // outside cells_min..cells_max
    BS_ERR_CAPACITY,      // zero / too large
    BS_ERR_VMAX,          // full pack voltage above the supply maximum
    BS_ERR_VMIN,          // cutoff pack voltage below the supply minimum
    BS_ERR_SOC,           // start SOC > 100 %
} bs_valid_t;

// Validate @p against the chemistry tables and the supply range
// [vmin_mv, vmax_mv]. Returns BS_OK or the first failing check.
bs_valid_t bs_params_validate(const bs_params_t *p, uint32_t vmin_mv,
                              uint32_t vmax_mv);

// Per-cell resting OCV (mV, fractional) at @soc_ppm (0..1e6).
float bs_ocv_cell_mv(bs_chem_t chem, uint32_t soc_ppm);

// Pack terminal voltage target (V): n * OCV(SOC) - I * R_pack.
float bs_pack_voltage(const bs_params_t *p, uint32_t soc_ppm, float i_amps);

// Nameplate capacity in pC.
int64_t bs_capacity_pc(const bs_params_t *p);

// Peukert multiplier for a load of @i_amps (>= 1.0; 1.0 below the rated
// current, so small loads are never credited more than the nameplate).
float bs_peukert_factor(const bs_params_t *p, float i_amps);

// Self-discharge over @dt_s seconds from @q_rem_pc remaining (exponential).
double bs_self_discharge_pc(const bs_params_t *p, int64_t q_rem_pc, double dt_s);

// REOPEN policy: only a loaded, writable STOPPED run may go back to PAUSED.
// @state is a battsim_state_t value (0 NONE 1 PAUSED 2 ACTIVE 3 DEPLETED
// 4 STOPPED). DEPLETED is refused on its own code: that end is physical.
typedef enum { BS_REOPEN_OK = 0, BS_REOPEN_NO_RUN, BS_REOPEN_DEPLETED, BS_REOPEN_STATE } bs_reopen_t;
bs_reopen_t bs_reopen_check(uint8_t state, bool loaded, bool writable);

#ifdef __cplusplus
}
#endif
