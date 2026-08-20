// =============================================================================
// ota.c — robust OTA firmware update for the ESP32-P4 DAQ HAT.
// =============================================================================

#include "ota.h"
#include <string.h>
#include "esp_ota_ops.h"
#include "esp_app_desc.h"
#include "esp_partition.h"
#include "esp_log.h"
#include "mbedtls/sha256.h"

#include "version.h"

static const char *TAG = "ota";

typedef struct {
    ota_state_t            state;
    esp_ota_handle_t       handle;
    const esp_partition_t *target;
    mbedtls_sha256_context sha;
    ota_meta_t             meta;
    uint32_t               received;
    bool                   pending_verify;
} ota_ctx_t;

static ota_ctx_t s_ota;

esp_err_t ota_init(void)
{
    memset(&s_ota, 0, sizeof(s_ota));
    s_ota.state = OTA_IDLE;

    // Determine whether the running app is awaiting verification (rollback).
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t img_state;
    if (running && esp_ota_get_state_partition(running, &img_state) == ESP_OK) {
        s_ota.pending_verify = (img_state == ESP_OTA_IMG_PENDING_VERIFY);
    }
    ESP_LOGI(TAG, "OTA init: running=%s pending_verify=%d",
             running ? running->label : "?", s_ota.pending_verify);
    return ESP_OK;
}

bool ota_pending_verify(void)
{
    return s_ota.pending_verify;
}

esp_err_t ota_confirm(void)
{
    esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
    if (err == ESP_OK) {
        s_ota.pending_verify = false;
        ESP_LOGI(TAG, "running image confirmed healthy");
    }
    return err;
}

esp_err_t ota_rollback(void)
{
    ESP_LOGW(TAG, "rolling back to previous image");
    return esp_ota_mark_app_invalid_rollback_and_reboot();  // does not return on success
}

esp_err_t ota_begin(const ota_meta_t *meta)
{
    if (s_ota.state == OTA_RECEIVING) {
        ota_abort();
    }

    // Product-id guard: reject an image built for a different target BEFORE
    // touching the partition. A wrong-target image rejected after esp_ota_begin()
    // leaves the staging partition polluted with arbitrary bytes.
    if (strncmp(meta->product_id, FW_PRODUCT_ID, sizeof(meta->product_id)) != 0) {
        ESP_LOGE(TAG, "product-id mismatch: '%.*s' != '%s'",
                 (int)sizeof(meta->product_id), meta->product_id, FW_PRODUCT_ID);
        s_ota.state = OTA_ERROR;
        return ESP_ERR_INVALID_ARG;
    }

    const esp_partition_t *next = esp_ota_get_next_update_partition(NULL);
    if (!next) {
        ESP_LOGE(TAG, "no OTA partition available");
        s_ota.state = OTA_ERROR;
        return ESP_ERR_NOT_FOUND;
    }
    if (meta->image_size == 0 || meta->image_size > next->size) {
        ESP_LOGE(TAG, "image size %lu exceeds partition %lu",
                 (unsigned long)meta->image_size, (unsigned long)next->size);
        s_ota.state = OTA_ERROR;
        return ESP_ERR_INVALID_SIZE;
    }

    esp_err_t err = esp_ota_begin(next, meta->image_size, &s_ota.handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_begin failed: %s", esp_err_to_name(err));
        s_ota.state = OTA_ERROR;
        return err;
    }

    s_ota.target   = next;
    s_ota.meta     = *meta;
    s_ota.received = 0;
    s_ota.state    = OTA_RECEIVING;
    mbedtls_sha256_init(&s_ota.sha);
    mbedtls_sha256_starts(&s_ota.sha, 0);   // 0 = SHA-256 (not 224)
    ESP_LOGI(TAG, "OTA begin -> %s (%lu bytes)", next->label,
             (unsigned long)meta->image_size);
    return ESP_OK;
}

esp_err_t ota_write(uint32_t offset, const uint8_t *data, size_t len)
{
    if (s_ota.state != OTA_RECEIVING) {
        return ESP_ERR_INVALID_STATE;
    }
    // Enforce in-order streaming so the host can resume on a link hiccup
    // without restarting the whole transfer.
    if (offset != s_ota.received) {
        ESP_LOGW(TAG, "out-of-order chunk: offset %lu, have %lu",
                 (unsigned long)offset, (unsigned long)s_ota.received);
        return ESP_ERR_INVALID_STATE;
    }
    if (s_ota.received + len > s_ota.meta.image_size) {
        ESP_LOGE(TAG, "write overflow (%lu + %u > %lu)",
                 (unsigned long)s_ota.received, (unsigned)len,
                 (unsigned long)s_ota.meta.image_size);
        ota_abort();
        return ESP_ERR_INVALID_SIZE;
    }
    esp_err_t err = esp_ota_write(s_ota.handle, data, len);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_write failed: %s", esp_err_to_name(err));
        ota_abort();
        return err;
    }
    mbedtls_sha256_update(&s_ota.sha, data, len);
    s_ota.received += len;
    return ESP_OK;
}

uint32_t ota_received(void)
{
    return s_ota.received;
}

esp_err_t ota_end(void)
{
    if (s_ota.state != OTA_RECEIVING) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_ota.received != s_ota.meta.image_size) {
        ESP_LOGE(TAG, "size mismatch: got %lu expected %lu",
                 (unsigned long)s_ota.received,
                 (unsigned long)s_ota.meta.image_size);
        ota_abort();
        return ESP_ERR_INVALID_SIZE;
    }

    // Verify SHA-256 over the received image.
    uint8_t digest[32];
    mbedtls_sha256_finish(&s_ota.sha, digest);
    mbedtls_sha256_free(&s_ota.sha);
    if (memcmp(digest, s_ota.meta.sha256, 32) != 0) {
        ESP_LOGE(TAG, "SHA-256 mismatch — image rejected");
        esp_ota_abort(s_ota.handle);
        s_ota.handle = 0;
        s_ota.state  = OTA_ERROR;
        return ESP_ERR_INVALID_CRC;
    }

    // esp_ota_end also validates the image header/signature.
    esp_err_t err = esp_ota_end(s_ota.handle);
    s_ota.handle = 0;
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_end failed: %s", esp_err_to_name(err));
        s_ota.state = OTA_ERROR;
        return err;
    }

    err = esp_ota_set_boot_partition(s_ota.target);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "set_boot_partition failed: %s", esp_err_to_name(err));
        s_ota.state = OTA_ERROR;
        return err;
    }

    s_ota.state = OTA_READY;
    ESP_LOGI(TAG, "OTA complete -> reboot into %s", s_ota.target->label);
    return ESP_OK;
}

void ota_abort(void)
{
    if (s_ota.handle) {
        esp_ota_abort(s_ota.handle);
        s_ota.handle = 0;
        mbedtls_sha256_free(&s_ota.sha);
    }
    s_ota.state = OTA_IDLE;
}

void ota_get_status(ota_status_t *out)
{
    memset(out, 0, sizeof(*out));
    out->state          = s_ota.state;
    out->received       = s_ota.received;
    out->image_size     = s_ota.meta.image_size;
    out->pending_verify = s_ota.pending_verify;

    const esp_partition_t *running = esp_ota_get_running_partition();
    if (running) {
        snprintf(out->running_partition, sizeof(out->running_partition),
                 "%s", running->label);
    }
    const esp_app_desc_t *desc = esp_app_get_description();
    if (desc) {
        snprintf(out->running_version, sizeof(out->running_version),
                 "%s", desc->version);
    }
}

ota_state_t ota_state(void)
{
    return s_ota.state;
}
