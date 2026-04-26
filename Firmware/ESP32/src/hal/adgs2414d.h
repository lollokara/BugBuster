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

// ERR_CONFIG bits
#define ADGS_ERR_CRC_EN         (1 << 0)  // CRC error detection enable
#define ADGS_ERR_SCLK_EN        (1 << 1)  // SCLK count error detection enable
#define ADGS_ERR_RW_EN          (1 << 2)  // Invalid R/W address error enable

// ERR_FLAGS bits
#define ADGS_ERR_CRC_FLAG       (1 << 0)
#define ADGS_ERR_SCLK_FLAG      (1 << 1)
#define ADGS_ERR_RW_FLAG        (1 << 2)

// Clear error flags command: write 0x6CA9 (special 16-bit SPI frame)
#define ADGS_ERR_CLEAR_HI       0x6C
#define ADGS_ERR_CLEAR_LO       0xA9

// Max retries for verified write
#define ADGS_MAX_RETRIES        3

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

// Public API always exposes the 4 main MUX bytes even on breadboard builds
// that only populate a single physical ADGS2414D device.
#define ADGS_API_MAIN_DEVICES   4

// GPIO mapping: which ESP GPIO drives each MUX switch input
// [device][group] → ESP GPIO number for the direct-drive switch
// Group A = S1/S2, Group B = S5/S6, Group C = S7/S8
static const uint8_t MUX_GPIO_MAP[4][3] = {
    { 1,  2,  4},   // U10 (MUX1): IO3→S1,  IO2→S5,  IO1→S7
    { 5,  6,  7},   // U11 (MUX2): IO6→S1,  IO5→S5,  IO4→S7
    {13, 12, 11},   // U16 (MUX3): IO12→S1, IO11→S5, IO10→S7
    {10,  9,  8},   // U17 (MUX4): IO9→S1,  IO8→S5,  IO7→S7
};

/**
 * @brief Initialize the ADGS2414D mux matrix.
 *        Configures CS pin, enables level shifters, resets all switches,
 *        and enters daisy-chain mode.
 *
 * @return true on success; false if the SPI device could not be registered.
 *         On failure, subsequent adgs_set_*() calls silently no-op so the
 *         rest of the firmware keeps running, but the caller should mark
 *         the MUX subsystem as faulted in g_deviceState so /api/status
 *         reflects reality rather than appearing healthy.
 */
bool adgs_init(void);

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
 * @brief Get the 4-byte public MUX API state.
 *        In breadboard mode, bytes for non-populated devices are shadowed in
 *        software so tests and clients can still round-trip the full API.
 */
void adgs_get_api_states(uint8_t out[ADGS_API_MAIN_DEVICES]);

/**
 * @brief Set the 4-byte public MUX API state with safety dead time.
 *        Only physically populated devices are driven in hardware.
 */
void adgs_set_api_all_safe(const uint8_t states[ADGS_API_MAIN_DEVICES]);

/**
 * @brief Set a single switch in the 4-byte public MUX API state.
 *        Non-populated devices are updated in software only.
 * @return true on accepted parameters, false on invalid device/switch.
 */
bool adgs_set_api_switch_safe(uint8_t device, uint8_t sw, bool closed);

/**
 * @brief Reset all switches to open (safe state).
 */
void adgs_reset_all(void);

/**
 * @brief Read back current switch states from the hardware via SPI.
 *        In address mode: reads SW_DATA register.
 *        In daisy-chain mode: performs a read cycle that shifts out states.
 *
 * @param out  Array to fill with read-back states (ADGS_NUM_DEVICES bytes)
 * @return true on success (readback matches cache), false on mismatch
 */
bool adgs_readback_verify(uint8_t out[ADGS_NUM_DEVICES]);

/**
 * @brief Check error flags on a single device (address mode only).
 *        Reads ERR_FLAGS register (0x03).
 *
 * @return Error flags byte (bits: 0=CRC, 1=SCLK, 2=RW), 0 = no errors.
 *         Returns 0xFF if read fails or not in address mode.
 */
uint8_t adgs_read_error_flags(void);

/**
 * @brief Clear the error flags register by sending the special 0x6CA9 command.
 */
void adgs_clear_error_flags(void);

/**
 * @brief Check if the MUX is in a fault state.
 *        A fault means the last write-verify failed after retries.
 */
bool adgs_is_faulted(void);

#if ADGS_HAS_SELFTEST
/**
 * @brief Set the self-test device (U23) switch state.
 *        Enforces the safety interlock: returns false if U17 S2 is closed.
 *
 * @param sw_byte  8-bit switch mask for U23
 * @return true on success, false if interlock violation
 */
bool adgs_set_selftest(uint8_t sw_byte);

/**
 * @brief Get the cached U23 switch state.
 */
uint8_t adgs_get_selftest(void);

/**
 * @brief Check if U17 S2 is currently closed (IO 9 analog mode).
 *        When true, U23 must NOT close any switches.
 */
bool adgs_u17_s2_active(void);

/**
 * @brief Check if U23 has any switches closed (self-test active).
 *        When true, U17 S2 must NOT be closed.
 */
bool adgs_selftest_active(void);
#endif

/**
 * @brief Test ADGS2414D in address mode (no daisy-chain).
 *        Writes SW_DATA register and reads it back.
 *        For debugging single-device setups.
 * @param sw_data  Switch data byte (bit0=S1 ... bit7=S8)
 * @return Read-back value (should match sw_data if device responds)
 */
uint8_t adgs_test_address_mode(uint8_t sw_data);

/**
 * @brief Soft-reset the ADGS.
 *        This only works in single-device address mode. In daisy-chain mode,
 *        the datasheet requires a hardware reset to exit/restore configuration.
 */
void adgs_soft_reset(void);

#ifdef __cplusplus
}
#endif
