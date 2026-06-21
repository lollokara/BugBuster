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

// Enter the C6 ROM bootloader, connect (with stub), and begin a flash of
// @p image_size bytes at offset 0 (full image: bootloader + partitions + app).
// @p image_size must be 4-byte aligned (esptool images are).
esp_err_t c6_flasher_begin(uint32_t image_size);

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
