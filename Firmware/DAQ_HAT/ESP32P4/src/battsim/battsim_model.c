// =============================================================================
// battsim_model.c - cell model tables and pure model functions.
//
// OCV tables are RESTING voltages per cell, 0..100 % SOC in 5 % steps.
// Sources (typical published rest-voltage curves at ~25 C; real cells vary by
// a few tens of mV, which is below the supply's ~72 mV step anyway):
//   LiPo/Li-ion : common LiCoO2/NMC rest-OCV chart (4.20 V full, 3.27 V empty)
//   LiFePO4     : 12.8 V LFP pack rest chart / 4 (3.40 V rested full, 2.50 V empty)
//   NiMH        : standard NiMH rest curve (1.40 V full, flat ~1.2-1.25 V plateau)
//   Lead-acid   : VRLA/AGM rest chart / 6 (12.80 V full, 11.80 V empty)
// Self-discharge defaults (per month, 20-25 C): Li-ion ~2 %, LiFePO4 ~2.5 %,
// NiMH ~20 % (low-self-discharge types ~1.5 %, set by override), AGM ~3 %.
// Peukert: lead-acid ~1.2 at the 20 h rate, NiMH ~1.1, Li chemistries ~1.03-1.05.
// =============================================================================

#include "battsim_model.h"
#include <math.h>
#include <stddef.h>

static const bs_chem_info_t s_chem[BS_CHEM_COUNT] = {
    [BS_CHEM_LIPO] = {
        .name = "LiPo",
        .ocv_mv = { 3270, 3610, 3690, 3710, 3730, 3750, 3770, 3790, 3800, 3820,
                    3840, 3850, 3870, 3910, 3950, 3980, 4020, 4080, 4110, 4150, 4200 },
        .cutoff_mv = 3000, .rint_mohm_ah = 60, .peukert_x1000 = 1050,
        .peukert_rated_h = 5, .sd_pct_month_x100 = 200, .cells_min = 1, .cells_max = 4,
    },
    [BS_CHEM_LIFEPO4] = {
        .name = "LiFePO4",
        .ocv_mv = { 2500, 2800, 3000, 3100, 3200, 3213, 3225, 3238, 3250, 3255,
                    3260, 3270, 3275, 3288, 3300, 3313, 3325, 3338, 3350, 3375, 3400 },
        .cutoff_mv = 2500, .rint_mohm_ah = 40, .peukert_x1000 = 1030,
        .peukert_rated_h = 5, .sd_pct_month_x100 = 250, .cells_min = 1, .cells_max = 5,
    },
    [BS_CHEM_NIMH] = {
        .name = "NiMH",
        .ocv_mv = { 1000, 1100, 1150, 1180, 1200, 1210, 1220, 1225, 1230, 1235,
                    1240, 1245, 1250, 1255, 1260, 1270, 1280, 1290, 1310, 1340, 1400 },
        .cutoff_mv = 1000, .rint_mohm_ah = 100, .peukert_x1000 = 1100,
        .peukert_rated_h = 5, .sd_pct_month_x100 = 2000, .cells_min = 2, .cells_max = 14,
    },
    [BS_CHEM_LEAD] = {
        .name = "Lead-acid",
        .ocv_mv = { 1967, 1975, 1983, 1992, 2000, 2008, 2017, 2025, 2033, 2042,
                    2050, 2058, 2067, 2075, 2083, 2092, 2100, 2108, 2117, 2125, 2133 },
        .cutoff_mv = 1750, .rint_mohm_ah = 30, .peukert_x1000 = 1200,
        .peukert_rated_h = 20, .sd_pct_month_x100 = 300, .cells_min = 2, .cells_max = 9,
    },
};

const bs_chem_info_t *bs_chem_info(bs_chem_t chem)
{
    return ((unsigned)chem < BS_CHEM_COUNT) ? &s_chem[chem] : NULL;
}

uint32_t bs_default_rint_uohm(bs_chem_t chem, uint32_t capacity_mah)
{
    const bs_chem_info_t *ci = bs_chem_info(chem);
    if (!ci || capacity_mah == 0) return 0;
    // mOhm*Ah / Ah -> mOhm; x1000 -> uOhm; Ah = mAh/1000 -> x1e6 / mAh.
    uint64_t u = (uint64_t)ci->rint_mohm_ah * 1000000ULL / capacity_mah;
    return (u > UINT32_MAX) ? UINT32_MAX : (uint32_t)u;
}

