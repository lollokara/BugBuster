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

    // HVPAK driver is a stub — the real register map and DAC conversion
    // are TODO (see bb_hvpak.h). Until the real driver lands we DO NOT
    // attempt the I2C transaction because:
    //   (a) the HVPAK chip is not populated on the breadboard, so every
    //       write NAKs; and
    //   (b) without external pull-ups the RP2040's i2c_write_blocking
    //       can stall the I2C engine waiting for SDA to release, which
    //       then blocks the HAT UART command handler task and looks like
    //       a "BUSY" to the ESP32 side. That false-BUSY cascade was the
    //       root cause of hat_setup_swd failing end-to-end on a
    //       breadboard where everything else was wired correctly.
    //
    // Returning false (with the ESP32's HAT_ERR_INVALID_FUNC wire
    // response) is what the ESP32's hat_setup_swd() 3.3 V fallback
    // expects — it will accept the request as "already satisfied" at
    // the default voltage and continue the SWD setup sequence.
    s_voltage_mv = mv;
    return false;
}

uint16_t bb_hvpak_get_voltage(void)
{
    return s_voltage_mv;
}

bool bb_hvpak_is_ready(void)
{
    return s_initialized;
}
