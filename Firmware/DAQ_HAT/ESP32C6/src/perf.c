#include "perf.h"

#if DISP_PERF_LOG

#include <string.h>
#include <inttypes.h>
#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "perf";

#ifndef DISP_PERF_PERIOD
#define DISP_PERF_PERIOD 60   // log averages every N frames
#endif

#define PERF_MAX_BUCKETS 12

typedef struct {
    const char *name;
    uint64_t    sum_us;   // accumulated time across the window
    uint32_t    last_us;  // last frame's value (for spotting spikes)
} bucket_t;

static bucket_t s_buckets[PERF_MAX_BUCKETS];
static int      s_nbuckets = 0;
static int64_t  s_mark_t = 0;     // timestamp of previous mark
static int64_t  s_frame_t = 0;    // timestamp of frame begin
static uint64_t s_frame_sum_us = 0;
static uint32_t s_frames = 0;

static bucket_t *find_bucket(const char *name)
{
    for (int i = 0; i < s_nbuckets; i++)
        if (s_buckets[i].name == name || strcmp(s_buckets[i].name, name) == 0)
            return &s_buckets[i];
    if (s_nbuckets < PERF_MAX_BUCKETS) {
        bucket_t *b = &s_buckets[s_nbuckets++];
        b->name = name;
        b->sum_us = 0;
        b->last_us = 0;
        return b;
    }
    return NULL;
}

void perf_frame_begin(void)
{
    s_frame_t = esp_timer_get_time();
    s_mark_t = s_frame_t;
}

void perf_mark(const char *name)
{
    int64_t now = esp_timer_get_time();
    uint32_t dt = (uint32_t)(now - s_mark_t);
    s_mark_t = now;
    bucket_t *b = find_bucket(name);
    if (b) { b->sum_us += dt; b->last_us = dt; }
}

void perf_frame_end(void)
{
    int64_t now = esp_timer_get_time();
    s_frame_sum_us += (uint64_t)(now - s_frame_t);
    s_frames++;

    if (s_frames < DISP_PERF_PERIOD) return;

    uint32_t n = s_frames;
    uint32_t frame_avg = (uint32_t)(s_frame_sum_us / n);
    float fps = frame_avg ? 1000000.0f / (float)frame_avg : 0.0f;

    ESP_LOGI(TAG, "---- frame avg %" PRIu32 " us (%.1f fps) over %" PRIu32 " frames ----",
             frame_avg, fps, n);
    for (int i = 0; i < s_nbuckets; i++) {
        uint32_t avg = (uint32_t)(s_buckets[i].sum_us / n);
        uint32_t pct = frame_avg ? (uint32_t)(s_buckets[i].sum_us * 100 / (s_frame_sum_us)) : 0;
        ESP_LOGI(TAG, "  %-10s %6" PRIu32 " us  (%2" PRIu32 "%%)  last=%" PRIu32 " us",
                 s_buckets[i].name, avg, pct, s_buckets[i].last_us);
        s_buckets[i].sum_us = 0;
    }

    s_frame_sum_us = 0;
    s_frames = 0;
}

#endif // DISP_PERF_LOG
