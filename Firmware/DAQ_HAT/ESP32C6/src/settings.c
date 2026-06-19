#include "settings.h"

#include <string.h>
#include "nvs.h"
#include "nvs_flash.h"
#include "esp_log.h"

static const char *TAG = "settings";
#define NVS_NS   "daqhat"
#define NVS_KEY  "settings"
// Bump when the settings_t layout changes so old blobs are discarded.
#define SETTINGS_VERSION  1

settings_t g_settings;

const char *const SETTINGS_RANGE[]      = { "A", "mA", "uA" };
const char *const SETTINGS_SAMPLERATE[] = { "10ksps", "50ksps", "100ksps",
                                            "250ksps", "1Msps" };

// On-flash blob: a version tag followed by the settings struct.
typedef struct {
    uint32_t   version;
    settings_t s;
} nvs_blob_t;

static void load_defaults(void)
{
    g_settings.autoranging     = true;
    g_settings.range_idx       = 1;     // mA
    g_settings.sample_rate_idx = 2;     // 100ksps
    g_settings.dut_current_ma  = 1000;  // 1.0 A
    g_settings.dut_voltage_mv  = 5000;  // 5.0 V
    g_settings.brightness_pct  = 100;
    g_settings.dark_mode       = false;
}

void settings_init(void)
{
    load_defaults();

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        err = nvs_flash_init();
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "NVS init failed (%s); using defaults", esp_err_to_name(err));
        return;
    }

    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) {
        ESP_LOGI(TAG, "no saved settings; using defaults");
        return;
    }
    nvs_blob_t blob;
    size_t len = sizeof(blob);
    err = nvs_get_blob(h, NVS_KEY, &blob, &len);
    nvs_close(h);

    if (err == ESP_OK && len == sizeof(blob) && blob.version == SETTINGS_VERSION) {
        g_settings = blob.s;
        ESP_LOGI(TAG, "settings loaded from NVS");
    } else {
        ESP_LOGI(TAG, "settings invalid/old; using defaults");
    }
}

void settings_save(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
        ESP_LOGW(TAG, "save: nvs_open failed");
        return;
    }
    nvs_blob_t blob = { .version = SETTINGS_VERSION, .s = g_settings };
    esp_err_t err = nvs_set_blob(h, NVS_KEY, &blob, sizeof(blob));
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    if (err != ESP_OK) ESP_LOGW(TAG, "save failed: %s", esp_err_to_name(err));
}

