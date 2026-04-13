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
 * @brief Helper to perform a full SWD line reset and JTAG-to-SWD switch.
 */
#ifdef DEBUGPROBE_INTEGRATION
static void swd_line_reset_sequence(void)
{
    // 1. Line Reset: 50+ clocks with SWDIO=1
    for (int i = 0; i < 8; i++) {
        probe_write_bits(8, 0xFF);
    }
    
    // 2. JTAG-to-SWD switching sequence (0x79, 0xE7 -> 0xE779 LSB first)
    probe_write_bits(16, 0xE779);
    
    // 3. Line Reset: 50+ clocks with SWDIO=1
    for (int i = 0; i < 8; i++) {
        probe_write_bits(8, 0xFF);
    }
    
    // 4. At least 2 idle cycles (SWDIO=0)
    probe_write_bits(8, 0x00);
}
#endif

bool bb_swd_detect_target(void)
{
#ifdef DEBUGPROBE_INTEGRATION
    // 1. Ensure PIO is initted
    probe_init();
    
    // 2. Perform reset/switch sequence
    swd_line_reset_sequence();
    
    // 3. Request DPIDR read (DP register 0)
    // Start(1) | APnDP(0) | RnW(1) | A[2:3](00) | Parity(1) | Stop(0) | Park(1) = 0xA5
    probe_write_bits(8, 0xA5);
    
    // 4. Read Turnaround(1) + ACK(3)
    // We expect ACK=OK (001 binary, LSB first)
    uint32_t ack_raw = probe_read_bits(4);
    uint8_t ack = (ack_raw >> 1) & 0x07;
    
    if (ack == 1) { // OK
        uint32_t idcode = probe_read_bits(32);
        probe_read_bits(1); // Parity (ignore)
        probe_read_bits(1); // Turnaround
        
        s_status.target_detected = true;
        s_status.dpidr = idcode;
        return true;
    } else {
        s_status.target_detected = false;
        s_status.dpidr = 0;
        return false;
    }
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
