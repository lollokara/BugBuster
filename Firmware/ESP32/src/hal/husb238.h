#pragma once

// =============================================================================
// husb238.h - HUSB238 USB PD Sink Controller Driver
//
// Reads negotiated PD contract status and source capabilities.
// The HUSB238 powers the board at 20V via USB-C PD even without I2C
// communication, so this driver is purely for monitoring/debug.
// =============================================================================

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// HUSB238 Register Addresses
#define HUSB238_REG_PD_STATUS0   0x00  // CC status
#define HUSB238_REG_PD_STATUS1   0x01  // PD contract info
#define HUSB238_REG_SRC_PDO_5V   0x02  // 5V PDO current
#define HUSB238_REG_SRC_PDO_9V   0x03  // 9V PDO current
#define HUSB238_REG_SRC_PDO_12V  0x04  // 12V PDO current
#define HUSB238_REG_SRC_PDO_15V  0x05  // 15V PDO current
#define HUSB238_REG_SRC_PDO_18V  0x06  // 18V PDO current
#define HUSB238_REG_SRC_PDO_20V  0x07  // 20V PDO current
#define HUSB238_REG_SRC_PDO      0x08  // Selected source PDO
#define HUSB238_REG_GO_COMMAND   0x09  // Trigger re-negotiation

// PD_STATUS1 bit fields
#define HUSB238_CC_DIR_MASK      0x80  // CC direction: 0=CC1, 1=CC2
#define HUSB238_ATTACH_MASK      0x40  // Attached: 1=attached
#define HUSB238_CC_STATUS_MASK   0x30  // CC status (00=no conn, 01=sink)
#define HUSB238_PD_STATUS_MASK   0x0E  // Current PD response
#define HUSB238_5V_CONTRACT      0x01  // 5V contract indicator

// PD_STATUS0 bit fields
#define HUSB238_VOLTAGE_MASK     0xF0  // Negotiated voltage
#define HUSB238_CURRENT_MASK     0x0F  // Negotiated current

// SRC_PDO register bit fields
#define HUSB238_PDO_DETECTED     0x80  // PDO detected
#define HUSB238_PDO_CURRENT_MASK 0x0F  // Max current for this PDO

// GO_COMMAND values
#define HUSB238_GO_SELECT_PDO    0x01  // Request selected PDO
#define HUSB238_GO_GET_SRC_CAP   0x04  // Get source capabilities
#define HUSB238_GO_HARD_RESET    0x10  // Hard reset

// Voltage codes in PD_STATUS1[7:4]
typedef enum {
    HUSB238_V_UNATTACHED = 0,
    HUSB238_V_5V  = 1,
    HUSB238_V_9V  = 2,
    HUSB238_V_12V = 3,
    HUSB238_V_15V = 4,
    HUSB238_V_18V = 5,
    HUSB238_V_20V = 6,
} Husb238Voltage;

// Current codes
typedef enum {
    HUSB238_I_0_5A  = 0,
    HUSB238_I_0_7A  = 1,
    HUSB238_I_1_0A  = 2,
    HUSB238_I_1_25A = 3,
    HUSB238_I_1_5A  = 4,
    HUSB238_I_1_75A = 5,
    HUSB238_I_2_0A  = 6,
    HUSB238_I_2_25A = 7,
    HUSB238_I_2_5A  = 8,
    HUSB238_I_2_75A = 9,
    HUSB238_I_3_0A  = 10,
    HUSB238_I_3_25A = 11,
    HUSB238_I_3_5A  = 12,
    HUSB238_I_4_0A  = 13,
    HUSB238_I_4_5A  = 14,
    HUSB238_I_5_0A  = 15,
} Husb238Current;

// Source PDO availability
typedef struct {
    bool detected;
    Husb238Current max_current;
} Husb238PdoInfo;

// Complete device state
typedef struct {
    bool present;

    // PD status
    bool     attached;
    bool     cc_direction;   // false=CC1, true=CC2
    uint8_t  pd_response;    // PD negotiation response
    bool     has_5v_contract;

    // Negotiated contract
    Husb238Voltage voltage;
    Husb238Current current;
    float    voltage_v;      // Decoded voltage in volts
    float    current_a;      // Decoded current in amps
    float    power_w;        // Computed power

    // Source PDOs
    Husb238PdoInfo pdo_5v;
    Husb238PdoInfo pdo_9v;
    Husb238PdoInfo pdo_12v;
    Husb238PdoInfo pdo_15v;
    Husb238PdoInfo pdo_18v;
    Husb238PdoInfo pdo_20v;

    // Selected PDO for request
    uint8_t  selected_pdo;
} Husb238State;

/**
 * @brief Initialize HUSB238 driver. Probes device and reads initial status.
 * @return true if device found
 */
bool husb238_init(void);

/**
 * @brief Check if device is present.
 */
bool husb238_present(void);

/**
 * @brief Get current state snapshot.
 */
const Husb238State* husb238_get_state(void);

/**
 * @brief Read all status registers and update internal state.
 * @return true on success
 */
bool husb238_update(void);

/**
 * @brief Request source capabilities (triggers GET_SRC_CAP).
 * @return true if command sent successfully
 */
bool husb238_get_src_cap(void);

/**
 * @brief Select a PDO voltage for next negotiation.
 * @param voltage  Target voltage (e.g., HUSB238_V_20V)
 * @return true on success
 */
bool husb238_select_pdo(Husb238Voltage voltage);

/**
 * @brief Trigger re-negotiation with selected PDO.
 * @return true if command sent
 */
bool husb238_go_command(uint8_t cmd);

/**
 * @brief Decode voltage enum to float volts.
 */
float husb238_decode_voltage(Husb238Voltage v);

/**
 * @brief Decode current enum to float amps.
 */
float husb238_decode_current(Husb238Current i);

#ifdef __cplusplus
}
#endif
