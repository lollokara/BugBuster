// =============================================================================
// bb_hvpak.c — Renesas HVPAK level translator driver (stub)
//
// This is a placeholder implementation. The actual I2C register writes
// must be filled in once the specific HVPAK part number is known.
//
// The HVPAK provides programmable voltage level translation for the
// EXP_EXT lines between BugBuster's 3.3V logic and the target voltage.
// =============================================================================

#include "bb_hvpak.h"
#include "bb_config.h"
#include "hardware/i2c.h"
#include "hardware/gpio.h"
#include "pico/stdlib.h"

static uint16_t s_voltage_mv = BB_HVPAK_DEFAULT_MV;
static bool s_initialized = false;

void bb_hvpak_init(void)
{
    // Initialize I2C for HVPAK communication
    i2c_init(BB_HVPAK_I2C, BB_HVPAK_I2C_FREQ);
    gpio_set_function(BB_HVPAK_SDA_PIN, GPIO_FUNC_I2C);
    gpio_set_function(BB_HVPAK_SCL_PIN, GPIO_FUNC_I2C);
    gpio_pull_up(BB_HVPAK_SDA_PIN);
    gpio_pull_up(BB_HVPAK_SCL_PIN);

    // Set default voltage
    s_voltage_mv = BB_HVPAK_DEFAULT_MV;
    s_initialized = true;

    // TODO: Probe HVPAK device on I2C bus
    // TODO: Read HVPAK device ID / status register
    // TODO: Set initial voltage to BB_HVPAK_DEFAULT_MV
}

bool bb_hvpak_set_voltage(uint16_t mv)
{
    if (!s_initialized) return false;
    if (mv < BB_HVPAK_MIN_MV || mv > BB_HVPAK_MAX_MV) return false;

    // TODO: Convert voltage to HVPAK DAC code / register value
    // The exact conversion depends on the HVPAK variant:
    //   - Some use a DAC code proportional to voltage
    //   - Some use a resistor divider with programmable taps
    //   - Refer to the specific HVPAK datasheet for register map
    //
    // Placeholder: write voltage as 16-bit LE to a control register
    uint8_t data[3];
    data[0] = 0x00;                     // Register address (placeholder)
    data[1] = (uint8_t)(mv & 0xFF);     // Voltage LSB
    data[2] = (uint8_t)(mv >> 8);       // Voltage MSB

    int ret = i2c_write_blocking(BB_HVPAK_I2C, BB_HVPAK_I2C_ADDR, data, 3, false);
    if (ret < 0) return false;

    s_voltage_mv = mv;
    sleep_ms(2);  // Allow HVPAK voltage to settle

    return true;
}

uint16_t bb_hvpak_get_voltage(void)
{
    return s_voltage_mv;
}

bool bb_hvpak_is_ready(void)
{
    return s_initialized;
}
