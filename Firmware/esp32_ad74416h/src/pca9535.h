#pragma once

// =============================================================================
// pca9535.h - PCA9535AHF 16-bit I2C GPIO Expander Driver
//
// Pin mapping (from schematic):
//   Port 0:
//     P0.0 = LOGIC_PG      (Input)  - Main logic power good
//     P0.1 = VADJ_1_PG     (Input)  - V_ADJ1 power good
//     P0.2 = VADJ_1_EN     (Output) - V_ADJ1 enable
//     P0.3 = VADJ_2_EN     (Output) - V_ADJ2 enable
//     P0.4 = VADJ_2_PG     (Input)  - V_ADJ2 power good
//     P0.5 = EN_15V_A      (Output) - ±15V analog supply enable
//     P0.6 = EN_MUX        (Output) - MUX power enable
//     P0.7 = EN_USB_HUB    (Output) - USB hub enable
//
//   Port 1:
//     P1.0 = EFUSE_EN_1    (Output) - E-Fuse 1 enable (→ P1)
//     P1.1 = EFUSE_FLT_1   (Input)  - E-Fuse 1 fault
//     P1.2 = EFUSE_EN_2    (Output) - E-Fuse 2 enable (→ P2)
//     P1.3 = EFUSE_FLT_2   (Input)  - E-Fuse 2 fault
//     P1.4 = EFUSE_EN_3    (Output) - E-Fuse 3 enable (→ P3)
//     P1.5 = EFUSE_FLT_3   (Input)  - E-Fuse 3 fault
//     P1.6 = EFUSE_EN_4    (Output) - E-Fuse 4 enable (→ P4)
//     P1.7 = EFUSE_FLT_4   (Input)  - E-Fuse 4 fault
// =============================================================================

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// PCA9535 Register Addresses
#define PCA9535_REG_INPUT0   0x00  // Input Port 0 (read-only)
#define PCA9535_REG_INPUT1   0x01  // Input Port 1 (read-only)
#define PCA9535_REG_OUTPUT0  0x02  // Output Port 0
#define PCA9535_REG_OUTPUT1  0x03  // Output Port 1
#define PCA9535_REG_POLAR0   0x04  // Polarity Inversion Port 0
#define PCA9535_REG_POLAR1   0x05  // Polarity Inversion Port 1
#define PCA9535_REG_CONFIG0  0x06  // Configuration Port 0
#define PCA9535_REG_CONFIG1  0x07  // Configuration Port 1

// Port 0 pin masks
#define PCA9535_LOGIC_PG     (1 << 0)  // P0.0 Input
#define PCA9535_VADJ1_PG     (1 << 1)  // P0.1 Input
#define PCA9535_VADJ1_EN     (1 << 2)  // P0.2 Output
#define PCA9535_VADJ2_EN     (1 << 3)  // P0.3 Output
#define PCA9535_VADJ2_PG     (1 << 4)  // P0.4 Input
#define PCA9535_EN_15V_A     (1 << 5)  // P0.5 Output
#define PCA9535_EN_MUX       (1 << 6)  // P0.6 Output
#define PCA9535_EN_USB_HUB   (1 << 7)  // P0.7 Output

// Port 0: input mask (pins configured as inputs)
// P0.0 (LOGIC_PG), P0.1 (VADJ1_PG), P0.4 (VADJ2_PG)
#define PCA9535_PORT0_INPUT_MASK  (PCA9535_LOGIC_PG | PCA9535_VADJ1_PG | PCA9535_VADJ2_PG)
// Port 0: output mask
#define PCA9535_PORT0_OUTPUT_MASK (PCA9535_VADJ1_EN | PCA9535_VADJ2_EN | PCA9535_EN_15V_A | PCA9535_EN_MUX | PCA9535_EN_USB_HUB)

// Port 1 pin masks (E-Fuses)
#define PCA9535_EFUSE_EN_1   (1 << 0)  // P1.0 Output
#define PCA9535_EFUSE_FLT_1  (1 << 1)  // P1.1 Input
#define PCA9535_EFUSE_EN_2   (1 << 2)  // P1.2 Output
#define PCA9535_EFUSE_FLT_2  (1 << 3)  // P1.3 Input
#define PCA9535_EFUSE_EN_3   (1 << 4)  // P1.4 Output
#define PCA9535_EFUSE_FLT_3  (1 << 5)  // P1.5 Input
#define PCA9535_EFUSE_EN_4   (1 << 6)  // P1.6 Output
#define PCA9535_EFUSE_FLT_4  (1 << 7)  // P1.7 Input

// Port 1: input mask (fault pins)
#define PCA9535_PORT1_INPUT_MASK  (PCA9535_EFUSE_FLT_1 | PCA9535_EFUSE_FLT_2 | PCA9535_EFUSE_FLT_3 | PCA9535_EFUSE_FLT_4)
// Port 1: output mask (enable pins)
#define PCA9535_PORT1_OUTPUT_MASK (PCA9535_EFUSE_EN_1 | PCA9535_EFUSE_EN_2 | PCA9535_EFUSE_EN_3 | PCA9535_EFUSE_EN_4)

// Fault event types
typedef enum {
    PCA_FAULT_EFUSE_TRIP = 0,   // E-fuse fault asserted (overcurrent, active low from TPS1641)
    PCA_FAULT_EFUSE_CLEAR,      // E-fuse fault cleared
    PCA_FAULT_PG_LOST,          // Power-good signal lost
    PCA_FAULT_PG_RESTORED,      // Power-good signal restored
} PcaFaultType;

