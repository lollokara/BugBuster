#include "auth.h"
#include "esp_system.h"
#include "esp_mac.h"
#include "esp_log.h"
#include "esp_random.h"
#include "nvs_flash.h"
#include "nvs.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "auth";
static char s_admin_token[65] = {0};

void auth_init(void)
{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open("auth", NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS namespace 'auth': %s", esp_err_to_name(err));
        return;
    }

    size_t size = sizeof(s_admin_token);
    err = nvs_get_str(nvs, "admin_token", s_admin_token, &size);

    if (err == ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGI(TAG, "Generating new admin token...");
        uint8_t random_bytes[32];
        esp_fill_random(random_bytes, sizeof(random_bytes));

        for (int i = 0; i < 32; i++) {
            sprintf(&s_admin_token[i * 2], "%02x", random_bytes[i]);
        }
        s_admin_token[64] = '\0';

        err = nvs_set_str(nvs, "admin_token", s_admin_token);
        if (err == ESP_OK) {
            nvs_commit(nvs);
            ESP_LOGI(TAG, "New admin token generated and stored in NVS");
        } else {
            ESP_LOGE(TAG, "Failed to store admin token in NVS: %s", esp_err_to_name(err));
        }
    } else if (err == ESP_OK) {
        ESP_LOGI(TAG, "Admin token loaded from NVS");
    } else {
        ESP_LOGE(TAG, "Failed to load admin token from NVS: %s", esp_err_to_name(err));
    }

    nvs_close(nvs);
}

const char* auth_get_admin_token(void)
{
    return s_admin_token;
}

bool auth_verify_token(const char *token)
{
    if (!token) return false;
    // Token must be exactly 64 characters
    if (strlen(token) != 64) return false;
    return (strcmp(token, s_admin_token) == 0);
}
