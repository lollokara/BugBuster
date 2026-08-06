// SPDX-License-Identifier: MIT
// daq_perf.h — per-stage cycle profiler for the DAQ HAT acquisition pipeline.
//
// WHY: `faststat` answers "how many samples/s survived", which is a single
// number that says nothing about WHERE the time went. When the pipeline
// saturates you cannot tell from it whether the capture core is SPI-bound, the
// consumer core is DSP-bound, or the wire push is blocking on USB back-pressure
// — and those three have completely different fixes. This module timestamps
// each stage of both hot loops so the answer is measured rather than guessed.
//
// MECHANISM: RISC-V cycle counter (`esp_cpu_get_cycle_count()`), read directly
// in the hot path. It is a CSR read — a few cycles, no memory barrier, no lock.
// The counter is PER CORE and 32-bit, so at 360 MHz it wraps every ~11.9 s;
// only deltas are ever taken and unsigned wraparound makes those correct as
// long as a single stage is shorter than the wrap period (all are, by orders of
// magnitude).
//
// COST: profiling is OFF by default and each instrumented site then costs one
// predictable-branch test of a cached global. With it ON, each stage costs two
// CSR reads plus the accumulate. That is not free, which is exactly why
// daq_perf_reset() self-calibrates the probe overhead into `overhead_cycles` —
// the reported numbers can then be read net of the instrument.
//
// THREADING: every stage is written by exactly ONE core (see the enum split
// below), so no locking is needed. Records are padded to a full 64-byte L2 line
// so the two cores never share a cache line and the profiler cannot itself
// introduce the coherency stalls it is trying to measure.

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_cpu.h"