void bs_params_defaults(bs_params_t *p, bs_chem_t chem, uint8_t cells,
                        uint32_t capacity_mah)
{
    const bs_chem_info_t *ci = bs_chem_info(chem);
    if (!ci) { chem = BS_CHEM_LIPO; ci = &s_chem[0]; }
    *p = (bs_params_t){0};
    p->chem              = (uint8_t)chem;
    p->cells             = cells;
    p->capacity_mah      = capacity_mah;
    p->start_soc_x10     = 1000;
    p->cutoff_mv_cell    = ci->cutoff_mv;
    p->rint_uohm_cell    = bs_default_rint_uohm(chem, capacity_mah);
    p->peukert_x1000     = ci->peukert_x1000;
    p->sd_enable         = 1;
    p->sd_pct_month_x100 = ci->sd_pct_month_x100;
}

bs_valid_t bs_params_validate(const bs_params_t *p, uint32_t vmin_mv,
                              uint32_t vmax_mv)
{
    const bs_chem_info_t *ci = bs_chem_info((bs_chem_t)p->chem);
    if (!ci) return BS_ERR_CHEM;
    if (p->cells < ci->cells_min || p->cells > ci->cells_max) return BS_ERR_CELLS;
    // 2,000 Ah = 7.2e18 pC, inside int64 (9.2e18); the run stops at depletion.
    if (p->capacity_mah == 0 || p->capacity_mah > 2000000u) return BS_ERR_CAPACITY;
    if (p->start_soc_x10 > 1000) return BS_ERR_SOC;
    if ((uint32_t)p->cells * ci->ocv_mv[BS_OCV_POINTS - 1] > vmax_mv) return BS_ERR_VMAX;
    if ((uint32_t)p->cells * p->cutoff_mv_cell < vmin_mv) return BS_ERR_VMIN;
    return BS_OK;
}

float bs_ocv_cell_mv(bs_chem_t chem, uint32_t soc_ppm)
{
    const bs_chem_info_t *ci = bs_chem_info(chem);
    if (!ci) return 0.0f;
    if (soc_ppm >= 1000000u) return (float)ci->ocv_mv[BS_OCV_POINTS - 1];
    // 50,000 ppm (5 %) per table step.
    uint32_t idx  = soc_ppm / 50000u;
    uint32_t frac = soc_ppm % 50000u;
    float a = (float)ci->ocv_mv[idx];
    float b = (float)ci->ocv_mv[idx + 1];
    return a + (b - a) * ((float)frac / 50000.0f);
}

float bs_pack_voltage(const bs_params_t *p, uint32_t soc_ppm, float i_amps)
{
    float ocv = bs_ocv_cell_mv((bs_chem_t)p->chem, soc_ppm) * 1e-3f * (float)p->cells;
    if (i_amps < 0.0f) i_amps = 0.0f;   // source-only: no charge-side rise
    float r_pack = (float)p->rint_uohm_cell * 1e-6f * (float)p->cells;
    return ocv - i_amps * r_pack;
}

int64_t bs_capacity_pc(const bs_params_t *p)
{
    return (int64_t)p->capacity_mah * BS_PC_PER_MAH;
}

float bs_peukert_factor(const bs_params_t *p, float i_amps)
{
    const bs_chem_info_t *ci = bs_chem_info((bs_chem_t)p->chem);
    if (!ci || p->peukert_x1000 <= 1000 || i_amps <= 0.0f) return 1.0f;
    float i_rated = ((float)p->capacity_mah * 1e-3f) / (float)ci->peukert_rated_h;
    if (i_rated <= 0.0f || i_amps <= i_rated) return 1.0f;
    float k = (float)p->peukert_x1000 * 1e-3f;
    return powf(i_amps / i_rated, k - 1.0f);
}

double bs_self_discharge_pc(const bs_params_t *p, int64_t q_rem_pc, double dt_s)
{
    if (!p->sd_enable || q_rem_pc <= 0 || dt_s <= 0.0) return 0.0;
    // Exponential decay with a monthly loss fraction f: rate = -ln(1-f)/month.
    double f = (double)p->sd_pct_month_x100 * 1e-4;
    if (f <= 0.0) return 0.0;
    if (f >= 0.99) f = 0.99;
    double lambda = -log1p(-f) / BS_SECONDS_PER_MONTH;
    // expm1: 1 - exp(-x) cancels catastrophically at the ~1e-9 per-second x.
    return (double)q_rem_pc * -expm1(-lambda * dt_s);
}

bs_reopen_t bs_reopen_check(uint8_t state, bool loaded, bool writable)
{
    if (!loaded) return BS_REOPEN_NO_RUN;
    if (state == 3) return BS_REOPEN_DEPLETED;   // BS_ST_DEPLETED
    if (state != 4 || !writable) return BS_REOPEN_STATE;   // BS_ST_STOPPED only
    return BS_REOPEN_OK;
}
