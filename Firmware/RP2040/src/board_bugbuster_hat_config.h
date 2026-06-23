#ifndef _BOARD_BUGBUSTER_HAT_CONFIG_H
#define _BOARD_BUGBUSTER_HAT_CONFIG_H

// =============================================================================
// BugBuster HAT board configuration for debugprobe
// Defines SWD pin assignments matching the HAT PCB layout
// =============================================================================

// PIO mode — use probe.pio.h (bidirectional single SWDIO pin)
#define PROBE_IO_RAW

// PIO state machine 0 for SWD
#define PROBE_SM            0

// SWD pins — match bb_config.h definitions.
#define PROBE_PIN_SWCLK     18
#define PROBE_PIN_SWDIO     16
#define PROBE_PIN_SWDI      16

// No target reset pin is routed on the current HAT PCB. GPIO22 is the
// high-speed level-shifter DIR pin, so it must not be claimed by debugprobe.

// UART bridge pins (debugprobe CDC UART, NOT the BugBuster command bus)
// The current PCB assigns GPIO16/GPIO17 to SWDIO/TRACE, so the debugprobe CDC
// UART bridge is disabled in bb_main_integrated.c. These definitions remain so
// the vendored cdc_uart.c/autobaud.c sources still compile.
#define BB_DEBUGPROBE_CDC_UART_ENABLED 0
#define PROBE_UART_TX       16
#define PROBE_UART_RX       17
#define PROBE_UART_INTERFACE uart1
#define PROBE_UART_BAUDRATE 115200

// No discrete debugprobe LED is available; GPIO25 is VADJ4 enable.

// USB PID/VID — use debugprobe defaults or customize
// #define PROBE_USB_VID  0x2E8A
// #define PROBE_USB_PID  0x000C

#endif
