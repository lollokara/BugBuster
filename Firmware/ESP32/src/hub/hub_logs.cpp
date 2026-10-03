// =============================================================================
// hub_logs.cpp - see hub_logs.h.
// =============================================================================

#include "hub_logs.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "hat.h"
#include "hub_json.h"
#include "hub_policy.h"
#include "hub_ratelimit.h"
#include "hub_ring.h"
#include "scripting.h"

#define RING_BYTES      (64u * 1024u)
#define BATCH_MAX       64u
#define SHIP_BYTES      4096u
#define SHIP_PERIOD_MS  5000u
#define P4_PULL_MS      5000u
#define REC_MAX         (9u + HUB_LOG_TAG_MAX + HUB_LOG_MSG_MAX)

static hub_ring_t s_ring;
static SemaphoreHandle_t s_lock;
static vprintf_like_t s_prev_vprintf;
static volatile char s_level = 'W';
static char s_line[192];                    // esp_log line scratch, used only under s_lock
static uint8_t s_rec[REC_MAX];              // packed record scratch, used only under s_lock
static volatile uint32_t s_hook_dropped;
static uint32_t s_reported_overflow;
static uint32_t s_last_ship_ms;
static uint32_t s_p4_seq, s_p4_last_now, s_last_p4_pull_ms;
static hub_ratelimit_t *s_rl;

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

// Caller holds s_lock.
static void push_locked(uint32_t ms, uint8_t src, char level, const char *tag, const char *msg, size_t len)
{
    size_t n = hub_log_pack(s_rec, sizeof s_rec, ms, src, level, tag, msg, len);
    if (n) hub_ring_push(&s_ring, s_rec, (uint16_t)n);
}

static void ratelimit_emit_cb(void *user, uint32_t ms, uint8_t src, char level,
                              const char *tag, const char *msg, size_t len)
{
    (void)user;
    push_locked(ms, src, level, tag, msg, len);
}

// Caller holds s_lock.
static void push_rate_limited_locked(uint32_t ms, uint8_t src, char level, const char *tag, const char *msg, size_t len)
{
    if (s_rl) {
        hub_ratelimit_flush_expired(s_rl, ms, ratelimit_emit_cb, NULL);
        if (!hub_ratelimit_filter(s_rl, ms, src, level, tag, msg, len)) {
            return;
        }
    }
    push_locked(ms, src, level, tag, msg, len);
}

void hub_logs_push(uint8_t src, char level, const char *tag, const char *msg, size_t len)
{
    if (!s_lock) return;
    if (src == HUB_LOGSRC_S3 && !hub_level_enabled(level, s_level)) return;
    if (xSemaphoreTake(s_lock, 0) != pdTRUE) { s_hook_dropped = s_hook_dropped + 1; return; }
    push_rate_limited_locked(hub_uptime_ms(), src, level, tag, msg, len);
    xSemaphoreGive(s_lock);
}

static int hub_vprintf(const char *fmt, va_list ap)
{
    va_list copy;
    va_copy(copy, ap);
    int n = s_prev_vprintf ? s_prev_vprintf(fmt, ap) : vprintf(fmt, ap);
    if (s_lock && xSemaphoreTake(s_lock, 0) == pdTRUE) {
        vsnprintf(s_line, sizeof s_line, fmt, copy);
        char level, tag[HUB_LOG_TAG_MAX + 1];
        const char *msg;
        size_t ml;
        if (hub_log_parse_esp(s_line, &level, tag, sizeof tag, &msg, &ml) && hub_level_enabled(level, s_level))
            push_rate_limited_locked(hub_uptime_ms(), HUB_LOGSRC_S3, level, tag, msg, ml);
        xSemaphoreGive(s_lock);
    } else if (s_lock) {
        s_hook_dropped = s_hook_dropped + 1;
    }
    va_end(copy);
    return n;
}

// MicroPython output: "<ts_ms> <L> <src> <text>\n" (script_runtime.h). Runs under the script log mutex: never block.
static void mpy_tee(const char *line, size_t len)
{
    char level;
    const char *src, *msg;
    size_t sl, ml;
    if (hub_log_parse_mpy(line, len, &level, &src, &sl, &msg, &ml) && ml) {
        hub_logs_push(HUB_LOGSRC_MPY, level, "script", msg, ml);
    }
}

bool hub_logs_init(void)
{
    if (s_lock) return true;
    uint8_t *buf = (uint8_t *)heap_caps_malloc(RING_BYTES, MALLOC_CAP_SPIRAM);
    if (!buf) return false;
    SemaphoreHandle_t lock = xSemaphoreCreateMutex();
    if (!lock) { heap_caps_free(buf); return false; }
    hub_ring_init(&s_ring, buf, RING_BYTES);
    hub_ratelimit_t *rl = (hub_ratelimit_t *)heap_caps_malloc(sizeof(hub_ratelimit_t), MALLOC_CAP_SPIRAM);
    if (rl) {
        hub_ratelimit_init(rl, HUB_RATELIMIT_DEFAULT_BURST, HUB_RATELIMIT_DEFAULT_WINDOW_MS);
        s_rl = rl;
    }
    s_prev_vprintf = esp_log_set_vprintf(hub_vprintf);
    s_lock = lock;                           // set last: the hook and the tee are live from here
    scripting_set_log_tee(mpy_tee);
    return true;
}

void hub_logs_set_level(char level)
{
    if (hub_level_enabled(level, 'D')) s_level = level;
}

uint32_t hub_logs_backlog(void) { return s_ring.count; }

