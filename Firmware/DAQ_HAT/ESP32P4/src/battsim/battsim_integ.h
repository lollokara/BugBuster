#pragma once

// =============================================================================
// battsim_integ.h - exact DUT charge integrator for the battery simulator.
//
// Fed from daq_fast_task's DSP tail (fast_emit) with the fused current and the
// number of raw ADC periods it covers. Accumulates in integer units only:
//
//   acc += round(I * 1e9) [nA] * raw_periods * period_mclk
//
// One nA for one MCLK tick (1 / 16.384 MHz) is exactly 1/16384 pC, so the
// accumulator folds into int64 pC with an exact integer division and carried
// remainder. Nothing is ever rounded twice, so the summation itself adds no
// drift no matter how long the run lasts. Independent of power_dsp, whose
// energy/charge window is reset by rate changes and host commands.
//
// Energy uses the same tick base with the block-mean power in nW:
//   eacc += round(P * 1e9) [nW] * raw_periods * period_mclk
// and 1 nW for 1 MCLK tick is exactly 1/16,384,000 nJ, folded the same way.
// =============================================================================

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define BS_MCLK_HZ          16384000LL   // ADAQ MCLK (config.h ADAQ_MCLK_HZ)
#define BS_NA_TICKS_PER_PC  16384LL      // nA * MCLK tick per pC
#define BS_NW_TICKS_PER_NJ  16384000LL   // nW * MCLK tick per nJ

typedef struct {
    int64_t  q_pc;        // integrated DUT charge (pC)
    int64_t  e_nj;        // integrated DUT energy (nJ)
    int64_t  ticks;       // integrated time (MCLK ticks)
    // Per-interval fields below are reset by every snapshot. Voltage figures
    // cover output-on samples only.
    int64_t  v_uv_ticks;  // sum of V(uV) * ticks over the interval
    int64_t  int_ticks;   // output-on ticks over the interval
    int64_t  off_ticks;   // output-off ticks over the interval
    float    i_min, i_max;   // extremes since the last snapshot (A)
    float    v_min, v_max;   // extremes since the last snapshot (V)
    uint32_t pushes;      // DSP-tail pushes since enable (stall detection)
} bs_integ_snap_t;

// Set the raw ADC period in MCLK ticks (call whenever the FINE ODR changes).
void bs_integ_set_odr(float fine_odr_hz);

// Start/stop accumulating. Disabling keeps the totals.
void bs_integ_enable(bool on);
bool bs_integ_enabled(void);

// Load the totals (resume from a checkpoint) - only while disabled.
void bs_integ_restore(int64_t q_pc, int64_t ticks);

// Hot path, daq_fast_task only. watts = block mean of V*I; output_on false
// keeps the sample out of the voltage statistics (charge/energy still count).
void bs_integ_push(float amps, float volts, float watts, uint32_t raw_periods,
                   bool output_on);

// Copy the totals and reset the per-interval min/max.
void bs_integ_snapshot(bs_integ_snap_t *out);

#ifdef __cplusplus
}
#endif
