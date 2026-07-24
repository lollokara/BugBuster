// =============================================================================
// relay_c6.c — see relay_c6.h.
// =============================================================================

#include "relay_c6.h"
#include "relay_stage.h"
#include "c6_flasher.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "config.h"   // C6_BOOT_PIN / C6_BOOT_EN_PIN

static const char *TAG = "relay_c6";

esp_err_t relay_c6_push(void)
{
    relay_status_t st;
    relay_stage_get_status(&st);
    if (st.target != RELAY_TARGET_C6 || (st.state != RELAY_STAGED && st.state != RELAY_PUSHING)) {
        ESP_LOGE(TAG, "no C6 relay ready (state=%d target=%d)", st.state, st.target);
        return ESP_ERR_INVALID_STATE;
    }

    uint32_t offset = st.pushed_bytes;   // resume point (0 on a fresh push)
    esp_err_t err = c6_flasher_begin(st.image_size - offset, offset);
    if (err != ESP_OK) {
        relay_stage_reset(RELAY_FAILED);
        return err;
    }

    // Persist resume progress only every this many bytes (mirrors
    // STAGE_PERSIST_INTERVAL in relay_stage.c) -- committing to NVS on every
    // single 256-byte chunk (~5200 commits for a 1.3 MB image) blocked long
    // enough, with no scheduler yield in between, to starve CPU0's
    // watchdog-checked IDLE task and reset the P4 mid-push.
    #define RELAY_C6_PERSIST_INTERVAL (16u * 1024u)
    uint32_t last_persisted = offset;

    uint8_t buf[RELAY_C6_CHUNK_SIZE];
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
        if (offset - last_persisted >= RELAY_C6_PERSIST_INTERVAL || offset >= st.image_size) {
            relay_stage_set_pushed_bytes(offset);   // persists to NVS for resume
            last_persisted = offset;
        }
        // Unconditional yield every chunk regardless of the persist interval
        // above -- a resumed run may redo up to RELAY_C6_PERSIST_INTERVAL
        // bytes of UART writes on a P4 reset, a bounded tradeoff against NVS
        // wear (same tradeoff STAGE_PERSIST_INTERVAL already accepts).
        vTaskDelay(1);
    }

    // Release BOOT strapping before the reset inside c6_flasher_finish()
    // (esp_loader_reset_target()) -- c6_flasher_begin() leaves C6_BOOT_PIN
    // driven LOW (download mode) for the whole transfer and c6_flasher.c
    // never releases it itself (cmd_c6flash in cli.c does this same release
    // inline before its own finish call; relay_c6_push() is a separate entry
    // point and needs the identical step). Without this, the post-flash
    // reset lands the C6 right back in ROM download mode instead of running
    // the new image -- silent (no crash, no log), just a black screen forever.
    gpio_set_level((gpio_num_t)C6_BOOT_PIN,    1);
    gpio_set_level((gpio_num_t)C6_BOOT_EN_PIN, 1);

    err = c6_flasher_finish();   // flushes final block, verifies MD5, resets C6
    if (err != ESP_OK) {
        relay_stage_reset(RELAY_FAILED);
        return err;
    }

    relay_stage_reset(RELAY_DONE);
    ESP_LOGI(TAG, "C6 relay complete: %lu bytes", (unsigned long)st.image_size);
    return ESP_OK;
}
