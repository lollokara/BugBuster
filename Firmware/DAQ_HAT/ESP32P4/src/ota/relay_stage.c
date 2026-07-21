// =============================================================================
// relay_stage.c — see relay_stage.h.
// =============================================================================

#include "relay_stage.h"
#include <string.h>
#include "esp_partition.h"
#include "esp_log.h"
#include "nvs.h"
#include "mbedtls/sha256.h"

static const char *TAG = "relay_stage";
#define RELAY_NVS_NS "relay"
#define STAGE_ERASE_BLOCK 4096u

typedef struct {
    relay_status_t          status;
    const esp_partition_t  *part;
    mbedtls_sha256_context  sha;
    uint32_t                erased_through;   // bytes of `staging` erased so far
} relay_ctx_t;

static relay_ctx_t s_relay;

static const esp_partition_t *stage_partition(void)
{
    if (!s_relay.part) {
        s_relay.part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, 0x40, "staging");
    }
    return s_relay.part;
}

static void persist_status(void)
{
    nvs_handle_t h;
    if (nvs_open(RELAY_NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_blob(h, "status", &s_relay.status, sizeof(s_relay.status));
    nvs_commit(h);
    nvs_close(h);
}

esp_err_t relay_stage_init(void)
{
    memset(&s_relay, 0, sizeof(s_relay));
    if (!stage_partition()) {
        ESP_LOGE(TAG, "staging partition not found");
        return ESP_ERR_NOT_FOUND;
    }
    nvs_handle_t h;
    if (nvs_open(RELAY_NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        size_t len = sizeof(s_relay.status);
        nvs_get_blob(h, "status", &s_relay.status, &len);
        nvs_close(h);
    }
    ESP_LOGI(TAG, "relay init: target=%d state=%d staged=%lu pushed=%lu",
             s_relay.status.target, s_relay.status.state,
             (unsigned long)s_relay.status.staged_bytes,
             (unsigned long)s_relay.status.pushed_bytes);
    return ESP_OK;
}

esp_err_t relay_stage_begin(relay_target_t target, const ota_meta_t *meta)
{
    const esp_partition_t *part = stage_partition();
    if (!part) return ESP_ERR_NOT_FOUND;
    if (meta->image_size == 0 || meta->image_size > part->size) {
        ESP_LOGE(TAG, "image size %lu exceeds staging partition %lu",
                 (unsigned long)meta->image_size, (unsigned long)part->size);
        return ESP_ERR_INVALID_SIZE;
    }
    memset(&s_relay.status, 0, sizeof(s_relay.status));
    s_relay.status.target     = target;
    s_relay.status.state      = RELAY_STAGING;
    s_relay.status.image_size = meta->image_size;
    memcpy(s_relay.status.sha256, meta->sha256, 32);
    s_relay.erased_through = 0;
    mbedtls_sha256_init(&s_relay.sha);
    mbedtls_sha256_starts(&s_relay.sha, 0);
    persist_status();
    ESP_LOGI(TAG, "relay stage begin: target=%d size=%lu", target, (unsigned long)meta->image_size);
    return ESP_OK;
}

static esp_err_t ensure_erased_through(uint32_t end_offset)
{
    const esp_partition_t *part = stage_partition();
    uint32_t aligned_end = (end_offset + STAGE_ERASE_BLOCK - 1) & ~(STAGE_ERASE_BLOCK - 1);
    if (aligned_end <= s_relay.erased_through) return ESP_OK;
    uint32_t erase_len = aligned_end - s_relay.erased_through;
    esp_err_t err = esp_partition_erase_range(part, s_relay.erased_through, erase_len);
    if (err == ESP_OK) s_relay.erased_through = aligned_end;
    return err;
}

esp_err_t relay_stage_write(uint32_t offset, const uint8_t *data, size_t len)
{
    if (s_relay.status.state != RELAY_STAGING) return ESP_ERR_INVALID_STATE;
    if (offset != s_relay.status.staged_bytes) {
        ESP_LOGW(TAG, "out-of-order stage chunk: offset %lu, have %lu",
                 (unsigned long)offset, (unsigned long)s_relay.status.staged_bytes);
        return ESP_ERR_INVALID_STATE;
    }
    if (offset + len > s_relay.status.image_size) return ESP_ERR_INVALID_SIZE;

    esp_err_t err = ensure_erased_through(offset + (uint32_t)len);
    if (err != ESP_OK) return err;

    const esp_partition_t *part = stage_partition();
    err = esp_partition_write(part, offset, data, len);
    if (err != ESP_OK) return err;

    mbedtls_sha256_update(&s_relay.sha, data, len);
    s_relay.status.staged_bytes += (uint32_t)len;
    persist_status();
    return ESP_OK;
}

esp_err_t relay_stage_end(void)
{
    if (s_relay.status.state != RELAY_STAGING) return ESP_ERR_INVALID_STATE;
    if (s_relay.status.staged_bytes != s_relay.status.image_size) {
        s_relay.status.state = RELAY_FAILED;
        persist_status();
        return ESP_ERR_INVALID_SIZE;
    }
    uint8_t digest[32];
    mbedtls_sha256_finish(&s_relay.sha, digest);
    mbedtls_sha256_free(&s_relay.sha);
    if (memcmp(digest, s_relay.status.sha256, 32) != 0) {
        ESP_LOGE(TAG, "staged image SHA-256 mismatch");
        s_relay.status.state = RELAY_FAILED;
        persist_status();
        return ESP_ERR_INVALID_CRC;
    }
    s_relay.status.state = RELAY_STAGED;
    persist_status();
    ESP_LOGI(TAG, "relay stage verified: %lu bytes", (unsigned long)s_relay.status.staged_bytes);
    return ESP_OK;
}

int relay_stage_read(uint32_t offset, uint8_t *out, size_t len)
{
    if (offset >= s_relay.status.staged_bytes) return 0;
    size_t clamped = len;
    if (offset + clamped > s_relay.status.staged_bytes) {
        clamped = s_relay.status.staged_bytes - offset;
    }
    const esp_partition_t *part = stage_partition();
    if (!part) return -1;
    if (esp_partition_read(part, offset, out, clamped) != ESP_OK) return -1;
    return (int)clamped;
}

esp_err_t relay_stage_set_pushed_bytes(uint32_t pushed)
{
    s_relay.status.pushed_bytes = pushed;
    if (s_relay.status.state == RELAY_STAGED) s_relay.status.state = RELAY_PUSHING;
    persist_status();
    return ESP_OK;
}

void relay_stage_get_status(relay_status_t *out)
{
    *out = s_relay.status;
}

void relay_stage_reset(relay_state_t final_state)
{
    s_relay.status.state = final_state;
    persist_status();
}
