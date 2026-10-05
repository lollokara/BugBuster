// =============================================================================
// hub_task.cpp - see hub_task.h. One unit of work per pass, in priority order:
// resolve hub -> register -> logs -> live samples -> run sync. Errors back off
// (5 s doubling to 60 s); nothing here ever blocks the web server or BLE.
// =============================================================================

#include "hub_task.h"

#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "hub_config.h"
#include "hub_health.h"
#include "hub_health_glue.h"
#include "hub_live.h"
#include "hub_logs.h"
#include "hub_net.h"
#include "hub_policy.h"
#include "hub_status.h"
#include "hub_sync.h"
#include "wifi_manager.h"

static const char *TAG = "hub";

#define REGISTER_MS   30000u
#define EPOCH_MS      3600000u
#define MAX_FAILS     3

void hub_early_init(void) { hub_logs_init(); }

static void hub_task(void *)
{
    hub_config_t cfg;
    hub_config_load(&cfg);
    hub_logs_set_level(cfg.log_level);
    uint32_t gen = hub_config_generation();
    char base[HUB_URL_MAX] = "", discovered[HUB_URL_MAX] = "", cached[HUB_URL_MAX] = "";
    hub_urlsrc_t src = HUB_URLSRC_NONE;
    bool need_resolve = true, registered = false, was_connected = false;
    int fails = 0;
    uint32_t next_register = 0, backoff = 0, next_ok = 0, next_epoch = 0;
    hub_health_sched_t health = {};
    bool health_first = true;

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(250));
        bool up = wifi_is_connected();
        if (up && !was_connected) { need_resolve = true; registered = false; discovered[0] = '\0'; }   // (re)connect
        was_connected = up;
        if (gen != hub_config_generation()) {                    // settings changed from the app or portal
            gen = hub_config_generation();
            hub_config_load(&cfg);
            hub_logs_set_level(cfg.log_level);
            need_resolve = true; registered = false; discovered[0] = '\0';
            hub_sync_reset();
        }
        // HEALTH record: queued in the log ring (buffered while the hub is unreachable), so it runs
        // before the link checks. First one 60 s after the first successful register, then hourly.
        if (cfg.hub_enabled && hub_health_sched_due(&health, hub_uptime_ms(), registered)) {
            hub_health_emit(health_first);
            health_first = false;
        }
        if (!cfg.hub_enabled || !up) { hub_status_note_push(false, 0, cfg.hub_enabled ? "no WiFi" : "disabled"); continue; }
        uint32_t now = hub_uptime_ms();
        hub_logs_pull_p4();
        hub_status_set_backlog(hub_logs_backlog(), hub_live_backlog());
        if (hub_resync_take()) hub_sync_reset();
        if ((int32_t)(now - next_ok) < 0) continue;                // backing off after a failure

        if (need_resolve) {
            if (!cfg.hub_url[0]) hub_discover(discovered, sizeof discovered);   // blocks up to 2 s
            hub_config_cached_get(cached, sizeof cached);
            src = hub_url_pick(cfg.hub_url, discovered, cached, HUB_DEFAULT_URL, base, sizeof base);
            hub_status_set_conn(base, src);
            ESP_LOGI(TAG, "hub %s (%s)", base, hub_urlsrc_name(src));
            need_resolve = false;
        }
        if (!registered || (int32_t)(now - next_register) >= 0) {
            bool clock_set = false;
            if (hub_register(base, &clock_set)) {
                registered = true; fails = 0; backoff = 0;
                next_register = now + REGISTER_MS;
                if (src != HUB_URLSRC_EXPLICIT) hub_config_cached_set(base);
                if (clock_set || (int32_t)(now - next_epoch) >= 0) { hub_push_epoch_to_p4(); next_epoch = now + EPOCH_MS; }
                if (clock_set) hub_sync_reset();                   // runs created before the clock was set can be dated now
                hub_status_note_push(true, (uint32_t)(hub_wall_ms() / 1000), NULL);
            } else {
                registered = false;
                backoff = hub_backoff_next(backoff);
                next_ok = now + backoff;
                hub_status_note_push(false, 0, "hub unreachable");
                if (++fails >= MAX_FAILS && !cfg.hub_url[0]) { need_resolve = true; discovered[0] = '\0'; fails = 0; }   // moved? browse again
            }
            continue;
        }
        hub_step_t r = hub_logs_ship(base, false);
        if (r == HUB_STEP_IDLE) r = hub_live_tick(base);
        if (r == HUB_STEP_IDLE) r = hub_sync_step(base);
        if (r == HUB_STEP_RETRY) { backoff = hub_backoff_next(backoff); next_ok = now + backoff; }
        else backoff = 0;
    }
}

void hub_start(void)
{
    hub_logs_init();
    if (!hub_net_init() || !hub_live_init() || !hub_sync_init()) {
        ESP_LOGE(TAG, "out of PSRAM: hub streaming disabled");
        return;
    }
    // Internal 6 KB stack on purpose: the task writes NVS (flash), which a PSRAM stack must not do.
    // Created after httpd_start so the web server keeps its internal-heap headroom.
    xTaskCreatePinnedToCore(hub_task, "hub", 6144, NULL, 3, NULL, tskNO_AFFINITY);
}
