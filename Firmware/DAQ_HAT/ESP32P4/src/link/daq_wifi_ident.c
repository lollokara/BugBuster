// =============================================================================
// daq_wifi_ident.c — SSID/password generator for the P4 softAP (see .h).
// =============================================================================

#include "daq_wifi_ident.h"

#include <stdio.h>
#include "esp_mac.h"
#include "esp_random.h"

// Excludes visually-ambiguous characters (0/O, 1/I/l). 32 chars total.
static const char s_charset[] = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789";

void daq_wifi_ident_generate(char *ssid, size_t ssid_len, char *password, size_t pass_len)
{
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(ssid, ssid_len, "BugBusterDAQ-%02X%02X%02X", mac[3], mac[4], mac[5]);

    uint8_t rnd[8];
    esp_fill_random(rnd, sizeof(rnd));
    size_t n = (pass_len > 0) ? (pass_len - 1) : 0;
    if (n > sizeof(rnd)) n = sizeof(rnd);
    for (size_t i = 0; i < n; i++) {
        password[i] = s_charset[rnd[i] % 32u];
    }
    if (pass_len > 0) password[n] = '\0';
}
