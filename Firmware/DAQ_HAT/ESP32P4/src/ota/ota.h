#pragma once

// =============================================================================
// ota.h — robust OTA firmware update for the ESP32-P4 DAQ HAT.
//
// The ESP32-S3 mainboard is the network gateway: it pulls a new P4 image from
// the network and streams it to the P4 over the S3 HAT link (chunked). This
// module is transport-agnostic — it consumes raw firmware bytes and handles:
//
//   * A/B partition selection (esp_ota next slot).
//   * Streaming write with a running SHA-256 over the received image.
//   * Size + product-id + version guards (reject wrong-target / stale images).
//   * Final SHA-256 verification against the host-declared digest.
//   * Boot-partition switch + rollback safety: the new app boots PENDING_VERIFY
//     and must call ota_confirm() once healthy, else the bootloader reverts.
//
// Flow: ota_begin(meta) -> ota_write(chunk) * N -> ota_end() -> reboot
//       new image: ota_pending_verify()? -> self-test -> ota_confirm()/ota_rollback()
// =============================================================================

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Update metadata sent by the host before streaming the image.
typedef struct {
    uint32_t image_size;       // total firmware bytes
    uint32_t version_u32;      // FW_VERSION_U32 of the new image (informational)
    uint8_t  sha256[32];       // expected SHA-256 of the whole image
    char     product_id[16];   // must match FW_PRODUCT_ID
} ota_meta_t;

typedef enum {
    OTA_IDLE = 0,
    OTA_RECEIVING,
    OTA_READY,        // image fully written + verified, awaiting reboot
    OTA_ERROR,
} ota_state_t;

typedef struct {
    char        running_version[40];
    char        running_partition[16];
    bool        pending_verify;     // running image awaits confirmation
    ota_state_t state;
    uint32_t    received;           // bytes written so far
    uint32_t    image_size;
} ota_status_t;

/** @brief Initialise OTA: query running partition + rollback-pending state. */
esp_err_t ota_init(void);

/** @brief True if the currently-running app is in PENDING_VERIFY state. */
bool ota_pending_verify(void);

/** @brief Confirm the running image is healthy (cancels pending rollback). */
esp_err_t ota_confirm(void);

/** @brief Mark the running image invalid and reboot into the previous one. */
esp_err_t ota_rollback(void);

/**
 * @brief Begin an update: validate metadata, select the next OTA slot and open
 *        it for writing. Rejects wrong product-id / implausible size.
 */
esp_err_t ota_begin(const ota_meta_t *meta);

/**
 * @brief Write the next firmware chunk at @p offset (streamed straight to flash;
 *        no full-image staging anywhere). Chunks must arrive in order:
 *        @p offset must equal the number of bytes already written, otherwise
 *        ESP_ERR_INVALID_STATE is returned so the host can resume from
 *        ota_received() without re-sending the whole image.
 */
esp_err_t ota_write(uint32_t offset, const uint8_t *data, size_t len);

/** @brief Bytes written so far (resume point on a transient link error). */
uint32_t ota_received(void);

/**
 * @brief Finalise: verify total size + SHA-256, set the boot partition. After
 *        this returns ESP_OK the caller should reboot to run the new image.
 */
esp_err_t ota_end(void);

/** @brief Abort an in-progress update and release the OTA handle. */
void ota_abort(void);

/** @brief Fill a status snapshot (versions, partition, progress). */
void ota_get_status(ota_status_t *out);

/** @brief Current state. */
ota_state_t ota_state(void);

#ifdef __cplusplus
}
#endif
