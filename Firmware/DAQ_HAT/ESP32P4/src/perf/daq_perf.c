// SPDX-License-Identifier: MIT
// daq_perf.c — per-stage cycle profiler storage + self-calibration.

#include "daq_perf.h"

#if DAQ_PERF_ENABLED

#include <string.h>

#include "esp_attr.h"
#include "esp_timer.h"
#include "esp_clk_tree.h"
#include "soc/soc.h"

// Hot data: keep it in internal DRAM. The capture core touches these records
// from a tight spin loop, and a PSRAM round trip there would dominate the very
// measurement it is taking.
volatile bool    g_daq_perf_on = false;
DRAM_ATTR daq_perf_stage_t g_daq_perf[DAQ_PERF_STAGE_COUNT];

static uint64_t s_window_start_us;
static uint32_t s_overhead_cycles;
static uint32_t s_cpu_hz;

static const char *const s_names[DAQ_PERF_STAGE_COUNT] = {
    [DAQ_PERF_CAP_PASS]      = "cap.pass",
    [DAQ_PERF_CAP_SPIN]      = "cap.spin",
    [DAQ_PERF_CAP_SCAN]      = "cap.scan",
    [DAQ_PERF_CAP_DRAIN]     = "cap.drain",
    [DAQ_PERF_CAP_BEGIN]     = "cap.begin",
    [DAQ_PERF_CAP_END]       = "cap.end",
    [DAQ_PERF_FAST_LOOP]     = "fast.loop",
    [DAQ_PERF_FAST_READ_A]   = "fast.readA",
    [DAQ_PERF_FAST_READ_B]   = "fast.readB",
    [DAQ_PERF_FAST_VOLT]     = "fast.volt",
    [DAQ_PERF_FAST_PAIR]     = "fast.pair",
    [DAQ_PERF_FAST_EMIT]     = "fast.emit",
    [DAQ_PERF_FAST_FUSION]   = "fast.fusion",
    [DAQ_PERF_FAST_DSP]      = "fast.dsp",
    [DAQ_PERF_FAST_WIRE]     = "fast.wire",
    [DAQ_PERF_FAST_SUMMARY]  = "fast.summary",
    [DAQ_PERF_FAST_IDLE]     = "fast.idle",
    [DAQ_PERF_FAST_YIELD]    = "fast.yield",
};

const char *daq_perf_stage_name(daq_perf_stage_id_t id)
{
    if ((unsigned)id >= DAQ_PERF_STAGE_COUNT) return "?";
    return s_names[id] ? s_names[id] : "?";
}

// Measure what an empty BEGIN/END pair costs, so a reader can subtract the
// instrument from the measurement. Uses the median-ish approach of taking the
// minimum over many trials: the minimum is the only value not polluted by an
// interrupt landing inside the probe.
static uint32_t measure_overhead(void)
{
    volatile uint32_t sink = 0;
    uint32_t best = UINT32_MAX;
    for (int i = 0; i < 64; ++i) {
        uint32_t a = daq_perf_now();
        uint32_t b = daq_perf_now();
        uint32_t d = b - a;
        sink += d;
        if (d < best) best = d;
    }
    (void)sink;
    return (best == UINT32_MAX) ? 0 : best;
}

void daq_perf_reset(void)
{
    for (int i = 0; i < DAQ_PERF_STAGE_COUNT; ++i) {
        g_daq_perf[i].cycles  = 0;
        g_daq_perf[i].count   = 0;
        g_daq_perf[i].dropped = 0;
        g_daq_perf[i].min     = UINT32_MAX;
        g_daq_perf[i].max     = 0;
    }
    if (s_cpu_hz == 0) {
        uint32_t hz = 0;
        if (esp_clk_tree_src_get_freq_hz(
                SOC_MOD_CLK_CPU, ESP_CLK_TREE_SRC_FREQ_PRECISION_CACHED,
                &hz) == ESP_OK && hz) {
            s_cpu_hz = hz;
        } else {
            s_cpu_hz = CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ * 1000000u;
        }
    }
    s_overhead_cycles = measure_overhead();
    s_window_start_us = (uint64_t)esp_timer_get_time();
}

void daq_perf_enable(bool on)
{
    if (on) {
        daq_perf_reset();
        g_daq_perf_on = true;
    } else {
        g_daq_perf_on = false;
    }
}

void daq_perf_get_window(daq_perf_window_t *out)
{
    if (!out) return;
    out->cpu_hz          = s_cpu_hz ? s_cpu_hz
                                    : CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ * 1000000u;
    out->overhead_cycles = s_overhead_cycles;
    out->window_us       = (uint64_t)esp_timer_get_time() - s_window_start_us;
    out->enabled         = g_daq_perf_on;
}

#endif // DAQ_PERF_ENABLED
