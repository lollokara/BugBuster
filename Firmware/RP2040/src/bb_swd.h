#pragma once

// =============================================================================
// bb_swd.h — SWD management layer
//
// This module does NOT implement SWD protocol — that's handled by debugprobe's
// PIO engine and CMSIS-DAP stack over USB. This module provides:
//   - Query whether USB DAP host is connected
//   - Query whether a target is detected (DPIDR readable)
//   - Read target DPIDR
//   - Adjust SWD clock speed
//
// When integrated with debugprobe, these functions read internal debugprobe
// state variables. In standalone mode, they return placeholder values.
// =============================================================================

#include <stdint.h>
#include <stdbool.h>

typedef struct {
    bool     dap_connected;     // USB host connected to CMSIS-DAP endpoint
    bool     target_detected;   // SWD target responding
    uint32_t dpidr;             // Target Debug Port ID Register
    uint32_t swd_clock_khz;    // Current SWD clock speed in kHz
} SwdStatus;

/**
 * @brief Initialize SWD management (read initial state from debugprobe).
 */
void bb_swd_init(void);

/**
 * @brief Get current SWD/DAP status.
 */
void bb_swd_get_status(SwdStatus *status);

/**
 * @brief Set SWD clock speed. Forwarded to debugprobe's probe_set_swclk_freq().
 * @param khz  Desired clock speed in kHz (100–10000)
 * @return true if accepted
 */
bool bb_swd_set_clock(uint32_t khz);

/**
 * @brief Attempt a target detect (SWD line reset + DPIDR read).
 *        Uses debugprobe's SWD engine if available.
 * @return true if target responded
 */
bool bb_swd_detect_target(void);
