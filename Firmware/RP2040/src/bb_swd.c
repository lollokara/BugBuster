// =============================================================================
// bb_swd.c — SWD management layer
//
// Provides status queries and clock configuration for the debugprobe SWD engine.
//
// Integration with debugprobe:
//   When DEBUGPROBE_INTEGRATION is defined, these functions access debugprobe's
//   internal state (probe.c, tusb callbacks, DAP state). When standalone,
//   they return placeholder values for testing the command protocol.
//
// To integrate:
//   1. #define DEBUGPROBE_INTEGRATION before including this file
//   2. Expose these from debugprobe:
//      - extern bool tud_mounted(void);          // TinyUSB: host connected
//      - extern void probe_set_swclk_freq(uint freq_khz);
//      - Access to last DPIDR read from DAP_SWD.c
// =============================================================================

#include "bb_swd.h"
#include "bb_config.h"
#include <string.h>

#ifdef DEBUGPROBE_INTEGRATION
// When integrated with debugprobe, include its headers
#include "tusb.h"        // tud_mounted()
#include "probe.h"       // probe_set_swclk_freq()
#endif

// bb_swd.c intentionally does NOT implement direct SWD DPIDR reads or
// target detection. Those require reaching into the debugprobe
// submodule (DAP_SWD.c / probe.c low-level API) which is vendor code and
// would need a non-trivial integration layer. The BugBuster approach is
// to let the host-side CMSIS-DAP driver (OpenOCD, pyOCD, probe-rs) do the
// detection and DPIDR read, and only surface the last-known-good values
// from this module.
//
// As a result:
//   - bb_swd_get_status() reports dap_connected based on tud_mounted()
//     only (does the host see the CMSIS-DAP interface?).
//   - target_detected and dpidr are always 0 unless the caller explicitly
//     set them via a future hook. They are NOT fabricated.
//   - bb_swd_detect_target() is an honest no-op that returns false and
//     leaves the cached values alone.
//
// See .omc/specs/deep-interview-swd-exp-ext-cleanup-2026-04-09.md for the
// design decision and the path to a real implementation.

static SwdStatus s_status = {
    .dap_connected = false,
    .target_detected = false,
    .dpidr = 0,
    .swd_clock_khz = 1000,   // Default 1 MHz
};

void bb_swd_init(void)
{
    memset(&s_status, 0, sizeof(s_status));
    s_status.swd_clock_khz = 1000;  // Default SWD clock
}

void bb_swd_get_status(SwdStatus *status)
{
#ifdef DEBUGPROBE_INTEGRATION
    // Host-side CMSIS-DAP interface enumeration is the only thing we can
    // observe cheaply — it tells us whether OpenOCD / pyOCD is attached.
    s_status.dap_connected = tud_mounted() && tud_connected();
#endif
    // target_detected and dpidr stay at whatever the last successful host
    // transaction left them — we do NOT invent values here.

    if (status) {
        *status = s_status;
    }
}

bool bb_swd_set_clock(uint32_t khz)
{
    if (khz < 100 || khz > 50000) return false;

#ifdef DEBUGPROBE_INTEGRATION
    probe_set_swclk_freq(khz * 1000);  // debugprobe expects Hz
#endif

    s_status.swd_clock_khz = khz;
    return true;
}

bool bb_swd_detect_target(void)
{
    // Honest no-op: we do not run a direct SWD transaction from here.
    // The host-side driver handles DPIDR read; BugBuster just relays
    // USB CMSIS-DAP packets through the debugprobe PIO.
    //
    // Returning the cached value lets higher layers display the last
    // known state without this function ever pretending to have probed.
    return s_status.target_detected;
}
