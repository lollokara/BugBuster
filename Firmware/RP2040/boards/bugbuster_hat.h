/*
 * BugBuster HAT — RP2040 board definition for pico-sdk
 *
 * Custom RP2040-based board. Flash and core config match the standard Pico;
 * pin assignments reflect the HAT PCB layout (see bb_config.h for details).
 */

// pico_cmake_set PICO_PLATFORM=rp2040

#ifndef _BOARDS_BUGBUSTER_HAT_H
#define _BOARDS_BUGBUSTER_HAT_H

// --- UART ---
#ifndef PICO_DEFAULT_UART
#define PICO_DEFAULT_UART 0
#endif
#ifndef PICO_DEFAULT_UART_TX_PIN
#define PICO_DEFAULT_UART_TX_PIN 0
#endif
#ifndef PICO_DEFAULT_UART_RX_PIN
#define PICO_DEFAULT_UART_RX_PIN 1
#endif

// --- LED ---
// GPIO25: activity LED (onboard)
// GPIO9:  status LED (HAT)
#ifndef PICO_DEFAULT_LED_PIN
#define PICO_DEFAULT_LED_PIN 25
#endif

// --- FLASH ---
#define PICO_BOOT_STAGE2_CHOOSE_W25Q080 1

#ifndef PICO_FLASH_SPI_CLKDIV
#define PICO_FLASH_SPI_CLKDIV 2
#endif

#ifndef PICO_FLASH_SIZE_BYTES
#define PICO_FLASH_SIZE_BYTES (2 * 1024 * 1024)
#endif

#ifndef PICO_RP2040_B0_SUPPORTED
#define PICO_RP2040_B0_SUPPORTED 1
#endif

#endif
