#pragma once

// =============================================================================
// wifi_hosted.h — C6-side entry point into the vendored ESP-Hosted slave stack
// (components/wifi_hosted, copied from espressif/esp_hosted's "slave/main").
//
// UNTESTED / bench bring-up needed: this vendors code that was designed to
// own its own app_main() at boot; we call its init function ourselves instead
// (CONFIG_ESP_HOSTED_COPROCESSOR_APP_MAIN=n, see sdkconfig.defaults) so it can
// coexist with our own main.c app_main. It has no documented teardown path --
// once started it runs its own persistent tasks (recv_task, host_reset_task)
// for the rest of the boot, bridging WiFi to the P4 over the SDIO pins.
//
// wifi_hosted_start() is idempotent (the vendor init guards itself with a
// static "already done" flag) and safe to call more than once. There is no
// wifi_hosted_stop(): "leaving WiFi streaming mode" only means our own
// display resumes normal rendering (ddp_wifi_stream_mode() going false,
// handled in main.c) -- the hosted bridge itself just goes idle again once
// the P4 stops using its softAP, matching how the vendor stack is meant to
// run continuously in the background.
// =============================================================================

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Start the ESP-Hosted slave bridge (WiFi over SDIO to the P4). Call once,
// the first time the C6 is told to enter WiFi streaming mode
// (ddp_wifi_stream_mode() going true) -- see main.c. Safe to call again.
esp_err_t wifi_hosted_start(void);

#ifdef __cplusplus
}
#endif
