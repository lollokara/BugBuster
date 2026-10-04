// =============================================================================
// hub_health_glue.cpp - gathers the live numbers for the HEALTH record (hub_health.h,
// Docs/hub-health-record.md) and queues it as chunked "HEALTH ..." log lines.
// Called only from the hub task, between steps.
// =============================================================================

#include "hub_health_glue.h"

#include <stdio.h>
#include <string.h>

#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "crash_report.h"
#include "hat.h"
#include "hub_health.h"
#include "hub_logs.h"
#include "hub_net.h"
#include "scripting.h"
#include "tasks.h"
#include "wifi_manager.h"

static const char *TAG = "health";
static uint32_t s_seq;

static void pool(hub_health_pool_t *p, uint32_t caps)
{
    p->free = (uint32_t)heap_caps_get_free_size(caps);
    p->min_free = (uint32_t)heap_caps_get_minimum_free_size(caps);
    p->largest = (uint32_t)heap_caps_get_largest_free_block(caps);
}

static void collect(hub_health_t *h, bool first)
{
    memset(h, 0, sizeof *h);
    h->seq = ++s_seq;
    h->first = first;
    h->epoch_s = hub_clock_valid() ? (uint32_t)(hub_wall_ms() / 1000) : 0;
    h->up_ms = (uint32_t)(esp_timer_get_time() / 1000LL);
    snprintf(h->dev, sizeof h->dev, "%s", hub_device_id());
    const esp_app_desc_t *d = esp_app_get_description();
    snprintf(h->fw, sizeof h->fw, "%s", d->version);
    snprintf(h->idf, sizeof h->idf, "%s", d->idf_ver);
    snprintf(h->elf, sizeof h->elf, "%.16s", esp_app_get_elf_sha256_str());
    const char *reason = "";
    crash_report_boot_info(&h->boot, &reason, &h->reset_code, &h->reset_abnormal);
    snprintf(h->reset_reason, sizeof h->reset_reason, "%.23s", reason);
    pool(&h->heap_int, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    pool(&h->heap_psram, MALLOC_CAP_SPIRAM);

    BbTaskInfo t[BB_TASK_REGISTRY_MAX];
    size_t n = tasks_get_registry(t, BB_TASK_REGISTRY_MAX);
    for (size_t i = 0; i < n && h->n_tasks < HUB_HEALTH_MAX_TASKS; i++) {
        hub_health_task_t *o = &h->tasks[h->n_tasks++];
        snprintf(o->name, sizeof o->name, "%s", t[i].name);
        o->min_free = t[i].handle ? (int32_t)t[i].hwm_bytes : -1;
    }
    TaskHandle_t self = xTaskGetCurrentTaskHandle();     // this is the "hub" task
    if (self && h->n_tasks < HUB_HEALTH_MAX_TASKS) {
        hub_health_task_t *o = &h->tasks[h->n_tasks++];
        snprintf(o->name, sizeof o->name, "hub");
        o->min_free = (int32_t)uxTaskGetStackHighWaterMark(self);
    }

    h->coredump = crash_report_dump_size() > 0;
    const HatState *hs = hat_get_state();
    h->hat_timeouts = hat_timeout_total();
    h->hat_streak = hs->consecutive_timeouts;
    h->hat_degraded = hs->degraded;
    h->hub_fail = hub_http_fail_count();
    h->log_drop = hub_logs_dropped_total();
    h->script_drop = scripting_log_dropped_total();
    h->wifi_reconn = wifi_get_disconnect_count();
}

void hub_health_emit(bool first)
{
    hub_health_t h;
    collect(&h, first);
    char *json = hub_net_body();                         // shared PSRAM scratch: the hub task is its only user
    size_t jl = hub_health_json(json, HUB_HEALTH_JSON_MAX, &h);
    if (!jl) { ESP_LOGW(TAG, "health record does not fit %u bytes", (unsigned)HUB_HEALTH_JSON_MAX); return; }
    char line[HUB_HEALTH_PART_MAX + 48];
    size_t parts = 1;
    for (size_t i = 0; i < parts; i++) {
        size_t ll = hub_health_line(line, sizeof line, h.boot, h.seq, json, jl, i, &parts);
        if (!ll) return;
        hub_logs_push_health(line, ll);
    }
    ESP_LOGD(TAG, "health #%u queued (%u B, %u parts)", (unsigned)h.seq, (unsigned)jl, (unsigned)parts);
}
