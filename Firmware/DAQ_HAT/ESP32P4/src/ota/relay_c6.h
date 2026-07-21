#pragma once

// =============================================================================
// relay_c6.h — push a staged image (see relay_stage.h) from P4 flash to the
// on-module ESP32-C6 over the UART ROM-bootloader path (c6_flasher.h), with
// bounded per-chunk retry and resume from the last acked offset across a P4
// reset (staging + pushed_bytes both survive in flash/NVS).
// =============================================================================

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define RELAY_C6_CHUNK_SIZE   256u   // matches c6_flasher's bootloader block size
#define RELAY_C6_MAX_RETRIES  5u

/**
 * @brief Drive the full (or resumed) push to the C6. Blocks until done/failed;
 *        call from a dedicated task, not from the s3_link callback context.
 */
esp_err_t relay_c6_push(void);

#ifdef __cplusplus
}
#endif
