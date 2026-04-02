#ifndef _BOARD_BUGBUSTER_HAT_CONFIG_H
#define _BOARD_BUGBUSTER_HAT_CONFIG_H

// =============================================================================
// BugBuster HAT board configuration for debugprobe
// Defines SWD pin assignments matching the HAT PCB layout
// =============================================================================

// SWD pins — match bb_config.h definitions
// These override the default debugprobe pin assignments
// Breadboard test: SWD pins moved to avoid UART conflict on GPIO2/3
#define PROBE_PIN_SWCLK     18
#define PROBE_PIN_SWDIO     19
#define PROBE_PIN_SWDI      19

// Reset pin — directly connected to target nRST (active low, optional)
#define PROBE_PIN_RESET     22

// UART bridge pins (debugprobe CDC UART, NOT the BugBuster command bus)
// This UART bridges to the target's serial port via connector
#define PROBE_UART_TX       16
#define PROBE_UART_RX       17
#define PROBE_UART_INTERFACE uart0
// NOTE: We use uart0 for BugBuster commands on GPIO0/1.
// The target UART bridge should use uart1 on GPIO16/17 instead.
// This requires modifying debugprobe's cdc_uart.c to use uart1.

// LED pins
#define PROBE_PIN_LED       25      // Onboard LED (activity)

// USB PID/VID — use debugprobe defaults or customize
// #define PROBE_USB_VID  0x2E8A
// #define PROBE_USB_PID  0x000C

#endif
