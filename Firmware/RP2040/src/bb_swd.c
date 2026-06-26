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
#include "hardware/gpio.h"
#include "pico/time.h"

#ifdef DEBUGPROBE_INTEGRATION
// When integrated with debugprobe, include its headers
#include "tusb.h"        // tud_mounted()
#include "probe.h"       // probe_set_swclk_freq()
#include "DAP.h"         // SWJ_Sequence(), SWD_Transfer(), DAP_TRANSFER_*
#endif

// bb_swd.c provides status queries and clock configuration for the debugprobe 
// SWD engine. It also implements a basic target detection sequence.

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
    // Host-side CMSIS-DAP interface enumeration tells us whether OpenOCD / pyOCD is attached.
    s_status.dap_connected = tud_mounted() && tud_connected();
#endif
    
    if (status) {
        *status = s_status;
    }
}

bool bb_swd_set_clock(uint32_t khz)
{
    if (khz < 100 || khz > 50000) return false;

#ifdef DEBUGPROBE_INTEGRATION
    probe_set_swclk_freq(khz);  // debugprobe expects kHz
#endif

    s_status.swd_clock_khz = khz;
    return true;
}

/**
 * @brief Line reset + JTAG-to-SWD switch using the CMSIS-DAP SWD engine.
 *
 * Uses SWJ_Sequence() from sw_dp_pio.c — the *same* proven code path the USB
 * CMSIS-DAP host (OpenOCD / pyOCD) drives. The previous hand-rolled bit-bang
 * reset mishandled the SWD turnaround and reported "no target" on parts (e.g.
 * STM32F4) that program fine over OpenOCD.
 */
#ifdef DEBUGPROBE_INTEGRATION
static void swd_switch_to_swd(void)
{
    static const uint8_t ones[]  = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
    static const uint8_t j2s[]   = { 0x9E, 0xE7 };  // JTAG-to-SWD switch, LSB-first
    static const uint8_t zeros[] = { 0x00 };

    SWJ_Sequence(51, ones);   // line reset: >= 50 clocks with SWDIO high
    SWJ_Sequence(16, j2s);    // JTAG-to-SWD switch sequence
    SWJ_Sequence(51, ones);   // line reset again (required before first DPIDR read)
    SWJ_Sequence(8,  zeros);  // >= 2 idle cycles
}
#endif

bool bb_swd_detect_target(void)
{
#ifdef DEBUGPROBE_INTEGRATION
    // Configure the SWD pins / PIO (same setup the USB DAP path performs via
    // PORT_SWD_SETUP()). DAP_Setup() ran at boot, so DAP_Data (turnaround,
    // retry counts) is already initialised.
    probe_init();

    // The first DPIDR read after a cold line reset can return WAIT/FAULT before
    // the DP wakes up, so retry the whole connect a few times.
    for (int attempt = 0; attempt < 3; attempt++) {
        swd_switch_to_swd();

        uint32_t dpidr = 0;
        uint8_t ack = SWD_Transfer(DAP_TRANSFER_RnW, &dpidr);  // read DP reg 0 = DPIDR
        if ((ack & 0x7u) == DAP_TRANSFER_OK && dpidr != 0u && dpidr != 0xFFFFFFFFu) {
            s_status.target_detected = true;
            s_status.dpidr = dpidr;
            return true;
        }
    }

    s_status.target_detected = false;
    s_status.dpidr = 0;
    return false;
#else
    // Standalone fallback: Line-level presence check (check for pull-up)
    // Most SWD targets have a 10k-100k pull-up on SWDIO.
    gpio_init(BB_SWD_SWDIO_PIN);
    gpio_set_dir(BB_SWD_SWDIO_PIN, GPIO_IN);
    gpio_pull_down(BB_SWD_SWDIO_PIN);
    sleep_us(100);
    bool detected = gpio_get(BB_SWD_SWDIO_PIN);
    gpio_disable_pulls(BB_SWD_SWDIO_PIN);
    
    s_status.target_detected = detected;
    s_status.dpidr = detected ? 0xDEADBEEF : 0; // Placeholder for standalone
    return detected;
#endif
}
