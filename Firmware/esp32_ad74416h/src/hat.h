#pragma once

// =============================================================================
// hat.h - HAT (Hardware Attached on Top) Expansion Board Driver
//
// Manages detection, communication, and configuration of HAT expansion boards
// connected via the HAT header on the BugBuster PCB.
//
// Physical interface:
//   GPIO47 (ADC)        - HAT detect: voltage divider identifies HAT type
//                         BugBuster: 10kΩ pull-up to 3.3V
//                         HAT board: pull-down resistor (value identifies type)
//   GPIO43 (TXD0)       - UART TX to HAT (BugBuster is master)
//   GPIO44 (RXD0)       - UART RX from HAT
//   GPIO15 (open-drain) - Shared interrupt line (bidirectional)
//
// HAT connector signals:
//   EXP_EXT_1..4        - 4 configurable I/O lines routed through HAT
//                         Each can be independently assigned to a function
//
// PCB mode only — not available in breadboard mode.
// =============================================================================

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// -----------------------------------------------------------------------------
// Pin Definitions (PCB mode only)
// -----------------------------------------------------------------------------
#define PIN_HAT_DETECT      GPIO_NUM_47   // ADC input for HAT identification
#define PIN_HAT_TX          GPIO_NUM_43   // UART TX to HAT
#define PIN_HAT_RX          GPIO_NUM_44   // UART RX from HAT
#define PIN_HAT_IRQ         GPIO_NUM_15   // Shared open-drain interrupt

#define HAT_UART_NUM        UART_NUM_0
#define HAT_UART_BAUD       115200
#define HAT_UART_BUF_SIZE   256

// -----------------------------------------------------------------------------
// HAT Detection — Voltage Thresholds
// -----------------------------------------------------------------------------
// No HAT:     pull-up only → ~3.3V  (ADC > 2.5V)
// SWD/GPIO:   10k down → ~1.65V    (ADC 1.2V–2.1V)
// Future HATs: different pull-down → different voltage bands

typedef enum {
    HAT_TYPE_NONE = 0,      // No HAT detected (~3.3V)
    HAT_TYPE_SWD_GPIO,      // SWD/GPIO HAT: 10kΩ pull-down (~1.65V)
    // Future HAT types:
    // HAT_TYPE_ANALOG,     // e.g. 4.7kΩ pull-down (~1.06V)
    // HAT_TYPE_PROTOCOL,   // e.g. 22kΩ pull-down (~2.27V)
    HAT_TYPE_UNKNOWN = 0xFF,
} HatType;

// -----------------------------------------------------------------------------
// EXP_EXT Pin Functions
// -----------------------------------------------------------------------------

typedef enum {
    HAT_FUNC_DISCONNECTED = 0,  // Pin not assigned
    HAT_FUNC_SWDIO,             // SWD data I/O
    HAT_FUNC_SWCLK,             // SWD clock
    HAT_FUNC_TRACE1,            // Trace data 1 (SWO)
    HAT_FUNC_TRACE2,            // Trace data 2
    HAT_FUNC_GPIO1,             // General-purpose I/O 1
    HAT_FUNC_GPIO2,             // General-purpose I/O 2
    HAT_FUNC_GPIO3,             // General-purpose I/O 3
    HAT_FUNC_GPIO4,             // General-purpose I/O 4
    HAT_FUNC_COUNT,
} HatPinFunction;

#define HAT_NUM_EXT_PINS    4   // EXP_EXT_1 through EXP_EXT_4

// -----------------------------------------------------------------------------
// HAT UART Protocol — Command/Response Framing
// -----------------------------------------------------------------------------
// Frame: [SYNC(0xAA)] [LEN(1)] [CMD(1)] [PAYLOAD(0..N)] [CRC8(1)]
// SYNC = 0xAA, LEN = payload length (excluding SYNC, LEN, CMD, CRC)
// CRC-8 polynomial 0x07 over CMD + PAYLOAD bytes
//
// BugBuster (master) sends commands, HAT (slave) sends responses.
// HAT can also assert IRQ to signal unsolicited status change.

