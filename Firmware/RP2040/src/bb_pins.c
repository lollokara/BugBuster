// =============================================================================
// bb_pins.c — EXP_EXT pin configuration
//
// Manages the 4 EXP_EXT lines routed through the HVPAK level translator.
// Each pin can be configured as SWD, trace, or GPIO function.
// GPIO direction and state are controlled by the RP2040.
// =============================================================================

#include "bb_pins.h"
#include "bb_config.h"
#include "hardware/gpio.h"
#include <string.h>

static uint8_t s_pin_func[BB_NUM_EXT_PINS] = {0};  // All disconnected

static const uint8_t PIN_MAP[BB_NUM_EXT_PINS] = {
    BB_EXT1_PIN, BB_EXT2_PIN, BB_EXT3_PIN, BB_EXT4_PIN
};

static void apply_pin_config(uint8_t pin_idx)
{
    if (pin_idx >= BB_NUM_EXT_PINS) return;
    uint gpio = PIN_MAP[pin_idx];
    uint8_t func = s_pin_func[pin_idx];

    switch (func) {
    case HAT_FUNC_DISCONNECTED:
        // High-impedance: configure as input, no pulls
        gpio_init(gpio);
        gpio_set_dir(gpio, GPIO_IN);
        gpio_disable_pulls(gpio);
        break;

    case HAT_FUNC_SWDIO:
        // Bidirectional — managed by PIO/SWD engine
        // For now, set as input; debugprobe PIO will take over
        gpio_init(gpio);
        gpio_set_dir(gpio, GPIO_IN);
        break;

    case HAT_FUNC_SWCLK:
        // Output — managed by PIO/SWD engine
        gpio_init(gpio);
        gpio_set_dir(gpio, GPIO_OUT);
        gpio_put(gpio, 0);
        break;

    case HAT_FUNC_TRACE1:
    case HAT_FUNC_TRACE2:
        // Input — capture from target
        gpio_init(gpio);
        gpio_set_dir(gpio, GPIO_IN);
        gpio_pull_down(gpio);
        break;

    case HAT_FUNC_GPIO1:
    case HAT_FUNC_GPIO2:
    case HAT_FUNC_GPIO3:
    case HAT_FUNC_GPIO4:
        // General GPIO — default to input with pull-down
        gpio_init(gpio);
        gpio_set_dir(gpio, GPIO_IN);
        gpio_pull_down(gpio);
        break;

    default:
        // Unknown: high-impedance
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

void bb_pins_set(uint8_t pin, uint8_t function)
{
    if (pin >= BB_NUM_EXT_PINS) return;
    if (function > HAT_FUNC_GPIO4) return;
    s_pin_func[pin] = function;
    apply_pin_config(pin);
}

void bb_pins_set_all(const uint8_t functions[4])
{
    for (int i = 0; i < BB_NUM_EXT_PINS; i++) {
        s_pin_func[i] = (functions[i] <= HAT_FUNC_GPIO4) ? functions[i] : HAT_FUNC_DISCONNECTED;
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
    case HAT_FUNC_SWDIO:        return "SWDIO";
    case HAT_FUNC_SWCLK:        return "SWCLK";
    case HAT_FUNC_TRACE1:       return "TRACE1";
    case HAT_FUNC_TRACE2:       return "TRACE2";
    case HAT_FUNC_GPIO1:        return "GPIO1";
    case HAT_FUNC_GPIO2:        return "GPIO2";
    case HAT_FUNC_GPIO3:        return "GPIO3";
    case HAT_FUNC_GPIO4:        return "GPIO4";
    default:                     return "Unknown";
    }
}
