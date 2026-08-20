#include "settings.h"

#include <string.h>
#include "nvs.h"
#include "nvs_flash.h"
#include "esp_log.h"

static const char *TAG = "settings";
#define NVS_NS   "daqhat"
#define NVS_KEY  "settings"
// Bump when the settings_t layout changes so old blobs are discarded.
#define SETTINGS_VERSION  6

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

    g_settings.filter_idx      = 0;     // Wideband
    g_settings.decim_idx       = 3;     // x256
    g_settings.reject_5060     = false;
    g_settings.sr_mode         = false;

    g_settings.fft_enable      = false;
    g_settings.fft_length_idx  = 4;     // 1024
    g_settings.fft_window_idx  = 1;     // Hann
    g_settings.fft_source_idx  = 0;     // Current

    g_settings.brightness_pct  = 100;
    g_settings.dark_mode       = false;

    g_settings.npx_mode        = 1;     // Solid (lit on boot; P4/S3 can switch to Channel)
    g_settings.npx_brightness  = 50;
    g_settings.npx_color       = 0x0000FF00; // green

    g_settings.wifi_enable     = false;
    g_settings.wifi_mode       = 0;     // AP
    g_settings.ssid[0]         = '\0';
    g_settings.password[0]     = '\0';
    g_settings.wifi_status     = 0;
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

    if (err == ESP_OK && len == sizeof(blob)) {
        if (blob.version == SETTINGS_VERSION) {
            g_settings = blob.s;
            ESP_LOGI(TAG, "settings loaded from NVS");
        } else if (blob.version == 5) {
            // Migrate v5 to v6: WiFi fields (ssid/password) added in v6.
            // Copy all fields that existed in v5, set new v6 fields to defaults.
            typedef struct {
                bool     autoranging;
                uint8_t  range_idx;
                uint8_t  sample_rate_idx;
                uint16_t dut_current_ma;
                uint16_t dut_voltage_mv;
                uint8_t  filter_idx;
                uint8_t  decim_idx;
                bool     reject_5060;
                bool     sr_mode;
                bool     fft_enable;
                uint8_t  fft_length_idx;
                uint8_t  fft_window_idx;
                uint8_t  fft_source_idx;
                uint8_t  brightness_pct;
                bool     dark_mode;
                uint8_t  npx_mode;
                uint8_t  npx_brightness;
                uint32_t npx_color;
                bool     wifi_enable;
                uint8_t  wifi_mode;
                uint8_t  wifi_status;
            } settings_v5_t;
            if (len >= sizeof(uint32_t) + sizeof(settings_v5_t)) {
                settings_v5_t *old = (settings_v5_t *)&blob.s;
                g_settings.autoranging     = old->autoranging;
                g_settings.range_idx       = old->range_idx;
                g_settings.sample_rate_idx = old->sample_rate_idx;
                g_settings.dut_current_ma  = old->dut_current_ma;
                g_settings.dut_voltage_mv  = old->dut_voltage_mv;
                g_settings.filter_idx      = old->filter_idx;
                g_settings.decim_idx       = old->decim_idx;
                g_settings.reject_5060     = old->reject_5060;
                g_settings.sr_mode         = old->sr_mode;
                g_settings.fft_enable      = old->fft_enable;
                g_settings.fft_length_idx  = old->fft_length_idx;
                g_settings.fft_window_idx  = old->fft_window_idx;
                g_settings.fft_source_idx  = old->fft_source_idx;
                g_settings.brightness_pct  = old->brightness_pct;
                g_settings.dark_mode       = old->dark_mode;
                g_settings.npx_mode        = old->npx_mode;
                g_settings.npx_brightness  = old->npx_brightness;
                g_settings.npx_color       = old->npx_color;
                g_settings.wifi_enable     = old->wifi_enable;
                g_settings.wifi_mode       = old->wifi_mode;
                g_settings.wifi_status     = old->wifi_status;
                // New v6 fields: set defaults.
                g_settings.ssid[0]         = '\0';
                g_settings.password[0]     = '\0';
                settings_save();  // Persist as v6.
                ESP_LOGI(TAG, "settings migrated v5 -> v6");
            } else {
                ESP_LOGI(TAG, "settings v5 blob too short; using defaults");
            }
        } else {
            ESP_LOGI(TAG, "settings version %lu unsupported; using defaults", (unsigned long)blob.version);
        }
    } else {
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "settings blob size mismatch; using defaults");
        }
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

