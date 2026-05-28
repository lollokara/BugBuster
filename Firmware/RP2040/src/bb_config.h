#pragma once

// =============================================================================
// bb_config.h — BugBuster HAT board pin definitions and constants
//
// Pin assignments for the BugBuster HAT expansion board (RP2040-based).
// PCB reference: Docs/HAT_RP2040_PCB_Reference.md
// =============================================================================

#include "hardware/uart.h"
#include "hardware/i2c.h"

// -----------------------------------------------------------------------------
// UART0 — BugBuster command bus (slave)
// -----------------------------------------------------------------------------
// UART0 for BugBuster command bus (stdio_uart disabled in bb_main_integrated.c)
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
// The 5-pin dedicated SWD connector exposes:
//   VADJ4  — target reference/supply
//   SWDIO  — bidirectional data
//   SWCLK  — clock from probe to target
//   TRACE  — SWO single-wire trace input
//   GND
#define BB_SWD_SWDIO_PIN    16      // GPIO16 — SWD data
#define BB_SWD_TRACE_PIN    17      // GPIO17 — SWO trace input
#define BB_SWD_SWCLK_PIN    18      // GPIO18 — SWD clock

// -----------------------------------------------------------------------------
// Power Management
// -----------------------------------------------------------------------------
#define BB_VADJ3_EN_PIN         23      // GPIO23 — VADJ3 enable
#define BB_3V3_ADJ_EN_PIN       24      // GPIO24 — adjustable 3V3 logic enable
#define BB_VADJ4_EN_PIN         25      // GPIO25 — VADJ4 enable

// Legacy two-connector API mapping retained until the HAT protocol grows named
// VADJ rails: connector 0 = VADJ3, connector 1 = VADJ4.
#define BB_EN_A_PIN             BB_VADJ3_EN_PIN
#define BB_EN_B_PIN             BB_VADJ4_EN_PIN

#define BB_CURRENT_A_ADC        26      // GPIO26 (ADC0) — VADJ3 LTM8083 ISMON
#define BB_CURRENT_B_ADC        27      // GPIO27 (ADC1) — VADJ4 LTM8083 ISMON
#define BB_VADJ3_SENSE_ADC      28      // GPIO28 (ADC2) — VADJ3 110k/10k divider
#define BB_VADJ4_SENSE_ADC      29      // GPIO29 (ADC3) — VADJ4 110k/10k divider

// LTM8083 ISMON: VISMON = 10 * (I * RCS) + 250mV, RCS = 50mOhm.
#define BB_CURRENT_SHUNT_MOHM   50
#define BB_CURRENT_ADC_VREF     3300    // ADC reference in mV (3.3V)
#define BB_LTM8083_ISMON_OFFSET_MV 250
#define BB_LTM8083_ISMON_GAIN   10

// Sense divider: 110k top, 10k bottom => Vrail = Vadc * 12.
#define BB_VADJ_SENSE_RTOP_OHM  110000
#define BB_VADJ_SENSE_RBOT_OHM  10000

// High-speed level shifter shared by IO10..15 and IO20/21.
#define BB_LEVEL_SHIFT_OE_PIN   19      // GPIO19 — output enable
#define BB_LEVEL_SHIFT_DIR_PIN  22      // GPIO22 — shared direction

// -----------------------------------------------------------------------------
// HAT I2C — DS4424 adjustable rail trim
// -----------------------------------------------------------------------------
#define BB_HVPAK_I2C        i2c1
#define BB_HVPAK_SDA_PIN    6       // GPIO6
#define BB_HVPAK_SCL_PIN    7       // GPIO7
#define BB_HVPAK_I2C_FREQ   400000  // 400 kHz
#define BB_HVPAK_PRESENT    0       // GreenPAK/HVPAK is not populated on this PCB
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
// LA-done IRQ — not available as a dedicated pin on this PCB
// -----------------------------------------------------------------------------
// GPIO8 is the shared open-drain HAT INT line. bb_la.c must not drive the old
// push-pull LA_DONE pulse because GPIO28/GPIO29 are now analog rail senses.
#define BB_LA_DONE_PIN      (-1)

// -----------------------------------------------------------------------------
// EXP_EXT — Expansion I/O lines routed through the high-speed level shifter
// -----------------------------------------------------------------------------
#define BB_EXT1_PIN         10      // GPIO10 — Conn1 pin 2
#define BB_EXT2_PIN         12      // GPIO12 — Conn1 pin 3
#define BB_EXT3_PIN         11      // GPIO11 — Conn1 pin 4
#define BB_EXT4_PIN         21      // GPIO21 — Conn2 pin 1
#define BB_NUM_EXT_PINS     4

