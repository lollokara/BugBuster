// =============================================================================
// relay_stage.c — see relay_stage.h.
// =============================================================================

#include "relay_stage.h"
#include <string.h>
#include "esp_partition.h"
#include "esp_log.h"
#include "nvs.h"
#include "mbedtls/sha256.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "relay_stage";
#define RELAY_NVS_NS "relay"
#define STAGE_ERASE_BLOCK 4096u

// Persist to NVS only every this many bytes of staging progress (plus always
// at relay_stage_end()), to avoid tens of thousands of NVS commits per image.
// Tradeoff: a P4 reset mid-staging may redo up to this many bytes of the
// current chunk run (not the whole image), since staged_bytes on disk may
// lag the true flash contents by up to this much.
#define STAGE_PERSIST_INTERVAL (64u * 1024u)

typedef struct {
    relay_status_t          status;
    const esp_partition_t  *part;
    uint32_t                erased_through;   // bytes of `staging` erased so far
    uint32_t                last_persisted_bytes;
    SemaphoreHandle_t       lock;
} relay_ctx_t;

static relay_ctx_t s_relay;

static void lock(void)   { if (s_relay.lock) xSemaphoreTake(s_relay.lock, portMAX_DELAY); }
static void unlock(void) { if (s_relay.lock) xSemaphoreGive(s_relay.lock); }

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
    s_relay.last_persisted_bytes = s_relay.status.staged_bytes;
}

esp_err_t relay_stage_init(void)
{
    SemaphoreHandle_t existing_lock = s_relay.lock;
    memset(&s_relay, 0, sizeof(s_relay));
    s_relay.lock = existing_lock ? existing_lock : xSemaphoreCreateMutex();
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
    if (s_relay.status.state == RELAY_STAGING) {
        // Resuming a staging run interrupted by a P4 reset: re-derive
        // erased_through from staged_bytes so a resumed write at
        // offset == staged_bytes doesn't re-erase (and wipe) the already
        // staged bytes from 0. SHA-256 verification is deferred to
        // relay_stage_end(), which re-reads the whole staged image back
        // from flash rather than relying on an in-memory running digest
        // (which cannot survive a reset).
        s_relay.erased_through = (s_relay.status.staged_bytes + STAGE_ERASE_BLOCK - 1) &
                                  ~(STAGE_ERASE_BLOCK - 1);
    }
    s_relay.last_persisted_bytes = s_relay.status.staged_bytes;
    ESP_LOGI(TAG, "relay init: target=%d state=%d staged=%lu pushed=%lu",
             s_relay.status.target, s_relay.status.state,
             (unsigned long)s_relay.status.staged_bytes,
             (unsigned long)s_relay.status.pushed_bytes);
    return ESP_OK;
}

esp_err_t relay_stage_begin(relay_target_t target, const ota_meta_t *meta)
{
    lock();
    const esp_partition_t *part = stage_partition();
    if (!part) { unlock(); return ESP_ERR_NOT_FOUND; }
    if (meta->image_size == 0 || meta->image_size > part->size) {
        ESP_LOGE(TAG, "image size %lu exceeds staging partition %lu",
                 (unsigned long)meta->image_size, (unsigned long)part->size);
        unlock();
        return ESP_ERR_INVALID_SIZE;
    }
    // Only one relay in flight at a time: reject if a staging/pushing run is
    // already active, regardless of target (a caller wanting to restart
    // should abort first).
    if (s_relay.status.state == RELAY_STAGING || s_relay.status.state == RELAY_PUSHING) {
        ESP_LOGW(TAG, "relay_stage_begin: rejected, relay already in flight (state=%d)",
                 s_relay.status.state);
        unlock();
        return ESP_ERR_INVALID_STATE;
    }
    memset(&s_relay.status, 0, sizeof(s_relay.status));
    s_relay.status.target     = target;
    s_relay.status.state      = RELAY_STAGING;
    s_relay.status.image_size = meta->image_size;
    memcpy(s_relay.status.sha256, meta->sha256, 32);
    s_relay.erased_through = 0;
    s_relay.last_persisted_bytes = 0;
    persist_status();
    ESP_LOGI(TAG, "relay stage begin: target=%d size=%lu", target, (unsigned long)meta->image_size);
    unlock();
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
    lock();
    if (s_relay.status.state != RELAY_STAGING) { unlock(); return ESP_ERR_INVALID_STATE; }
    if (offset != s_relay.status.staged_bytes) {
        ESP_LOGW(TAG, "out-of-order stage chunk: offset %lu, have %lu",
                 (unsigned long)offset, (unsigned long)s_relay.status.staged_bytes);
        unlock();
        return ESP_ERR_INVALID_STATE;
    }
    if (offset + len > s_relay.status.image_size) { unlock(); return ESP_ERR_INVALID_SIZE; }

    esp_err_t err = ensure_erased_through(offset + (uint32_t)len);
    if (err != ESP_OK) { unlock(); return err; }

    const esp_partition_t *part = stage_partition();
    err = esp_partition_write(part, offset, data, len);
    if (err != ESP_OK) { unlock(); return err; }

    s_relay.status.staged_bytes += (uint32_t)len;
    // Persist only every STAGE_PERSIST_INTERVAL bytes of progress (see
    // comment above STAGE_PERSIST_INTERVAL) to bound NVS wear; the final
    // state is always persisted in relay_stage_end()/relay_stage_reset().
    if (s_relay.status.staged_bytes - s_relay.last_persisted_bytes >= STAGE_PERSIST_INTERVAL) {
        persist_status();
    }
    unlock();
    return ESP_OK;
}

