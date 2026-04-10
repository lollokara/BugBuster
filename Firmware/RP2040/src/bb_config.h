#pragma once

// =============================================================================
// bb_config.h — BugBuster HAT board pin definitions and constants
//
// Pin assignments for the BugBuster HAT expansion board (RP2040-based).
// These are PRELIMINARY — finalize with actual HAT PCB layout.
// =============================================================================

#include "hardware/uart.h"
#include "hardware/i2c.h"

// -----------------------------------------------------------------------------
// UART0 — BugBuster command bus (slave)
// -----------------------------------------------------------------------------
// UART0 for BugBuster command bus (stdio_uart disabled in bb_main_integrated.c)
// UART1 is used by debugprobe CDC UART bridge (GPIO4/5)
#define BB_UART             uart0
#define BB_UART_BAUD        921600
#define BB_UART_TX_PIN      0       // GPIO0 → ESP32 RX (GPIO44)
#define BB_UART_RX_PIN      1       // GPIO1 ← ESP32 TX (GPIO43)
#define BB_UART_BUF_SIZE    512

// -----------------------------------------------------------------------------
// SWD — debugprobe pins (PIO 0)
// These must match the debugprobe board config
// -----------------------------------------------------------------------------
// SWD pins — managed by debugprobe (PIO 0), defined here for reference.
// The 3-pin dedicated SWD connector (new PCB, 2026-04-09) exposes:
//   SWDIO  — bidirectional data
//   SWCLK  — clock from probe to target
//   TRACE  — SWO single-wire trace input
// Physical pins are finalized on the HAT board; the override is in
// board_bugbuster_hat_config.h (SWCLK/SWDIO) and here (TRACE).
#define BB_SWD_SWCLK_PIN    2       // GPIO2 — SWD clock (debugprobe default for Pico)
#define BB_SWD_SWDIO_PIN    3       // GPIO3 — SWD data (debugprobe default for Pico)
#define BB_SWD_TRACE_PIN    29      // GPIO29 — SWO trace input (new dedicated SWD connector)
                                    // TODO(user): confirm GPIO29 matches the final HAT PCB;
                                    // GPIO23/24/29 were free per the 2026-04-09 pin audit.

// -----------------------------------------------------------------------------
// Power Management
// -----------------------------------------------------------------------------
#define BB_EN_A_PIN         4       // GPIO4 — Connector A power enable
#define BB_EN_B_PIN         5       // GPIO5 — Connector B power enable
#define BB_FAULT_A_PIN      20      // GPIO20 — Connector A overcurrent (input, active low)
#define BB_FAULT_B_PIN      21      // GPIO21 — Connector B overcurrent (input, active low)
#define BB_CURRENT_A_ADC    26      // GPIO26 (ADC0) — Connector A current sense
#define BB_CURRENT_B_ADC    27      // GPIO27 (ADC1) — Connector B current sense

// Current sense: V_shunt = I * R_shunt, ADC reads V_shunt
// Typical R_shunt = 0.1 Ohm, so 100mA = 10mV, 1A = 100mV
#define BB_CURRENT_SHUNT_MOHM   100     // Shunt resistance in milliohms
#define BB_CURRENT_ADC_VREF     3300    // ADC reference in mV (3.3V)

// -----------------------------------------------------------------------------
// HVPAK — Renesas level translator (I2C control)
// -----------------------------------------------------------------------------
#define BB_HVPAK_I2C        i2c1
#define BB_HVPAK_SDA_PIN    6       // GPIO6
#define BB_HVPAK_SCL_PIN    7       // GPIO7
#define BB_HVPAK_I2C_FREQ   400000  // 400 kHz
#define BB_HVPAK_I2C_ADDR   0x48    // Documented HVPAK mailbox address
#define BB_HVPAK_I2C_TIMEOUT_US 5000
#define BB_HVPAK_IDENTITY_REG 0x48  // Read-only identity byte (virtual output mailbox)
#define BB_HVPAK_COMMAND_REG  0x4C  // Writable command byte (virtual input mailbox)
#define BB_HVPAK_ID_SLG47104   0x04
#define BB_HVPAK_ID_SLG47115_E 0x15

// HVPAK voltage range
#define BB_HVPAK_MIN_MV     1200    // 1.2V minimum
#define BB_HVPAK_MAX_MV     5500    // 5.5V maximum
#define BB_HVPAK_DEFAULT_MV 3300    // Default 3.3V

// HVPAK-specific HAT errors (wire protocol is not released yet, so extending the
// error space is acceptable as long as it is documented alongside the change).
#define HAT_ERR_HVPAK_NO_DEVICE        0x09
#define HAT_ERR_HVPAK_TIMEOUT          0x0A
#define HAT_ERR_HVPAK_UNKNOWN_IDENTITY 0x0B
#define HAT_ERR_HVPAK_UNSUPPORTED_VOLT 0x0C
#define HAT_ERR_HVPAK_WRITE_FAILED     0x0D
#define HAT_ERR_HVPAK_INVALID_INDEX    0x0E
#define HAT_ERR_HVPAK_UNSUPPORTED_CAP  0x0F
#define HAT_ERR_HVPAK_INVALID_ARG      0x10
#define HAT_ERR_HVPAK_UNSAFE_REG       0x11

// -----------------------------------------------------------------------------
// IRQ — Shared interrupt line with BugBuster
// -----------------------------------------------------------------------------
#define BB_IRQ_PIN          8       // GPIO8 — Open-drain, active low