#define BB_HS_IO0_PIN       10
#define BB_HS_IO1_PIN       11
#define BB_HS_IO2_PIN       12
#define BB_HS_IO3_PIN       13
#define BB_HS_IO4_PIN       14
#define BB_HS_IO5_PIN       15
#define BB_HS_IO6_PIN       20
#define BB_HS_IO7_PIN       21
#define BB_NUM_HS_IO_PINS   8

// -----------------------------------------------------------------------------
// Logic Analyzer — PIO 1 low-speed capture inputs routed to ESP32 EXT muxes
// -----------------------------------------------------------------------------
#define BB_LA_CH0_PIN       2       // GPIO2 — LA channel 0
#define BB_LA_CH1_PIN       3       // GPIO3 — LA channel 1
#define BB_LA_CH2_PIN       4       // GPIO4 — LA channel 2
#define BB_LA_CH3_PIN       5       // GPIO5 — LA channel 3
#define BB_LA_NUM_CHANNELS  4
#define BB_LA_BUFFER_SIZE   (76 * 1024)   // 76KB SRAM capture buffer (reduced for RAM fit)

// -----------------------------------------------------------------------------
// LEDs
// -----------------------------------------------------------------------------
#define BB_WS2812_PIN       9       // GPIO9 — 8x WS2812B status LEDs
#define BB_WS2812_COUNT     8
#define BB_LED_STATUS_PIN   BB_WS2812_PIN

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
#define HAT_CMD_GET_CAPS        0x06

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
#define HAT_CMD_LA_LOG_ENABLE   0x39  // Enable/disable log relay to host
#define HAT_CMD_LA_USB_RESET    0x3A  // Reinitialize vendor bulk endpoint
#define HAT_CMD_LA_SET_ROUTE    0x3B  // Select low-speed/high-speed LA route

// HAT v2 supplies, LEDs, and shifted IO controls
#define HAT_CMD_GET_RAIL_STATUS 0x40
#define HAT_CMD_SET_RAIL_ENABLE 0x41
#define HAT_CMD_SET_LED_STATE   0x42
#define HAT_CMD_CALIBRATE_START 0x43
#define HAT_CMD_CALIBRATE_STATUS 0x44
#define HAT_CMD_CALIBRATE_IMPORT 0x45
#define HAT_CMD_SET_IO_BANK     0x46
#define HAT_CMD_SET_LEVEL_SHIFT 0x47
#define HAT_CMD_SET_RAIL_VOLTAGE 0x48

// Responses
#define HAT_RSP_OK              0x80
#define HAT_RSP_ERROR           0x81
#define HAT_RSP_INFO            0x82
#define HAT_RSP_POWER_STATUS    0x83
#define HAT_RSP_DAP_STATUS      0x84
#define HAT_RSP_LA_STATUS       0x85
#define HAT_RSP_LA_DATA         0x86
#define HAT_RSP_CAPS            0x87
#define HAT_RSP_RAIL_STATUS     0x88
#define HAT_RSP_LA_LOG          0x89  // Log message relay from RP2040
#define HAT_RSP_CALIBRATE_STATUS 0x8A


// Error codes
#define HAT_ERR_INVALID_CMD     0x01
#define HAT_ERR_INVALID_PIN     0x02
#define HAT_ERR_INVALID_FUNC    0x03
#define HAT_ERR_BUSY            0x04
#define HAT_ERR_CRC             0x05
#define HAT_ERR_FRAME           0x06
#define HAT_ERR_NOT_CONNECTED   0x07
#define HAT_ERR_POWER_FAULT     0x08
#define HAT_ERR_UNSUPPORTED     0x12

// HAT type codes
#define HAT_TYPE_NONE           0x00
#define HAT_TYPE_SWD_GPIO       0x01

// HAT v2 capability flags
#define HAT_CAP_RAILS             (1u << 0)
#define HAT_CAP_LEDS              (1u << 1)
#define HAT_CAP_LA_LOW_SPEED      (1u << 2)
#define HAT_CAP_LA_HIGH_SPEED     (1u << 3)
#define HAT_CAP_SHIFTED_IO        (1u << 4)
#define HAT_CAP_HVPAK_UNSUPPORTED (1u << 5)

// HAT v2 rail IDs
#define HAT_RAIL_3V3_ADJ        0
#define HAT_RAIL_VADJ3          1
#define HAT_RAIL_VADJ4          2
#define HAT_RAIL_COUNT          3

// HAT v2 LA route IDs
#define HAT_LA_ROUTE_LOW_SPEED  0
#define HAT_LA_ROUTE_HIGH_SPEED 1

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