// Fault event structure
typedef struct {
    PcaFaultType type;
    uint8_t      channel;       // E-fuse index (0-3) or PG source (0=logic, 1=vadj1, 2=vadj2)
    uint32_t     timestamp_ms;
} PcaFaultEvent;

// Callback signature: called from ISR task context (not ISR itself — safe for I2C/logging)
typedef void (*pca9535_fault_cb_t)(const PcaFaultEvent *event);

// Fault behavior configuration
typedef struct {
    bool auto_disable_efuse;    // If true, auto-disable faulted e-fuse (default: true)
    bool log_events;            // If true, log all fault events to console (default: true)
} PcaFaultConfig;

// Named control IDs for high-level API
typedef enum {
    PCA_CTRL_VADJ1_EN = 0,   // V_ADJ1 enable
    PCA_CTRL_VADJ2_EN,       // V_ADJ2 enable
    PCA_CTRL_15V_EN,         // ±15V analog enable
    PCA_CTRL_MUX_EN,         // MUX power enable
    PCA_CTRL_USB_HUB_EN,     // USB hub enable
    PCA_CTRL_EFUSE1_EN,      // E-Fuse 1 enable
    PCA_CTRL_EFUSE2_EN,      // E-Fuse 2 enable
    PCA_CTRL_EFUSE3_EN,      // E-Fuse 3 enable
    PCA_CTRL_EFUSE4_EN,      // E-Fuse 4 enable
    PCA_CTRL_COUNT
} PcaControl;

// Named status IDs for high-level API
typedef enum {
    PCA_STATUS_LOGIC_PG = 0,   // Main logic power good
    PCA_STATUS_VADJ1_PG,       // V_ADJ1 power good
    PCA_STATUS_VADJ2_PG,       // V_ADJ2 power good
    PCA_STATUS_EFUSE1_FLT,     // E-Fuse 1 fault (active low)
    PCA_STATUS_EFUSE2_FLT,     // E-Fuse 2 fault (active low)
    PCA_STATUS_EFUSE3_FLT,     // E-Fuse 3 fault (active low)
    PCA_STATUS_EFUSE4_FLT,     // E-Fuse 4 fault (active low)
    PCA_STATUS_COUNT
} PcaStatus;

// Complete device state
typedef struct {
    bool     present;

    // Raw register values
    uint8_t  input0;     // Port 0 input register
    uint8_t  input1;     // Port 1 input register
    uint8_t  output0;    // Port 0 output register (cached)
    uint8_t  output1;    // Port 1 output register (cached)

    // Decoded status (from inputs)
    bool     logic_pg;
    bool     vadj1_pg;
    bool     vadj2_pg;
    bool     efuse_flt[4];  // true = fault active (low)

    // Decoded enables (from outputs)
    bool     vadj1_en;
    bool     vadj2_en;
    bool     en_15v;
    bool     en_mux;
    bool     en_usb_hub;
    bool     efuse_en[4];
} PCA9535State;

/**
 * @brief Initialize PCA9535 driver. Probes device, configures pin directions,
 *        and sets all outputs to OFF (safe start).
 * @return true if device found
 */
bool pca9535_init(void);

/**
 * @brief Check if device is present.
 */
bool pca9535_present(void);

/**
 * @brief Get current state snapshot.
 */
const PCA9535State* pca9535_get_state(void);

/**
 * @brief Read input registers and update state.
 * @return true on success
 */
bool pca9535_update(void);

/**
 * @brief Set a named control output.
 * @param ctrl  Control ID
 * @param on    true = enable, false = disable
 * @return true on success
 */
bool pca9535_set_control(PcaControl ctrl, bool on);

/**
 * @brief Get a named status input.
 * @param status  Status ID
 * @return true if signal is active
 */
bool pca9535_get_status(PcaStatus status);

/**
 * @brief Set raw output port value.
 * @param port  0 or 1
 * @param val   Output value (only output bits are applied)
 * @return true on success
 */
bool pca9535_set_port(uint8_t port, uint8_t val);

/**
 * @brief Read raw input port value.
 * @param port  0 or 1
 * @param val   Output: input register value
 * @return true on success
 */
bool pca9535_read_port(uint8_t port, uint8_t *val);

/**
 * @brief Set or clear a single output bit.
 * @param port  0 or 1
 * @param bit   Bit number (0-7)
 * @param val   true = set high, false = set low
 * @return true on success
 */
bool pca9535_set_bit(uint8_t port, uint8_t bit, bool val);

/**
 * @brief Get the control name string.
 */
const char* pca9535_control_name(PcaControl ctrl);

/**
 * @brief Get the status name string.
 */
const char* pca9535_status_name(PcaStatus status);

/**
 * @brief Install GPIO ISR on the PCA9535 INT pin.
 *        On interrupt, auto-reads input registers and runs fault detection.
 *        Call after pca9535_init() and GPIO ISR service install.
 */
void pca9535_install_isr(void);

/**
 * @brief Set fault behavior configuration.
 */
void pca9535_set_fault_config(const PcaFaultConfig *cfg);

/**
 * @brief Register a callback for fault events (e-fuse trips, PG changes).
 *        Called from ISR task context (safe for I2C, logging, BBP sends).
 *        Only one callback supported; subsequent calls replace previous.
 */
void pca9535_register_fault_callback(pca9535_fault_cb_t cb);

/**
 * @brief Check if any e-fuse fault is currently active.
 */
bool pca9535_any_fault_active(void);

#ifdef __cplusplus
}
#endif
