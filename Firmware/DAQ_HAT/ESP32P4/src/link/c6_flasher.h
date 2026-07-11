#pragma once

// =============================================================================
// c6_flasher.h — flash the on-module ESP32-C6 from the P4 over UART2.
//
// The P4 drives the C6's BOOT (C6_BOOT_PIN) + RST (C6_RST_PIN) into ROM download
// mode and speaks the ESP serial bootloader protocol (espressif/esp-serial-
// flasher) to program a full firmware image the S3 mainboard streams down over
// the HAT-protocol OTA commands.
//
// The C6 link (ddp_master) MUST release UART2 before c6_flasher_begin() and
// reclaim it after c6_flasher_finish()/abort() — the board layer orchestrates
// this (it owns ddp_master). The flasher buffers the streamed chunks into
// bootloader-sized blocks; the host (S3) only needs to send image bytes in
// order, exactly like the P4 self-OTA path.
// =============================================================================

#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Configure C6 RST, BOOT and BOOT_EN pins as push-pull outputs via GPIO matrix.
// Call this after any library init that calls gpio_reset_pin on these pads.
void c6_gpio_init_output(void);

// Enter the C6 ROM bootloader via UART and begin a flash of @p image_size bytes
// at @p flash_offset. GPIO8 (BOOT_EN) must be driven LOW by the caller before
// this is called to select UART sub-mode (vs SDIO).
esp_err_t c6_flasher_begin(uint32_t image_size, uint32_t flash_offset);

// Enter the C6 ROM bootloader via SDIO and begin a flash of @p image_size bytes.
// Use this on hardware where the SDIO bus is wired (GPIO14-19 on P4).
// GPIO9 LOW at EN release → C6 SDIO download mode.
// Enter the C6 SDIO ROM bootloader and begin a flash of @p image_size bytes
// starting at @p flash_offset. Use offset=0 for full image, 0x10000 for app-only.
esp_err_t c6_flasher_begin_sdio(uint32_t image_size, uint32_t flash_offset);

// Stream the next image bytes (in order). Internally buffered into blocks.
esp_err_t c6_flasher_write(const uint8_t *data, size_t len);

// Flush the final block, verify (MD5), reset the C6 to run the new image, and
// release the serial port.
esp_err_t c6_flasher_finish(void);

// Abort: reset the C6 back to normal boot and release the serial port.
void c6_flasher_abort(void);

// Bytes written to C6 flash so far (resume/progress reporting).
uint32_t c6_flasher_received(void);

#ifdef __cplusplus
}
#endif
