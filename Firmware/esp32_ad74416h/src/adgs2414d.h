#pragma once

// =============================================================================
// adgs2414d.h - ADGS2414D Octal SPST Switch Driver (4x daisy-chain)
//
// Controls 4 ADGS2414D switches on the shared SPI bus (different CS from ADC).
// Uses daisy-chain mode: one CS frame sends 4 bytes to set all 32 switches.
// Enforces 100ms dead time on group switching to protect level shifters.
// =============================================================================

#include <stdint.h>
#include <stdbool.h>
#include "config.h"

#ifdef __cplusplus
extern "C" {
#endif

// ADGS2414D Register Map (address mode)
#define ADGS_REG_SW_DATA        0x01
#define ADGS_REG_ERR_CONFIG     0x02
#define ADGS_REG_ERR_FLAGS      0x03
#define ADGS_REG_BURST_EN       0x05
#define ADGS_REG_SOFT_RESET     0x0B

// SPI command bits
#define ADGS_CMD_WRITE          0x00
#define ADGS_CMD_READ           0x80

// Enter daisy-chain mode command
#define ADGS_DAISY_CHAIN_CMD_HI 0x25
#define ADGS_DAISY_CHAIN_CMD_LO 0x00

// Soft reset sequence
#define ADGS_SOFT_RESET_VAL1    0xA3
#define ADGS_SOFT_RESET_VAL2    0x05

// Switch group masks
#define ADGS_GROUP_A_MASK       0x0F  // SW1-SW4 (bits 0-3)
#define ADGS_GROUP_B_MASK       0x30  // SW5-SW6 (bits 4-5)
#define ADGS_GROUP_C_MASK       0xC0  // SW7-SW8 (bits 6-7)

// GPIO mapping: which ESP GPIO drives each MUX switch input
// [device][group] → ESP GPIO number for the direct-drive switch
// Group A = S1/S2, Group B = S5/S6, Group C = S7/S8
static const uint8_t MUX_GPIO_MAP[4][3] = {
    { 1,  2,  3},   // U10 (MUX1): IO1→S1, IO2→S5, IO3→S7
    { 5,  6,  7},   // U11 (MUX2): IO5→S1, IO6→S5, IO7→S7
    {13, 12, 11},   // U16 (MUX3): IO13→S1, IO12→S5, IO11→S7
    {10,  9,  8},   // U17 (MUX4): IO10→S1, IO9→S5, IO8→S7
};

/**
 * @brief Initialize the ADGS2414D mux matrix.
 *        Configures CS pin, enables level shifters, resets all switches,
 *        and enters daisy-chain mode.
 */
void adgs_init(void);

/**
 * @brief Set all 4 devices' switch states in one SPI transaction.
 *        Does NOT enforce dead time — caller is responsible.
 *
 * @param states  Array of 4 bytes, one per device (bit 0=SW1 ... bit 7=SW8)
 */
void adgs_set_all_raw(const uint8_t states[ADGS_NUM_DEVICES]);

/**
 * @brief Set all 4 devices' switch states with safety dead time.
 *        Opens all switches first, waits ADGS_DEAD_TIME_MS, then sets new state.
 *
 * @param states  Array of 4 bytes
 */
void adgs_set_all_safe(const uint8_t states[ADGS_NUM_DEVICES]);

/**
 * @brief Set a single switch with dead time protection.
 *        Opens all switches in the same group first, waits, then closes.
 *
 * @param device   Device index 0-3
 * @param sw       Switch index 0-7
 * @param closed   true = close switch, false = open switch
 */
void adgs_set_switch_safe(uint8_t device, uint8_t sw, bool closed);

/**
 * @brief Get the cached switch state for a device.
 * @param device  Device index 0-3
 * @return 8-bit switch state mask
 */
uint8_t adgs_get_state(uint8_t device);

/**
 * @brief Get all 4 cached device states.
 * @param out  Array to fill with 4 device states
 */
void adgs_get_all_states(uint8_t out[ADGS_NUM_DEVICES]);

/**
 * @brief Reset all switches to open (safe state).
 */
void adgs_reset_all(void);

#ifdef __cplusplus
}
#endif
