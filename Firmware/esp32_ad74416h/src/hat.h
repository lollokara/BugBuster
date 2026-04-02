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
#if BREADBOARD_MODE
// Breadboard test: UART0 on GPIO43/44 (still free in breadboard mode)
// HAT detect on GPIO47 (reused from DIO, acceptable for testing)
// IRQ not connected in breadboard test
#define PIN_HAT_DETECT      GPIO_NUM_NC   // No detect in breadboard (always assume connected)
#define PIN_HAT_TX          GPIO_NUM_43   // UART TX to RP2040 GPIO2
#define PIN_HAT_RX          GPIO_NUM_44   // UART RX from RP2040 GPIO3
#define PIN_HAT_IRQ         GPIO_NUM_NC   // No IRQ in breadboard test
#else
#define PIN_HAT_DETECT      GPIO_NUM_47   // ADC input for HAT identification
#define PIN_HAT_TX          GPIO_NUM_43   // UART TX to HAT
#define PIN_HAT_RX          GPIO_NUM_44   // UART RX from HAT
#define PIN_HAT_IRQ         GPIO_NUM_15   // Shared open-drain interrupt
#endif

#define HAT_UART_NUM        UART_NUM_0
#define HAT_UART_BAUD       921600
#define HAT_UART_BUF_SIZE   512

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

// Commands (master → slave): Core (0x01–0x0F)
#define HAT_CMD_PING            0x01
#define HAT_CMD_GET_INFO        0x02
#define HAT_CMD_SET_PIN_CONFIG  0x03
#define HAT_CMD_GET_PIN_CONFIG  0x04
#define HAT_CMD_RESET           0x05

// Commands: Power Management (0x10–0x1F)
#define HAT_CMD_SET_POWER       0x10  // Enable/disable connector power
#define HAT_CMD_GET_POWER_STATUS 0x11 // Read power state + current
#define HAT_CMD_SET_IO_VOLTAGE  0x12  // Set HVPAK I/O level (mV)
#define HAT_CMD_GET_IO_VOLTAGE  0x13  // Read current I/O voltage setting

// Commands: SWD Management (0x20–0x2F)
#define HAT_CMD_GET_DAP_STATUS  0x20  // Is debugprobe USB connected? Target detected?
#define HAT_CMD_GET_TARGET_INFO 0x21  // DPIDR, SWD clock
#define HAT_CMD_SET_SWD_CLOCK   0x22  // Adjust SWD clock speed

// Commands: Logic Analyzer (0x30–0x3F)
#define HAT_CMD_LA_CONFIG       0x30  // Configure capture
#define HAT_CMD_LA_SET_TRIGGER  0x31  // Set trigger condition
#define HAT_CMD_LA_ARM          0x32  // Arm trigger
#define HAT_CMD_LA_FORCE        0x33  // Force immediate capture
#define HAT_CMD_LA_GET_STATUS   0x34  // Capture state + sample count
#define HAT_CMD_LA_READ_DATA    0x35  // Read captured data chunk
#define HAT_CMD_LA_STOP         0x36  // Abort capture

// Responses (slave → master)
#define HAT_RSP_OK              0x80
#define HAT_RSP_ERROR           0x81
#define HAT_RSP_INFO            0x82
#define HAT_RSP_POWER_STATUS    0x83
#define HAT_RSP_DAP_STATUS      0x84
#define HAT_RSP_LA_STATUS       0x85
#define HAT_RSP_LA_DATA         0x86

// Error codes
#define HAT_ERR_INVALID_CMD     0x01
#define HAT_ERR_INVALID_PIN     0x02
#define HAT_ERR_INVALID_FUNC    0x03
#define HAT_ERR_BUSY            0x04
#define HAT_ERR_CRC             0x05
#define HAT_ERR_FRAME           0x06
#define HAT_ERR_NOT_CONNECTED   0x07
#define HAT_ERR_POWER_FAULT     0x08

// -----------------------------------------------------------------------------
// Connector / Power Types
// -----------------------------------------------------------------------------

typedef enum {
    HAT_CONNECTOR_A = 0,    // Target 1: powered by VADJ1
    HAT_CONNECTOR_B = 1,    // Target 2: powered by VADJ2
} HatConnector;