// -----------------------------------------------------------------------------
// LA-done IRQ — dedicated capture-complete signal to ESP32
// -----------------------------------------------------------------------------
// Pulsed low for ~2 µs by bb_la.c whenever the LA transitions to LA_STATE_DONE,
// so the ESP32 can stop polling hat_la_get_status() over the UART bridge.
// Wired to ESP32-S3 GPIO18 (PIN_HAT_LA_DONE_IRQ in Firmware/esp32_ad74416h/src/hat.h).
// Push-pull output on this side (only the RP2040 drives this line); idle high.
#define BB_LA_DONE_PIN      28      // GPIO28 — active-low LA-done pulse to ESP32

// -----------------------------------------------------------------------------
// EXP_EXT — Expansion I/O lines (routed through HVPAK level translation)
// -----------------------------------------------------------------------------
#define BB_EXT1_PIN         10      // GPIO10 — EXP_EXT_1
#define BB_EXT2_PIN         11      // GPIO11 — EXP_EXT_2
#define BB_EXT3_PIN         12      // GPIO12 — EXP_EXT_3
#define BB_EXT4_PIN         13      // GPIO13 — EXP_EXT_4
#define BB_NUM_EXT_PINS     4

// -----------------------------------------------------------------------------
// Logic Analyzer — PIO 1 capture inputs (Phase 3)
// These can overlap with EXT pins if LA captures the same signals
// -----------------------------------------------------------------------------
#define BB_LA_CH0_PIN       14      // GPIO14 — LA channel 0
#define BB_LA_CH1_PIN       15      // GPIO15 — LA channel 1
#define BB_LA_CH2_PIN       16      // GPIO16 — LA channel 2
#define BB_LA_CH3_PIN       17      // GPIO17 — LA channel 3
#define BB_LA_NUM_CHANNELS  4
#define BB_LA_BUFFER_SIZE   (76 * 1024)   // 76KB SRAM capture buffer (reduced for RAM fit)

// -----------------------------------------------------------------------------
// LEDs
// -----------------------------------------------------------------------------
#define BB_LED_STATUS_PIN   9       // GPIO9 — Status LED
#define BB_LED_ACTIVITY_PIN 25      // GPIO25 — RP2040 onboard LED (if present)

// -----------------------------------------------------------------------------
// HAT Protocol Constants (shared with ESP32 side)
// These MUST match the defines in the ESP32 hat.h
// -----------------------------------------------------------------------------
#define HAT_FRAME_SYNC      0xAA
#define HAT_FRAME_MAX_LEN   32

// Core commands
#define HAT_CMD_PING            0x01
#define HAT_CMD_GET_INFO        0x02
#define HAT_CMD_SET_PIN_CONFIG  0x03
#define HAT_CMD_GET_PIN_CONFIG  0x04
#define HAT_CMD_RESET           0x05

// Power commands
#define HAT_CMD_SET_POWER       0x10
#define HAT_CMD_GET_POWER_STATUS 0x11
#define HAT_CMD_SET_IO_VOLTAGE  0x12
#define HAT_CMD_GET_IO_VOLTAGE  0x13
#define HAT_CMD_GET_HVPAK_INFO   0x14
#define HAT_CMD_GET_HVPAK_CAPS   0x15
#define HAT_CMD_GET_HVPAK_LUT    0x16
#define HAT_CMD_SET_HVPAK_LUT    0x17
#define HAT_CMD_GET_HVPAK_BRIDGE 0x18
#define HAT_CMD_SET_HVPAK_BRIDGE 0x19
#define HAT_CMD_GET_HVPAK_ANALOG 0x1A
#define HAT_CMD_SET_HVPAK_ANALOG 0x1B
#define HAT_CMD_GET_HVPAK_PWM    0x1C
#define HAT_CMD_SET_HVPAK_PWM    0x1D
#define HAT_CMD_HVPAK_REG_READ   0x1E
#define HAT_CMD_HVPAK_REG_WRITE_MASKED 0x1F

// SWD management
#define HAT_CMD_GET_DAP_STATUS  0x20
#define HAT_CMD_GET_TARGET_INFO 0x21
#define HAT_CMD_SET_SWD_CLOCK   0x22

// Logic analyzer
#define HAT_CMD_LA_CONFIG       0x30
#define HAT_CMD_LA_SET_TRIGGER  0x31
#define HAT_CMD_LA_ARM          0x32
#define HAT_CMD_LA_FORCE        0x33
#define HAT_CMD_LA_GET_STATUS   0x34
#define HAT_CMD_LA_READ_DATA    0x35
#define HAT_CMD_LA_STOP         0x36
#define HAT_CMD_LA_STREAM_START 0x37
#define HAT_CMD_LA_USB_SEND     0x38  // Send capture buffer via USB bulk

// Responses
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

// HAT type codes
#define HAT_TYPE_NONE           0x00
#define HAT_TYPE_SWD_GPIO       0x01

// Pin function codes
#define HAT_FUNC_DISCONNECTED   0x00
// Slots 0x01..0x04 are RESERVED for wire-protocol compatibility.
// Formerly SWDIO, SWCLK, TRACE1, TRACE2 — removed when SWD moved to its
// dedicated 3-pin connector (2026-04-09). bb_pins_set() rejects these.
#define HAT_FUNC_RESERVED_1     0x01   // formerly SWDIO
#define HAT_FUNC_RESERVED_2     0x02   // formerly SWCLK
#define HAT_FUNC_RESERVED_3     0x03   // formerly TRACE1
#define HAT_FUNC_RESERVED_4     0x04   // formerly TRACE2
#define HAT_FUNC_GPIO1          0x05
#define HAT_FUNC_GPIO2          0x06
#define HAT_FUNC_GPIO3          0x07
#define HAT_FUNC_GPIO4          0x08
