#pragma once

// =============================================================================
// ext_bus_errmap.h - BUS-017: map an external-bus esp_err_t to a CmdError so a
// NACK (no device), a stuck bus and a contended bus stay distinguishable.
// Header-only so the mapping is host-testable (tests/firmware_host).
// =============================================================================

#include "esp_err.h"
#include "cmd_errors.h"
#include "ext_bus.h"

static inline int ext_bus_cmd_error(esp_err_t err)
{
    switch (err) {
        case ESP_OK:                return CMD_OK;
        case ESP_FAIL:              return CMD_ERR_HARDWARE;       // NACK / arbitration lost
        case ESP_ERR_TIMEOUT:       return CMD_ERR_TIMEOUT;        // bus held low / stuck
        case EXT_BUS_ERR_MUTEX:     return CMD_ERR_BUSY;
        case ESP_ERR_INVALID_STATE: return CMD_ERR_INVALID_STATE;  // not set up
        case ESP_ERR_INVALID_ARG:   return CMD_ERR_BAD_ARG;
        default:                    return CMD_ERR_HARDWARE;
    }
}
