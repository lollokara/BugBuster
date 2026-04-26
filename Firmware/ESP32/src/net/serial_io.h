#pragma once

// =============================================================================
// serial_io.h - Serial I/O abstraction over USB CDC (TinyUSB)
//
// Currently: single CDC port for CLI
// Future: composite USB device with multiple CDC ports for UART bridging
// =============================================================================

#include <stdint.h>
#include <stdarg.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize USB CDC serial (TinyUSB).
 *        Must be called before any serial_* functions.
 */
void serial_init(void);

/**
 * @brief Check if data is available to read.
 * @return Number of bytes available (0 if none)
 */
int serial_available(void);

/**
 * @brief Read one byte from serial input.
 * @return Byte read, or -1 if nothing available
 */
int serial_read(void);

/**
 * @brief Print a null-terminated string.
 */
void serial_print(const char* s);

/**
 * @brief Print a null-terminated string followed by CRLF.
 */
void serial_println(const char* s);

/**
 * @brief Printf-style formatted output.
 */
void serial_printf(const char* fmt, ...);

#ifdef __cplusplus
}
#endif
