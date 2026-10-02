// =============================================================================
// battsim_integ.c - exact DUT charge integrator (see battsim_integ.h).
// =============================================================================

#include "battsim_integ.h"
#include <math.h>
#include "freertos/FreeRTOS.h"

static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;

static volatile bool s_enabled;
static uint32_t s_period_mclk = 2048;   // 8 kSPS default until set
static int64_t  s_acc;                  // nA*ticks not yet folded into pC
static bs_integ_snap_t s_tot;

static void reset_extremes(void)
{
    s_tot.v_uv_ticks = 0;
    s_tot.int_ticks = 0;
    s_tot.i_min = INFINITY;  s_tot.i_max = -INFINITY;
    s_tot.v_min = INFINITY;  s_tot.v_max = -INFINITY;
}

void bs_integ_set_odr(float fine_odr_hz)
{
    if (fine_odr_hz <= 0.0f) return;
    long t = lroundf((float)BS_MCLK_HZ / fine_odr_hz);
    if (t < 1) t = 1;
    portENTER_CRITICAL(&s_mux);
    s_period_mclk = (uint32_t)t;
    portEXIT_CRITICAL(&s_mux);
}

void bs_integ_enable(bool on)
{
    portENTER_CRITICAL(&s_mux);
    if (on && !s_enabled) reset_extremes();
    s_enabled = on;
    portEXIT_CRITICAL(&s_mux);
}

bool bs_integ_enabled(void) { return s_enabled; }

void bs_integ_restore(int64_t q_pc, int64_t ticks)
{
    portENTER_CRITICAL(&s_mux);
    if (!s_enabled) {
        s_acc = 0;
        s_tot.q_pc = q_pc;
        s_tot.ticks = ticks;
        s_tot.pushes = 0;
        reset_extremes();
    }
    portEXIT_CRITICAL(&s_mux);
}

void bs_integ_push(float amps, float volts, uint32_t raw_periods)
{
    if (!s_enabled || raw_periods == 0) return;
    // llrintf: round-to-nearest, unbiased; 1 nA LSB is 6 orders below 1 mA.
    int64_t na = llrintf(amps * 1e9f);
    int64_t uv = llrintf(volts * 1e6f);
    portENTER_CRITICAL(&s_mux);
    int64_t dt = (int64_t)raw_periods * s_period_mclk;
    s_acc += na * dt;
    // 16384 = 2^14: arithmetic shift is an exact floor division and the mask
    // keeps the (non-negative) remainder for the next push.
    s_tot.q_pc += s_acc >> 14;
    s_acc &= (BS_NA_TICKS_PER_PC - 1);
    s_tot.ticks += dt;
    s_tot.v_uv_ticks += uv * dt;
    s_tot.int_ticks += dt;
    s_tot.pushes++;
    if (amps < s_tot.i_min) s_tot.i_min = amps;
    if (amps > s_tot.i_max) s_tot.i_max = amps;
    if (volts < s_tot.v_min) s_tot.v_min = volts;
    if (volts > s_tot.v_max) s_tot.v_max = volts;
    portEXIT_CRITICAL(&s_mux);
}

void bs_integ_snapshot(bs_integ_snap_t *out)
{
    portENTER_CRITICAL(&s_mux);
    *out = s_tot;
    reset_extremes();
    portEXIT_CRITICAL(&s_mux);
}
