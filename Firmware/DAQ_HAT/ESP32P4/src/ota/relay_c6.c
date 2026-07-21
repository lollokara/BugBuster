// =============================================================================
// relay_c6.c — see relay_c6.h.
// =============================================================================

#include "relay_c6.h"
#include "relay_stage.h"
#include "c6_flasher.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "relay_c6";

esp_err_t relay_c6_push(void)
{
    relay_status_t st;
    relay_stage_get_status(&st);
    if (st.target != RELAY_TARGET_C6 || (st.state != RELAY_STAGED && st.state != RELAY_PUSHING)) {
        ESP_LOGE(TAG, "no C6 relay ready (state=%d target=%d)", st.state, st.target);
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t err = c6_flasher_begin(st.image_size, 0);
    if (err != ESP_OK) return err;

    uint8_t buf[RELAY_C6_CHUNK_SIZE];
    uint32_t offset = st.pushed_bytes;   // resume point
    while (offset < st.image_size) {
        size_t want = st.image_size - offset;
        if (want > RELAY_C6_CHUNK_SIZE) want = RELAY_C6_CHUNK_SIZE;

        int n = relay_stage_read(offset, buf, want);
        if (n <= 0) {
            ESP_LOGE(TAG, "staging read failed at offset %lu", (unsigned long)offset);
            c6_flasher_abort();
            relay_stage_reset(RELAY_FAILED);
            return ESP_FAIL;
        }

        esp_err_t werr = ESP_FAIL;
        for (uint32_t attempt = 0; attempt < RELAY_C6_MAX_RETRIES; attempt++) {
            werr = c6_flasher_write(buf, (size_t)n);
            if (werr == ESP_OK) break;
            ESP_LOGW(TAG, "C6 chunk write failed (attempt %lu/%u) at offset %lu: %s",
                     (unsigned long)attempt + 1, RELAY_C6_MAX_RETRIES, (unsigned long)offset,
                     esp_err_to_name(werr));
            vTaskDelay(pdMS_TO_TICKS(100 * (attempt + 1)));   // linear backoff
        }
        if (werr != ESP_OK) {
            c6_flasher_abort();
            relay_stage_reset(RELAY_FAILED);
            return werr;
        }

        offset += (uint32_t)n;
        relay_stage_set_pushed_bytes(offset);   // persists to NVS for resume
    }

    err = c6_flasher_finish();   // flushes final block, verifies MD5, resets C6
    if (err != ESP_OK) {
        relay_stage_reset(RELAY_FAILED);
        return err;
    }

    relay_stage_reset(RELAY_DONE);
    ESP_LOGI(TAG, "C6 relay complete: %lu bytes", (unsigned long)st.image_size);
    return ESP_OK;
}
