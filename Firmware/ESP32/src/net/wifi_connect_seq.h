#pragma once

// =============================================================================
// wifi_connect_seq.h - WiFi STA connect sequence (pure, host-testable).
//
// wifi_manager.cpp binds these ops to esp_wifi; the order of operations lives
// here so tests/firmware_host/test_wifi_connect_seq.py can drive it with
// recording fakes (PLT-05).
// =============================================================================

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    void (*disconnect)(void);
    void (*stop)(void);
    void (*start)(void);
    void (*set_mode)(bool sta_only);                       // false = AP+STA
    void (*set_sta)(const char *ssid, const char *pass);
    void (*get_sta)(char *ssid, size_t ssid_sz, char *pass, size_t pass_sz);
    bool (*try_connect)(uint32_t timeout_ms);              // connect + wait for IP
    void (*reapply_ap)(void);
    bool (*save_creds)(const char *ssid, const char *pass);
    void (*delay)(uint32_t ms);
} WifiConnectOps;

// Returns true when connected to `ssid` (credentials then persisted).
bool wifi_connect_seq(const WifiConnectOps *ops, const char *ssid, const char *pass);