typedef struct {
    bool     enabled;           // Connector power is on
    float    current_ma;        // Measured current (if shunt present)
    bool     fault;             // Overcurrent fault detected
} HatConnectorStatus;

// -----------------------------------------------------------------------------
// HAT State
// -----------------------------------------------------------------------------

typedef struct {
    // Detection & connection
    bool         detected;                          // HAT physically present (ADC detect)
    bool         connected;                         // UART communication established
    HatType      type;                              // Detected HAT type
    float        detect_voltage;                    // Raw ADC voltage on detect pin
    uint8_t      fw_version_major;                  // HAT firmware version
    uint8_t      fw_version_minor;

    // Pin configuration
    HatPinFunction pin_config[HAT_NUM_EXT_PINS];   // Current EXP_EXT assignments
    bool         config_confirmed;                  // HAT acknowledged last config

    // Power management
    HatConnectorStatus connector[2];                // Connector A and B status
    uint16_t     io_voltage_mv;                     // HVPAK I/O voltage (mV)

    // SWD management
    bool         dap_connected;                     // USB CMSIS-DAP host connected
    bool         target_detected;                   // SWD target responding
    uint32_t     target_dpidr;                      // Target DPIDR value

    // Timing
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

// --- Power Management ---

/**
 * @brief Enable or disable a target connector's power.
 * @param conn  HAT_CONNECTOR_A or HAT_CONNECTOR_B
 * @param on    true = enable, false = disable
 * @return true if HAT acknowledged
 */
bool hat_set_power(HatConnector conn, bool on);

/**
 * @brief Get power status for both connectors.
 * @return true if HAT responded
 */
bool hat_get_power_status(void);

/**
 * @brief Set the HVPAK I/O level translation voltage.
 * @param mv  Target I/O voltage in millivolts (1200–5500)
 * @return true if HAT acknowledged
 */
bool hat_set_io_voltage(uint16_t mv);

/**
 * @brief One-call SWD setup: set VADJ, I/O voltage, power on, route SWD pins.
 * @param target_voltage_mv  Target voltage in mV (e.g. 3300 for 3.3V)
 * @param connector          Which connector the target is on
 * @return true if all steps succeeded
 */
bool hat_setup_swd(uint16_t target_voltage_mv, HatConnector connector);

// --- SWD Management ---

/**
 * @brief Query DAP/SWD status from the HAT (USB connection, target detect, clock).
 * @return true if HAT responded
 */
bool hat_get_dap_status(void);

/**
 * @brief Set SWD clock speed on the HAT debugprobe.
 * @param khz  Clock speed in kHz (100–50000)
 * @return true if HAT acknowledged
 */
bool hat_set_swd_clock(uint16_t khz);

// --- Logic Analyzer ---

typedef struct {
    uint8_t  state;         // LaState enum
    uint8_t  channels;
    uint32_t samples_captured;
    uint32_t total_samples;
    uint32_t actual_rate_hz;
} HatLaStatus;

/**
 * @brief Configure LA capture parameters.
 * @param channels   1, 2, or 4
 * @param rate_hz    Sample rate in Hz
 * @param depth      Total samples to capture
 */
bool hat_la_configure(uint8_t channels, uint32_t rate_hz, uint32_t depth);

/**
 * @brief Set LA trigger condition.
 * @param type     Trigger type (0=none, 1=rising, 2=falling, 3=both, 4=high, 5=low)
 * @param channel  Channel to trigger on (0-3)
 */
bool hat_la_set_trigger(uint8_t type, uint8_t channel);

bool hat_la_arm(void);
bool hat_la_force(void);
bool hat_la_stop(void);
bool hat_la_get_status(HatLaStatus *status);

/**
 * @brief Read a chunk of captured data from the LA buffer.
 * @param offset  Byte offset into buffer
 * @param buf     Output buffer
 * @param len     Bytes to read (max 28)
 * @return Actual bytes read
 */
uint8_t hat_la_read_data(uint32_t offset, uint8_t *buf, uint8_t len);

// --- String Helpers ---

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
