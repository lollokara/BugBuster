#include "auth.h"
#include "esp_system.h"
#include "esp_mac.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "mbedtls/sha256.h"
#include <string.h>

static const char *TAG = "auth";
static char s_admin_token[33] = {0};

// Hardware-specific salt for token derivation. 
// In a real production environment, this should be unique per batch or 
// randomized at first boot, but for this "zero-config" stage we use a 
// static firmware salt combined with the unique MAC.
static const char *AUTH_SALT = "BugBuster_Secure_2026_Salt_!@#";

void auth_init(void)
{
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);

    // Derive token: SHA256(MAC + SALT)
    mbedtls_sha256_context ctx;
    mbedtls_sha256_init(&ctx);
    mbedtls_sha256_starts(&ctx, 0); // 0 = SHA256
    mbedtls_sha256_update(&ctx, mac, 6);
    mbedtls_sha256_update(&ctx, (const uint8_t*)AUTH_SALT, strlen(AUTH_SALT));
    
    uint8_t hash[32];
    mbedtls_sha256_finish(&ctx, hash);
    mbedtls_sha256_free(&ctx);

    // Convert first 16 bytes of hash to hex string (32 chars)
    for (int i = 0; i < 16; i++) {
        sprintf(&s_admin_token[i * 2], "%02x", hash[i]);
    }
    s_admin_token[32] = '\0';

    ESP_LOGI(TAG, "Admin token derived from hardware ID");
}

const char* auth_get_admin_token(void)
{
    return s_admin_token;
}

bool auth_verify_token(const char *token)
{
    if (!token) return false;
    return (strcmp(token, s_admin_token) == 0);
}