esp_err_t relay_stage_end(void)
{
    lock();
    if (s_relay.status.state != RELAY_STAGING) { unlock(); return ESP_ERR_INVALID_STATE; }
    if (s_relay.status.staged_bytes != s_relay.status.image_size) {
        s_relay.status.state = RELAY_FAILED;
        persist_status();
        unlock();
        return ESP_ERR_INVALID_SIZE;
    }

    // Compute SHA-256 over the full staged image by reading it back from
    // flash, rather than relying on an incremental digest kept in RAM during
    // relay_stage_write(). This is correct regardless of whether staging was
    // interrupted and resumed by a P4 reset partway through (an in-memory
    // running digest cannot be checkpointed/restored across a reset).
    const esp_partition_t *part = stage_partition();
    if (!part) {
        s_relay.status.state = RELAY_FAILED;
        persist_status();
        unlock();
        return ESP_ERR_NOT_FOUND;
    }

    mbedtls_sha256_context sha;
    mbedtls_sha256_init(&sha);
    mbedtls_sha256_starts(&sha, 0);

    uint8_t buf[4096];
    uint32_t remaining = s_relay.status.image_size;
    uint32_t off = 0;
    esp_err_t err = ESP_OK;
    while (remaining > 0) {
        size_t chunk = remaining < sizeof(buf) ? remaining : sizeof(buf);
        err = esp_partition_read(part, off, buf, chunk);
        if (err != ESP_OK) break;
        mbedtls_sha256_update(&sha, buf, chunk);
        off += (uint32_t)chunk;
        remaining -= (uint32_t)chunk;
    }

    uint8_t digest[32];
    mbedtls_sha256_finish(&sha, digest);
    mbedtls_sha256_free(&sha);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "staged image read-back failed: %d", err);
        s_relay.status.state = RELAY_FAILED;
        persist_status();
        unlock();
        return err;
    }

    if (memcmp(digest, s_relay.status.sha256, 32) != 0) {
        ESP_LOGE(TAG, "staged image SHA-256 mismatch");
        s_relay.status.state = RELAY_FAILED;
        persist_status();
        unlock();
        return ESP_ERR_INVALID_CRC;
    }
    s_relay.status.state = RELAY_STAGED;
    persist_status();
    ESP_LOGI(TAG, "relay stage verified: %lu bytes", (unsigned long)s_relay.status.staged_bytes);
    unlock();
    return ESP_OK;
}

int relay_stage_read(uint32_t offset, uint8_t *out, size_t len)
{
    lock();
    if (offset >= s_relay.status.staged_bytes) { unlock(); return 0; }
    size_t clamped = len;
    if (offset + clamped > s_relay.status.staged_bytes) {
        clamped = s_relay.status.staged_bytes - offset;
    }
    const esp_partition_t *part = stage_partition();
    if (!part) { unlock(); return -1; }
    int ret = (esp_partition_read(part, offset, out, clamped) != ESP_OK) ? -1 : (int)clamped;
    unlock();
    return ret;
}

esp_err_t relay_stage_set_pushed_bytes(uint32_t pushed)
{
    lock();
    s_relay.status.pushed_bytes = pushed;
    if (s_relay.status.state == RELAY_STAGED) s_relay.status.state = RELAY_PUSHING;
    persist_status();
    unlock();
    return ESP_OK;
}

void relay_stage_get_status(relay_status_t *out)
{
    lock();
    *out = s_relay.status;
    unlock();
}

void relay_stage_reset(relay_state_t final_state)
{
    lock();
    s_relay.status.state = final_state;
    persist_status();
    unlock();
}