#define HAT_FRAME_SYNC      0xAA
#define HAT_FRAME_MAX_LEN   32      // Max payload length

// Commands (master → slave)
#define HAT_CMD_PING            0x01  // Ping / identify
#define HAT_CMD_GET_INFO        0x02  // Get HAT info (type, version, capabilities)
#define HAT_CMD_SET_PIN_CONFIG  0x03  // Set EXP_EXT pin function mapping
#define HAT_CMD_GET_PIN_CONFIG  0x04  // Get current pin config
#define HAT_CMD_RESET           0x05  // Reset HAT to default state

// Responses (slave → master)
#define HAT_RSP_OK              0x80  // Success (echoes CMD in payload[0])
#define HAT_RSP_ERROR           0x81  // Error (error code in payload)
#define HAT_RSP_INFO            0x82  // Info response (to GET_INFO)

// Error codes
#define HAT_ERR_INVALID_CMD     0x01
#define HAT_ERR_INVALID_PIN     0x02
#define HAT_ERR_INVALID_FUNC    0x03
#define HAT_ERR_BUSY            0x04

// -----------------------------------------------------------------------------
// HAT State
// -----------------------------------------------------------------------------

typedef struct {
    bool         detected;                          // HAT physically present (ADC detect)
    bool         connected;                         // UART communication established
    HatType      type;                              // Detected HAT type
    float        detect_voltage;                    // Raw ADC voltage on detect pin
    uint8_t      fw_version_major;                  // HAT firmware version
    uint8_t      fw_version_minor;
    HatPinFunction pin_config[HAT_NUM_EXT_PINS];   // Current EXP_EXT assignments
    bool         config_confirmed;                  // HAT acknowledged last config
    uint32_t     last_ping_ms;                      // Last successful ping timestamp
} HatState;

// -----------------------------------------------------------------------------
// Public API
// -----------------------------------------------------------------------------

/**
 * @brief Initialize HAT subsystem. Sets up ADC detect, UART, and IRQ pin.
 *        PCB mode only — returns false and does nothing in breadboard mode.
 */
bool hat_init(void);

/**
 * @brief Check if a HAT is physically present (based on ADC detect).
 */
bool hat_detected(void);

/**
 * @brief Get current HAT state snapshot.
 */
const HatState* hat_get_state(void);

/**
 * @brief Read the detect pin ADC and identify the HAT type.
 *        Called periodically or on-demand.
 * @return Detected HAT type
 */
HatType hat_detect(void);

/**
 * @brief Attempt to establish UART communication with the HAT.
 *        Sends PING, waits for response.
 * @return true if HAT responded
 */
bool hat_connect(void);

/**
 * @brief Set the function of a single EXP_EXT pin.
 * @param ext_pin  Pin index 0-3 (EXP_EXT_1 to EXP_EXT_4)
 * @param func     Desired function
 * @return true if HAT acknowledged the change
 */
bool hat_set_pin(uint8_t ext_pin, HatPinFunction func);

/**
 * @brief Set all 4 EXP_EXT pin functions at once.
 * @param config   Array of 4 HatPinFunction values
 * @return true if HAT acknowledged
 */
bool hat_set_all_pins(const HatPinFunction config[HAT_NUM_EXT_PINS]);

/**
 * @brief Get the current pin config from the HAT (queries via UART).
 * @param config   Output: array of 4 HatPinFunction values
 * @return true if HAT responded
 */
bool hat_get_pin_config(HatPinFunction config[HAT_NUM_EXT_PINS]);

/**
 * @brief Reset HAT to default state (all pins disconnected).
 * @return true if HAT acknowledged
 */
bool hat_reset(void);

/**
 * @brief Get function name string for display.
 */
const char* hat_func_name(HatPinFunction func);

/**
 * @brief Get HAT type name string for display.
 */
const char* hat_type_name(HatType type);

#ifdef __cplusplus
}
#endif
