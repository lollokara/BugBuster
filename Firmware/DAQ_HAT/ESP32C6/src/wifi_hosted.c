// =============================================================================
// wifi_hosted.c — see wifi_hosted.h. UNTESTED, needs hardware bring-up.
// =============================================================================

#include "wifi_hosted.h"
#include "esp_hosted_coprocessor.h"
#include "esp_log.h"

static const char *TAG = "wifi_hosted";

esp_err_t wifi_hosted_start(void)
{
    esp_err_t err = esp_hosted_coprocessor_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_hosted_coprocessor_init failed: %s", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "ESP-Hosted slave bridge started (WiFi over SDIO)");
    }
    return err;
}
