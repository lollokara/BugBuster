#pragma once

// =============================================================================
// diagnostics.h — onboard-device diagnostics snapshot for the C6 menu
//
// Gathers every readable onboard device (AD7415 board temps, ADAQ7769-1 die
// temps, ESP32-P4 internal temp, fused I/V/P, SMU monitor currents, V_DUT, the
// S3 mainboard telemetry relay, and P4 runtime stats), packs them into the
// shared ddp_diag_t, and pushes it to the C6 over the DDP link (~1 Hz). The C6
// renders the grouped Diagnostics menu and per-sensor live sparklines.
// =============================================================================

#include "esp_err.h"
#include "daq_board.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Install + enable the ESP32-P4 internal temperature sensor used for the
 *        "ESP32-P4 Temp" diagnostics row. Safe to call once at boot; a failure
 *        only blanks that single row (diagnostics_push still works).
 */
esp_err_t diagnostics_init(void);

/**
 * @brief Read the ESP32-P4 internal die temperature (deg C) from the sensor
 *        installed by diagnostics_init(). Returns ESP_ERR_INVALID_STATE if the
 *        sensor is unavailable. Used by the bring-up TUI live temperature panel.
 */
esp_err_t diagnostics_p4_temp_celsius(float *out_c);

/**
 * @brief Gather every readable onboard device and push the snapshot to the C6.
 *        Call periodically (~1 Hz) from the housekeeping loop. ADAQ die temps
 *        are only read while acquisition is stopped (the diagnostic mux shares
 *        the converter with the gapless stream); otherwise they report N/A.
 */
void diagnostics_push(daq_board_t *b);

#ifdef __cplusplus
}
#endif
