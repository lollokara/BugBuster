#include "auth.h"
#include "esp_system.h"
#include "esp_mac.h"
#include "esp_log.h"
#include "esp_random.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "mbedtls/sha256.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "auth";
static char s_admin_token[65] = {0};

void auth_init(void)
{
    // Ensure the NVS flash partition is initialised before opening namespaces.
    // Safe to call multiple times — nvs_flash_init() only does work once.
    // If it returns NO_FREE_PAGES or NEW_VERSION_FOUND, the partition is full
    // or out of date: erase and retry once.
    esp_err_t init_err = nvs_flash_init();
    if (init_err == ESP_ERR_NVS_NO_FREE_PAGES ||
        init_err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS partition needs erasing (%s), recreating...",
                 esp_err_to_name(init_err));
        ESP_ERROR_CHECK(nvs_flash_erase());
        init_err = nvs_flash_init();
    }
    if (init_err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_flash_init failed: %s", esp_err_to_name(init_err));
        return;
    }

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
            err = nvs_commit(nvs);
            if (err == ESP_OK) {
                ESP_LOGI(TAG, "New admin token generated and stored in NVS");
            } else {
                ESP_LOGE(TAG, "Failed to commit admin token to NVS: %s", esp_err_to_name(err));
            }
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
    // Constant-time comparison: avoid leaking common-prefix length via timing.
    // Both buffers are guaranteed 64 bytes here (length checked above and
    // s_admin_token is 64 hex chars when populated); if the device token
    // hasn't been initialised this returns false because s_admin_token is all
    // zeros, which won't match any 64-char hex string.
    volatile uint8_t diff = 0;
    for (size_t i = 0; i < 64; i++) {
        diff |= (uint8_t)token[i] ^ (uint8_t)s_admin_token[i];
    }
    return diff == 0;
}

bool auth_rotate_token(char out[65])
{
    if (!out) return false;

    // Generate 32 fresh random bytes and render as 64 hex chars.
    uint8_t random_bytes[32];
    esp_fill_random(random_bytes, sizeof(random_bytes));
    char fresh[65] = {0};
    for (int i = 0; i < 32; i++) {
        sprintf(&fresh[i * 2], "%02x", random_bytes[i]);
    }
    fresh[64] = '\0';

    // Persist BEFORE swapping the in-memory token: if NVS write fails we
    // leave the previous token active (caller can retry), instead of
    // ending up with an in-memory token that won't survive reboot.
    nvs_handle_t nvs;
    esp_err_t err = nvs_open("auth", NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "auth_rotate_token: nvs_open failed: %s", esp_err_to_name(err));
        return false;
    }
    err = nvs_set_str(nvs, "admin_token", fresh);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "auth_rotate_token: nvs_set_str failed: %s", esp_err_to_name(err));
        nvs_close(nvs);
        return false;
    }
    err = nvs_commit(nvs);
    nvs_close(nvs);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "auth_rotate_token: nvs_commit failed: %s", esp_err_to_name(err));
        return false;
    }

    // Commit to memory and return.
    memcpy(s_admin_token, fresh, sizeof(fresh));
    memcpy(out, fresh, sizeof(fresh));
    ESP_LOGI(TAG, "Admin token rotated");
    return true;
}

bool auth_token_fingerprint(char out[17])
{
    if (!out) return false;
    if (s_admin_token[0] == '\0') {
        out[0] = '\0';
        return false;
    }

    uint8_t digest[32];
    int rc = mbedtls_sha256((const unsigned char *)s_admin_token,
                            strlen(s_admin_token),
                            digest,
                            0 /* is224 = false, use SHA-256 */);
    if (rc != 0) {
        ESP_LOGW(TAG, "sha256 failed: %d", rc);
        out[0] = '\0';
        return false;
    }

    // First 8 bytes → 16 lowercase hex chars + NUL.
    for (int i = 0; i < 8; i++) {
        sprintf(&out[i * 2], "%02x", digest[i]);
    }
    out[16] = '\0';
    return true;
}
