#pragma once

// =============================================================================
// daq_settings_glue.h — bind the authoritative settings store to the P4 board.
//
// daq_settings.c is subsystem-agnostic: it stores/validates/persists values and
// calls an "apply" callback. This glue translates each registry key into the
// concrete subsystem call on a daq_board_t (range manager, SMU, spectrum, power
// DSP, USB stream) and routes the reset actions.
//
// Call daq_board_bind_settings() once, AFTER daq_board_init() (so the SMU,
// range manager, spectrum and DSP exist) and BEFORE the fast acquisition path
// starts. It initialises the store, registers the callbacks, and applies every
// persisted value to its subsystem.
// =============================================================================

#include "daq_board.h"

#ifdef __cplusplus
extern "C" {
#endif

// Initialise the settings store, bind apply/action callbacks to @b, and apply
// all persisted/default values to the subsystems.
void daq_board_bind_settings(daq_board_t *b);

#ifdef __cplusplus
}
#endif