#ifdef __cplusplus
extern "C" {
#endif

// Master switch. Set to 0 (e.g. `-DDAQ_PERF_ENABLED=0` in platformio.ini) to
// compile the profiler out ENTIRELY: every macro below becomes a no-op, the
// counter array and its DRAM disappear, daq_perf.c compiles to nothing, and the
// `perf` CLI command degrades to a one-line "compiled out" notice. No residual
// branch is left in either hot loop — `DAQ_PERF_ON` folds to a constant 0 and
// the optimiser deletes the guarded code. Keep it at 1 for bench/profiling
// builds; ship with 0 once a tuning pass is finished.
#ifndef DAQ_PERF_ENABLED
#define DAQ_PERF_ENABLED 1
#endif

// L2 cache line on the ESP32-P4 (CONFIG_CACHE_L2_CACHE_LINE_SIZE=64).
#define DAQ_PERF_CACHELINE 64

typedef enum {
    // ---- Core 1: capture_task_comb() in adaq7769_stream.c ----------------
    DAQ_PERF_CAP_PASS = 0,  // one full poll pass that found >=1 DRDY edge
    DAQ_PERF_CAP_SPIN,      // consecutive polls that found NO edge (idle spin)
    DAQ_PERF_CAP_SCAN,      // phase 1: scan latch + trigger one read per host
    DAQ_PERF_CAP_DRAIN,     // phase 2: drain the overlapped SPI transfers
    DAQ_PERF_CAP_BEGIN,     // capture_begin(): queue the SPI transaction
    DAQ_PERF_CAP_END,       // capture_end(): wait for SPI + decode + ring push

    // ---- Core 0: daq_fast_task() in daq_board.c --------------------------
    DAQ_PERF_FAST_LOOP,     // one consumer loop iteration (all of the below)
    DAQ_PERF_FAST_READ_A,   // adaq_stream_read() on bus A (FINE)
    DAQ_PERF_FAST_READ_B,   // bus B drain loop (COARSE + VOLTAGE)
    DAQ_PERF_FAST_VOLT,     // voltage despike + power_dsp_set_voltage + push
    DAQ_PERF_FAST_PAIR,     // FINE/COARSE sequence pairing decision
    DAQ_PERF_FAST_EMIT,     // fast_emit() total
    DAQ_PERF_FAST_FUSION,   // range_manager_step + current_fusion_step + despike
    DAQ_PERF_FAST_DSP,      // decimated DSP tail: power_dsp + spectrum_push
    DAQ_PERF_FAST_WIRE,     // usb_stream_push_sample() (frame assembly + TX)
    DAQ_PERF_FAST_SUMMARY,  // daq_board_stream_summary() (10 Hz STATS/ENERGY/FFT)
    DAQ_PERF_FAST_IDLE,     // vTaskDelay(1) taken because both rings were empty
    DAQ_PERF_FAST_YIELD,    // the forced every-1024-iterations vTaskDelay(1)

    DAQ_PERF_STAGE_COUNT
} daq_perf_stage_id_t;

typedef struct {
    uint64_t cycles;   // summed duration
    uint32_t count;    // number of samples taken
    uint32_t min;      // shortest observed (UINT32_MAX when count == 0)
    uint32_t max;      // longest observed
    uint32_t dropped;  // samples rejected as counter-wrap artifacts
    uint8_t  _pad[DAQ_PERF_CACHELINE - 24];
} daq_perf_stage_t;

_Static_assert(sizeof(daq_perf_stage_t) == DAQ_PERF_CACHELINE,
               "perf record must occupy exactly one cache line");

// The 32-bit cycle counter wraps every ~11.9 s at 360 MHz. Any "duration"
// longer than this is not a real measurement, it is a wrapped delta (or a
// sample straddling a debugger halt). Rejecting them matters: a single wrapped
// outlier landing in a blocking stage swamps the sum and reports >100% of a
// core, which is how this was first noticed.
#define DAQ_PERF_SANE_CYCLES  (1u << 30)   // ~2.98 s at 360 MHz

// Blocking stages get a much tighter bound. Nothing in this pipeline should
// legitimately block for 100 ms: a vTaskDelay(1) is ~1 ms and an idle spin ends
// at the next DRDY. Anything longer is a console stall or a wrapped delta, and
// letting one through visibly skews the mean over a few-second window.
#define DAQ_PERF_SANE_BLOCK_CYCLES  (36000000u)   // 100 ms at 360 MHz

// Stages that measure BLOCKED wall-clock time rather than CPU work. Their
// totals must never be added to a CPU budget — the core is running other tasks
// (or another core entirely) for that whole span.
static inline bool daq_perf_stage_is_blocking(daq_perf_stage_id_t id)
{
    return id == DAQ_PERF_CAP_SPIN ||
           id == DAQ_PERF_FAST_IDLE ||
           id == DAQ_PERF_FAST_YIELD;
}

// Snapshot of everything needed to turn raw cycle sums into percentages.
typedef struct {
    uint32_t cpu_hz;
    uint32_t overhead_cycles;  // measured cost of one BEGIN/END probe pair
    uint64_t window_us;        // esp_timer elapsed since daq_perf_reset()
    bool     enabled;
} daq_perf_window_t;

#if DAQ_PERF_ENABLED

extern volatile bool     g_daq_perf_on;
extern daq_perf_stage_t  g_daq_perf[DAQ_PERF_STAGE_COUNT];

// Use this, never g_daq_perf_on directly, in instrumented code: it is the only
// spelling that also compiles away when DAQ_PERF_ENABLED is 0.
#define DAQ_PERF_ON  (g_daq_perf_on)

/** @brief Zero all counters, re-measure probe overhead, restart the window. */
void daq_perf_reset(void);

/** @brief Turn sampling on or off. Turning ON implies a reset. */
void daq_perf_enable(bool on);

/** @brief Window metadata for converting cycles to wall-clock percentages. */
void daq_perf_get_window(daq_perf_window_t *out);

/** @brief Human-readable stage name, for the CLI dump. */
const char *daq_perf_stage_name(daq_perf_stage_id_t id);

static inline uint32_t daq_perf_now(void)
{
    return (uint32_t)esp_cpu_get_cycle_count();
}

static inline void daq_perf_account(daq_perf_stage_id_t id, uint32_t start)
{
    // Unsigned wraparound makes this correct across the 32-bit counter's
    // ~11.9 s rollover without a branch.
    uint32_t d = daq_perf_now() - start;
    daq_perf_stage_t *s = &g_daq_perf[id];
    uint32_t limit = daq_perf_stage_is_blocking(id) ? DAQ_PERF_SANE_BLOCK_CYCLES
                                                    : DAQ_PERF_SANE_CYCLES;
    if (d >= limit) { s->dropped++; return; }
    s->cycles += d;
    s->count++;
    if (d < s->min) s->min = d;
    if (d > s->max) s->max = d;
}

/** @brief Count an event without timing it (used for the idle-spin tally). */
static inline void daq_perf_tally(daq_perf_stage_id_t id, uint32_t cycles)
{
    daq_perf_stage_t *s = &g_daq_perf[id];
    uint32_t limit = daq_perf_stage_is_blocking(id) ? DAQ_PERF_SANE_BLOCK_CYCLES
                                                    : DAQ_PERF_SANE_CYCLES;
    if (cycles >= limit) { s->dropped++; return; }
    s->cycles += cycles;
    s->count++;
    if (cycles < s->min) s->min = cycles;
    if (cycles > s->max) s->max = cycles;
}

#define DAQ_PERF_BEGIN(var)                                                    \
    uint32_t var = g_daq_perf_on ? daq_perf_now() : 0
#define DAQ_PERF_END(id, var)                                                  \
    do { if (g_daq_perf_on) daq_perf_account((id), (var)); } while (0)
#define DAQ_PERF_TALLY(id, cycles)                                             \
    do { if (g_daq_perf_on) daq_perf_tally((id), (cycles)); } while (0)

#else  // !DAQ_PERF_ENABLED

#define DAQ_PERF_ON                  0
#define DAQ_PERF_BEGIN(var)          ((void)0)
#define DAQ_PERF_END(id, var)        ((void)0)
#define DAQ_PERF_TALLY(id, cycles)   ((void)0)

static inline void daq_perf_reset(void) {}
static inline void daq_perf_enable(bool on) { (void)on; }
// Still declared: instrumented code may call it inside a `DAQ_PERF_ON && ...`
// guard, which folds to dead code but must still compile.
static inline uint32_t daq_perf_now(void) { return 0; }
static inline void daq_perf_get_window(daq_perf_window_t *out)
{
    if (!out) return;
    out->cpu_hz = 0;
    out->overhead_cycles = 0;
    out->window_us = 0;
    out->enabled = false;
}
static inline const char *daq_perf_stage_name(daq_perf_stage_id_t id)
{ (void)id; return "?"; }

#endif // DAQ_PERF_ENABLED

#ifdef __cplusplus
}
#endif
