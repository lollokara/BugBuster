// =============================================================================
// wifi_connect_seq.cpp - see wifi_connect_seq.h.
// =============================================================================

#include "wifi_connect_seq.h"

bool wifi_connect_seq(const WifiConnectOps *ops, const char *ssid, const char *pass)
{
    // Full stop/restart for a clean connection
    ops->disconnect();
    ops->stop();
    ops->delay(200);

    // STA-only mode for connection (avoids AP channel conflicts)
    ops->set_mode(true);
    ops->set_sta(ssid, pass);
    ops->start();
    ops->delay(200);

    // Retry up to 5 times with increasing delay
    bool connected = false;
    for (int attempt = 0; attempt < 5 && !connected; attempt++) {
        if (attempt > 0) {
            ops->disconnect();
            ops->delay(500 + attempt * 500);
        }
        connected = ops->try_connect(10000);
    }

    // Restore AP+STA mode
    if (!connected) ops->disconnect();
    ops->stop();
    ops->delay(100);
    ops->set_mode(false);
    ops->reapply_ap();
    ops->set_sta(ssid, pass);   // keep the network we just tried
    ops->start();

    // If we were connected in STA-only mode, reconnect in APSTA mode
    if (connected) connected = ops->try_connect(10000);

    if (connected) ops->save_creds(ssid, pass);
    return connected;
}