void hub_logs_pull_p4(void)
{
    uint32_t now = hub_uptime_ms();
    if (!s_lock || !hub_pace_due(now, s_last_p4_pull_ms, P4_PULL_MS)) return;
    s_last_p4_pull_ms = now;
    uint8_t buf[240];
    int n = hat_log_pull(s_p4_seq, buf, sizeof buf, 400);
    if (n == HAT_ERR_LOCK_BUSY) {
        // HAT link busy: back off and retry in 1000 ms instead of waiting a full 5 s period
        s_last_p4_pull_ms = now - P4_PULL_MS + 1000u;
        return;
    }
    if (n < 14) return;                                   // no DAQ HAT, timeout or nothing to say
    uint32_t p4_now = rd32(buf), first = rd32(buf + 4), dropped = rd32(buf + 8);
    uint8_t cnt = buf[12];
    if (p4_now < s_p4_last_now) {                         // the P4 rebooted: its sequence restarted
        s_p4_seq = 0;
        s_p4_last_now = p4_now;
        hub_logs_push(HUB_LOGSRC_S3, 'W', "hub", "DAQ HAT rebooted", 16);
        return;
    }
    s_p4_last_now = p4_now;
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(20)) != pdTRUE) return;
    size_t off = 14;
    uint8_t taken = 0;
    for (uint8_t k = 0; k < cnt; k++) {
        if (off + 7 > (size_t)n) break;
        uint32_t ms = rd32(buf + off);
        char level = (char)buf[off + 4];
        size_t tl = buf[off + 5], ml = buf[off + 6];
        if (off + 7 + tl + ml > (size_t)n) break;
        char tag[HUB_LOG_TAG_MAX + 1];
        if (tl > HUB_LOG_TAG_MAX) tl = HUB_LOG_TAG_MAX;
        memcpy(tag, buf + off + 7, tl);
        tag[tl] = '\0';
        push_locked(now - (p4_now - ms), HUB_LOGSRC_P4, level, tag, (const char *)buf + off + 7 + buf[off + 5], ml);
        off += 7 + buf[off + 5] + ml;
        taken++;
    }
    if (dropped) {
        char m[48];
        int l = snprintf(m, sizeof m, "P4 log ring overflowed: %u records lost", (unsigned)dropped);
        push_locked(now, HUB_LOGSRC_S3, 'W', "hub", m, (size_t)l);
    }
    xSemaphoreGive(s_lock);
    if (taken) s_p4_seq = first + taken - 1;
}

hub_step_t hub_logs_ship(const char *base, bool force)
{
    if (!s_lock || !hub_clock_valid()) return HUB_STEP_IDLE;
    uint32_t now = hub_uptime_ms();
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(50)) != pdTRUE) return HUB_STEP_IDLE;
    // Say once when the ring overwrote entries or producers had to drop.
    uint32_t over = s_ring.dropped - s_reported_overflow;
    if (over || s_hook_dropped) {
        char m[64];
        int l = snprintf(m, sizeof m, "log shipper dropped %u entries (ring) %u (busy)", (unsigned)over, (unsigned)s_hook_dropped);
        s_reported_overflow = s_ring.dropped;
        s_hook_dropped = 0;
        push_locked(now, HUB_LOGSRC_S3, 'W', "hub", m, (size_t)l);
    }
    if (s_rl) {
        hub_ratelimit_flush_expired(s_rl, now, ratelimit_emit_cb, NULL);
    }
    bool due = s_ring.count && (force || s_ring.used >= SHIP_BYTES || now - s_last_ship_ms >= SHIP_PERIOD_MS);
    if (!due) { xSemaphoreGive(s_lock); return HUB_STEP_IDLE; }
    s_last_ship_ms = now;

    char *body = hub_net_body();
    hub_jw_t w;
    hub_jw_init(&w, body, 8192);
    hub_jw_raw(&w, "[");
    uint64_t wall = hub_wall_ms();
    uint32_t d0 = s_ring.dropped, n = 0;
    for (uint32_t k = 0; k < s_ring.count && n < BATCH_MAX; k++) {
        int len = hub_ring_peek(&s_ring, k, s_rec, sizeof s_rec);
        hub_log_rec_t r;
        if (len < 0 || !hub_log_unpack(s_rec, (uint16_t)len, &r)) break;
        size_t mark = w.len;
        if (n) hub_jw_raw(&w, ",");
        if (!hub_json_log_entry(&w, wall - (uint32_t)(now - r.ms), &r)) { w.len = mark; w.p[mark] = '\0'; w.over = false; break; }
        n++;
    }
    xSemaphoreGive(s_lock);
    if (!n) return HUB_STEP_IDLE;
    hub_jw_raw(&w, "]");

    size_t rcap;
    char *resp = hub_net_resp(&rcap);
    int st = 0;
    bool got = hub_http("POST", base, "/api/v1/ingest/logs", hub_device_id(), body, w.len, resp, rcap, &st);
    hub_res_t res = hub_classify_status(got, st);
    if (res == HUB_RES_RETRY) {
        hub_status_note_push(false, 0, got ? "logs: hub error" : "hub unreachable");
        return HUB_STEP_RETRY;
    }
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(50)) == pdTRUE) {
        uint32_t lost = s_ring.dropped - d0;               // overwritten while the request was in flight
        hub_ring_drop(&s_ring, n > lost ? n - lost : 0);
        xSemaphoreGive(s_lock);
    }
    if (res == HUB_RES_OK) hub_status_note_push(true, (uint32_t)(hub_wall_ms() / 1000), NULL);
    else hub_status_note_push(false, 0, "logs rejected by the hub");   // 4xx: the batch can never succeed, it was dropped
    return s_ring.count >= BATCH_MAX ? HUB_STEP_MORE : HUB_STEP_IDLE;
}
