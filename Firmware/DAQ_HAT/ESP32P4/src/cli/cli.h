#pragma once

// =============================================================================
// cli.h — interactive bring-up console for the DAQ HAT (ESP32-P4).
//
// Runs an esp_console REPL on the built-in USB-Serial-JTAG debug port (J1) —
// SEPARATE from the USB-HS measurement stream (J5, PID 0x4001). Provides system
// status, ADAQ/temperature/SMU readouts and supply-control commands for bench
// bring-up. The DUT supply (V_DUT) stays OFF until explicitly enabled here.
// =============================================================================

#include "esp_err.h"
#include "daq_board.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Start the bring-up REPL on the USB-Serial-JTAG console.
 *
 * @param board  fully-initialised board (borrowed; must outlive the REPL).
 */
esp_err_t daq_cli_start(daq_board_t *board);

#ifdef __cplusplus
}
#endif
