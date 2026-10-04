#include "hub_health.h"

#include <stdio.h>
#include <string.h>

#include "hub_json.h"

static void pool(hub_jw_t *w, const char *key, const hub_health_pool_t *p)
{
    hub_jw_fmt(w, "\"%s\":{\"free\":%u,\"min\":%u,\"big\":%u}", key, (unsigned)p->free, (unsigned)p->min_free,
               (unsigned)p->largest);
}

static void str(hub_jw_t *w, const char *key, const char *v)
{
    hub_jw_fmt(w, ",\"%s\":", key);
    hub_jw_str(w, v, strlen(v));
}

size_t hub_health_json(char *out, size_t cap, const hub_health_t *h)
{
    hub_jw_t w;
    hub_jw_init(&w, out, cap);
    hub_jw_fmt(&w, "{\"kind\":\"health\",\"v\":%d,\"seq\":%u,\"trig\":\"%s\",\"ts\":%u,\"up_ms\":%u",
               HUB_HEALTH_VERSION, (unsigned)h->seq, h->first ? "boot" : "hourly", (unsigned)h->epoch_s, (unsigned)h->up_ms);
    str(&w, "dev", h->dev);
    str(&w, "fw", h->fw);
    str(&w, "elf", h->elf);
    str(&w, "idf", h->idf);
    hub_jw_fmt(&w, ",\"boot\":%u,\"reset\":{\"reason\":", (unsigned)h->boot);
    hub_jw_str(&w, h->reset_reason, strlen(h->reset_reason));
    hub_jw_fmt(&w, ",\"code\":%d,\"abnormal\":%s},\"heap\":{", h->reset_code, h->reset_abnormal ? "true" : "false");
    pool(&w, "int", &h->heap_int);
    hub_jw_raw(&w, ",");
    pool(&w, "psram", &h->heap_psram);
    hub_jw_raw(&w, "},\"stacks\":{");
    unsigned n = h->n_tasks > HUB_HEALTH_MAX_TASKS ? HUB_HEALTH_MAX_TASKS : h->n_tasks;
    for (unsigned k = 0; k < n; k++) {
        if (k) hub_jw_raw(&w, ",");
        hub_jw_str(&w, h->tasks[k].name, strnlen(h->tasks[k].name, HUB_HEALTH_NAME_MAX));
        if (h->tasks[k].min_free < 0) hub_jw_raw(&w, ":null");
        else hub_jw_fmt(&w, ":%d", (int)h->tasks[k].min_free);
    }
    hub_jw_fmt(&w, "},\"cnt\":{\"coredump\":%d,\"hat_to\":%u,\"hat_streak\":%u,\"hat_degraded\":%d,"
                   "\"hub_fail\":%u,\"log_drop\":%u,\"script_drop\":%u,\"wifi_reconn\":%u}}",
               h->coredump ? 1 : 0, (unsigned)h->hat_timeouts, (unsigned)h->hat_streak, h->hat_degraded ? 1 : 0,
               (unsigned)h->hub_fail, (unsigned)h->log_drop, (unsigned)h->script_drop, (unsigned)h->wifi_reconn);
    return w.over ? 0 : w.len;
}

size_t hub_health_line(char *out, size_t cap, uint32_t boot, uint32_t seq, const char *json, size_t json_len,
                       size_t i, size_t *n_parts)
{
    size_t parts = json_len ? (json_len + HUB_HEALTH_PART_MAX - 1) / HUB_HEALTH_PART_MAX : 1;
    if (n_parts) *n_parts = parts;
    if (i >= parts) return 0;
    size_t off = i * HUB_HEALTH_PART_MAX;
    size_t n = json_len - off < HUB_HEALTH_PART_MAX ? json_len - off : HUB_HEALTH_PART_MAX;
    int pre = snprintf(out, cap, "HEALTH %u %u %u/%u ", (unsigned)boot, (unsigned)seq, (unsigned)(i + 1), (unsigned)parts);
    if (pre < 0 || (size_t)pre + n + 1 > cap) return 0;
    memcpy(out + pre, json + off, n);
    out[pre + n] = '\0';
    return (size_t)pre + n;
}

bool hub_health_sched_due(hub_health_sched_t *s, uint32_t now_ms, bool link_ready)
{
    if (!s->armed) {
        if (link_ready) { s->armed = true; s->next_ms = now_ms + HUB_HEALTH_FIRST_DELAY_MS; }
        else if (now_ms >= HUB_HEALTH_ARM_FALLBACK_MS) { s->armed = true; s->next_ms = now_ms; }
        else return false;
    }
    if ((int32_t)(now_ms - s->next_ms) < 0) return false;
    s->next_ms = now_ms + HUB_HEALTH_INTERVAL_MS;
    return true;
}
