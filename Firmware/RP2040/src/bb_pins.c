// =============================================================================
// bb_pins.c — EXP_EXT pin configuration
//
// Manages the 4 EXP_EXT lines exposed on the HAT expansion connector.
// Each pin can be Disconnected or assigned a GPIO role.
//
// SWD / TRACE used to be assignable here, but the new HAT PCB (2026-04-09)
// routes SWD (SWDIO/SWCLK/TRACE) to a dedicated 3-pin connector driven by
// the debugprobe PIO on its own fixed RP2040 GPIOs. The SWDIO/SWCLK/TRACE1/
// TRACE2 function codes (enum slots 1-4) are therefore REMOVED from the
// assignable set. The numeric slots stay reserved for wire-protocol
// compatibility; bb_pins_set() rejects them explicitly.
// =============================================================================

#include "bb_pins.h"
#include "bb_config.h"
#include "hardware/gpio.h"
#include <string.h>

static uint8_t s_pin_func[BB_NUM_EXT_PINS] = {0};  // All disconnected

static const uint8_t PIN_MAP[BB_NUM_EXT_PINS] = {
    BB_EXT1_PIN, BB_EXT2_PIN, BB_EXT3_PIN, BB_EXT4_PIN
};

// Enum slots 1..4 are reserved (formerly SWDIO/SWCLK/TRACE1/TRACE2).
// Any attempt to assign them is treated as invalid and the pin falls back
// to the last-known-good state.
static bool bb_pins_func_is_reserved(uint8_t function)
{
    return function >= 1 && function <= 4;
}

static void apply_pin_config(uint8_t pin_idx)
{
    if (pin_idx >= BB_NUM_EXT_PINS) return;
    uint gpio = PIN_MAP[pin_idx];
    uint8_t func = s_pin_func[pin_idx];

    switch (func) {
    case HAT_FUNC_GPIO1:
    case HAT_FUNC_GPIO2:
    case HAT_FUNC_GPIO3:
    case HAT_FUNC_GPIO4:
        // General GPIO — default to input with pull-down
        gpio_init(gpio);
        gpio_set_dir(gpio, GPIO_IN);
        gpio_pull_down(gpio);
        break;

    case HAT_FUNC_DISCONNECTED:
    default:
        // High-impedance: configure as input, no pulls.
        // Reserved function codes (SWDIO/SWCLK/TRACE*) also land here —
        // they should have been rejected by bb_pins_set() before reaching
        // apply_pin_config(), but we fail safe just in case.
        gpio_init(gpio);
        gpio_set_dir(gpio, GPIO_IN);
        gpio_disable_pulls(gpio);
        break;
    }
}

void bb_pins_init(void)
{
    memset(s_pin_func, HAT_FUNC_DISCONNECTED, sizeof(s_pin_func));
    for (int i = 0; i < BB_NUM_EXT_PINS; i++) {
        apply_pin_config(i);
    }
}

bool bb_pins_set(uint8_t pin, uint8_t function)
{
    if (pin >= BB_NUM_EXT_PINS) return false;
    if (function > HAT_FUNC_GPIO4) return false;
    if (bb_pins_func_is_reserved(function)) {
        // SWDIO/SWCLK/TRACE1/TRACE2 are no longer assignable to EXP_EXT —
        // SWD now lives on the dedicated 3-pin connector.
        return false;
    }
    s_pin_func[pin] = function;
    apply_pin_config(pin);
    return true;
}

void bb_pins_set_all(const uint8_t functions[4])
{
    for (int i = 0; i < BB_NUM_EXT_PINS; i++) {
        uint8_t f = functions[i];
        if (f > HAT_FUNC_GPIO4 || bb_pins_func_is_reserved(f)) {
            s_pin_func[i] = HAT_FUNC_DISCONNECTED;
        } else {
            s_pin_func[i] = f;
        }
    }
    for (int i = 0; i < BB_NUM_EXT_PINS; i++) {
        apply_pin_config(i);
    }
}

void bb_pins_get_all(uint8_t functions[4])
{
    memcpy(functions, s_pin_func, BB_NUM_EXT_PINS);
}

void bb_pins_reset(void)
{
    memset(s_pin_func, HAT_FUNC_DISCONNECTED, sizeof(s_pin_func));
    for (int i = 0; i < BB_NUM_EXT_PINS; i++) {
        apply_pin_config(i);
    }
}

const char *bb_pins_func_name(uint8_t function)
{
    switch (function) {
    case HAT_FUNC_DISCONNECTED: return "Disconnected";
    case HAT_FUNC_GPIO1:        return "GPIO1";
    case HAT_FUNC_GPIO2:        return "GPIO2";
    case HAT_FUNC_GPIO3:        return "GPIO3";
    case HAT_FUNC_GPIO4:        return "GPIO4";
    default:
        if (function >= 1 && function <= 4) {
            // Reserved enum slots 1-4 — formerly SWDIO/SWCLK/TRACE1/TRACE2.
            return "Reserved (deprecated)";
        }
        return "Unknown";
    }
}
