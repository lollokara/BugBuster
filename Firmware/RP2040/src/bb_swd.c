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
// TODO: Expose DPIDR from DAP internals or cache it after first read
#endif

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
    // Read live state from debugprobe / TinyUSB
    s_status.dap_connected = tud_mounted() && tud_connected();
    // TODO: target_detected and dpidr require hooking into DAP_SWD.c
    // For now, cache from last bb_swd_detect_target() call
#endif

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
#ifdef DEBUGPROBE_INTEGRATION
    // Use debugprobe's SWD engine to attempt target detection
    // This requires:
    //   1. SWD line reset (50+ clock cycles with SWDIO high)
    //   2. JTAG-to-SWD switch sequence
    //   3. Read DPIDR register
    //
    // The debugprobe probe.c exposes probe_write_bits() and probe_read_bits()
    // which can be used for raw SWD transactions.
    //
    // TODO: Implement direct SWD DPIDR read using probe low-level API
    // For now, rely on the host (OpenOCD/pyOCD) to detect and cache the result
    //
    // Placeholder: check if last DAP transaction succeeded
    // probe_swd_read(DP, DPIDR_ADDR, &dpidr);
#else
    // Standalone test mode: simulate target detection
    // In real hardware, this would be a SWD transaction
    s_status.target_detected = false;
    s_status.dpidr = 0;
#endif

    return s_status.target_detected;
}
