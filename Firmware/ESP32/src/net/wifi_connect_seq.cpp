// =============================================================================
// wifi_connect_seq.cpp - see wifi_connect_seq.h.
// =============================================================================

#include "wifi_connect_seq.h"

bool wifi_connect_seq(const WifiConnectOps *ops, const char *ssid, const char *pass)
{
    // PLT-05: stay in AP+STA for the whole attempt. Switching to STA-only
    // dropped the AP (the link HTTP clients and the phone use) for up to a
    // minute, and a failed attempt then had no fallback path to the device.
    char prev_ssid[33] = {};
    char prev_pass[65] = {};
    ops->get_sta(prev_ssid, sizeof(prev_ssid), prev_pass, sizeof(prev_pass));

    ops->disconnect();
    ops->set_mode(false);
    ops->set_sta(ssid, pass);

    // Retry up to 5 times with increasing delay
    bool connected = false;
    for (int attempt = 0; attempt < 5 && !connected; attempt++) {
        if (attempt > 0) {
            ops->disconnect();
            ops->delay(500 + attempt * 500);
        }
        connected = ops->try_connect(10000);
    }

    if (connected) {
        ops->save_creds(ssid, pass);
        return true;
    }

    // Failed: put the previous STA network back so the reconnect worker goes
    // back to it instead of chasing the SSID that just failed.
    ops->disconnect();
    ops->set_sta(prev_ssid, prev_pass);
    if (prev_ssid[0]) ops->try_connect(10000);
    return false;
}
