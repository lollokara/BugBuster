// =============================================================================
// battsim_host.c - see battsim_host.h.
// =============================================================================

#include "battsim_host.h"

#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "battsim.h"
#include "battsim_store.h"
#include "battsim_s1.h"

static const char *TAG = "bs_host";

_Static_assert(sizeof(battsim_status_t) <= 240, "status must fit one S3-link reply");

#define REQ_MAX       16
#define RSP_MAX       240
#define WAIT_MS       400
#define DIR_FIXED     5          // META, CKPT, EVENTS, Q15, S1
#define M1_DAYS_MAX   400

static TaskHandle_t      s_task;
static SemaphoreHandle_t s_done;
static volatile bool     s_busy;
static uint32_t          s_job, s_done_job;
static uint8_t           s_req[REQ_MAX], s_req_len;
static uint8_t           s_rsp[RSP_MAX];
static int               s_rsp_n;

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static void wr16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void wr32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

static int op_list_runs(const uint8_t *a, uint8_t n, uint8_t *out)
{
    if (n < 2) return -1;
    uint16_t start = rd16(a);
    int total = bs_store_list_runs(NULL, 0);
    uint16_t *ids = NULL;
    if (total > 0) {
        ids = malloc(sizeof(uint16_t) * (size_t)total);
        if (!ids) return -1;
        total = bs_store_list_runs(ids, total);
    }
    uint16_t active = 0;
    if (!bs_store_active_get(&active)) active = 0;
    wr16(out, (uint16_t)total);
    wr16(out + 2, active);
    int len = 4;
    for (int i = start; i < total && (len - 4) / 2 < (int)BS_HOST_IDS_MAX; i++) {
        wr16(out + len, ids[i]);
        len += 2;
    }
    free(ids);
    return len;
}

static int op_run_dir(const uint8_t *a, uint8_t n, uint8_t *out)
{
    if (n < 3) return -1;
    uint16_t run = rd16(a);
    uint8_t start = a[2];
    bs_run_meta_t meta;
    if (!bs_store_meta_read(run, &meta)) return -1;

    uint16_t *ids = malloc(sizeof(uint16_t) * (DIR_FIXED + M1_DAYS_MAX));
    if (!ids) return -1;
    int cnt = 0;
    static const uint16_t fixed[DIR_FIXED] = {
        BS_FILE_META, BS_FILE_CKPT, BS_FILE_EVENTS, BS_FILE_Q15, BS_FILE_S1,
    };
    for (int i = 0; i < DIR_FIXED; i++) ids[cnt++] = fixed[i];
    uint16_t *days = &ids[cnt];
    int nd = bs_store_m1_days(run, days, M1_DAYS_MAX);
    for (int i = 0; i < nd; i++) days[i] = (uint16_t)(BS_FILE_M1_BASE + days[i]);
    cnt += nd;

    // Only files that exist are listed, so `total` counts present files.
    int len = 4, present = 0;
    for (int i = 0; i < cnt; i++) {
        int32_t sz = bs_store_file_size(run, ids[i]);
        if (sz < 0) continue;
        if (present >= start && (len - 4) / 6 < (int)BS_HOST_DIR_MAX) {
            wr16(out + len, ids[i]);
            wr32(out + len + 2, (uint32_t)sz);
            len += 6;
        }
        present++;
    }
    free(ids);
    wr16(out, run);
    out[2] = (uint8_t)(present > 255 ? 255 : present);
    out[3] = start;
    return len;
}

// Hosts read files sequentially in 236 B chunks; one LittleFS open per chunk
// dominated the transfer, so each open prefetches READ_CACHE bytes.
#define READ_CACHE     8192
#define READ_CACHE_MS  2000

static uint8_t   *s_rc;
static uint16_t   s_rc_run, s_rc_file;
static uint32_t   s_rc_off, s_rc_len;
static TickType_t s_rc_at;

static int op_read(const uint8_t *a, uint8_t n, uint8_t *out)
{
    if (n < 9) return -1;
    uint16_t run = rd16(a), file = rd16(a + 2);
    uint32_t off = rd32(a + 4);
    uint32_t want = a[8];
    if (want > BS_HOST_READ_MAX) want = BS_HOST_READ_MAX;
    if (!s_rc) s_rc = heap_caps_malloc(READ_CACHE, MALLOC_CAP_SPIRAM);
    if (!s_rc) {
        int32_t got = bs_store_file_read(run, file, off, out, want);
        return got < 0 ? -1 : (int)got;
    }
    bool hit = s_rc_len > 0 && run == s_rc_run && file == s_rc_file &&
               off >= s_rc_off && off <= s_rc_off + s_rc_len &&
               (off + want <= s_rc_off + s_rc_len || s_rc_len < READ_CACHE) &&
               (xTaskGetTickCount() - s_rc_at) < pdMS_TO_TICKS(READ_CACHE_MS);
    if (!hit) {
        int32_t got = bs_store_file_read(run, file, off, s_rc, READ_CACHE);
        if (got < 0) { s_rc_len = 0; return -1; }
        s_rc_run = run; s_rc_file = file; s_rc_off = off; s_rc_len = (uint32_t)got;
        s_rc_at = xTaskGetTickCount();
    }
    uint32_t rel = off - s_rc_off;
    uint32_t avail = s_rc_len - rel;
    uint32_t n_out = want < avail ? want : avail;
    memcpy(out, s_rc + rel, n_out);
    return (int)n_out;
}

static int op_profile(const uint8_t *a, uint8_t n, uint8_t *out)
{
    if (n < 1) return -1;
    char name[BS_NAME_LEN];
    bs_params_t p;
    if (!bs_store_profile_load(a[0], name, &p)) return -1;
    memcpy(out, name, BS_NAME_LEN);
    memcpy(out + BS_NAME_LEN, &p, sizeof(p));
    return BS_NAME_LEN + (int)sizeof(p);
}

static int op_s1_since(const uint8_t *a, uint8_t n, uint8_t *out)
{
    if (n < 7) return -1;
    uint16_t run = rd16(a);
    uint32_t since = rd32(a + 2);
    int max = a[6];
    if (max <= 0 || max > (int)BS_S1_SINCE_MAX) max = (int)BS_S1_SINCE_MAX;
    bs_s1_sample_t recs[BS_S1_SINCE_MAX];
    bool more = false;
    int got = battsim_s1_since(run, since, recs, max, &more);
    if (got < 0) return -1;
    wr16(out, run);
    out[2] = (uint8_t)got;
    out[3] = more ? 1 : 0;
    memcpy(out + 4, recs, (size_t)got * sizeof(bs_s1_sample_t));
    return 4 + got * (int)sizeof(bs_s1_sample_t);
}

static int serve(const uint8_t *req, uint8_t len, uint8_t *out)
{
    if (len < 1) return -1;
    const uint8_t *a = req + 1;
    uint8_t n = (uint8_t)(len - 1);
    switch (req[0]) {
    case BS_HOP_STATUS: {
        battsim_status_t st;
        battsim_get_status(&st);
        memcpy(out, &st, sizeof(st));
        return (int)sizeof(st);
    }
    case BS_HOP_LIST_RUNS: return op_list_runs(a, n, out);
    case BS_HOP_RUN_DIR:   return op_run_dir(a, n, out);
    case BS_HOP_READ:      return op_read(a, n, out);
    case BS_HOP_PROFILE:   return op_profile(a, n, out);
    case BS_HOP_S1_SINCE:  return op_s1_since(a, n, out);
    case BS_HOP_SET_EPOCH:
        if (n < 4) return -1;
        battsim_set_epoch(rd32(a));
        return 0;
    default:
        return -1;
    }
}

static void host_task(void *arg)
{
    (void)arg;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        uint32_t job = s_job;
        s_rsp_n = serve(s_req, s_req_len, s_rsp);
        s_done_job = job;
        s_busy = false;
        xSemaphoreGive(s_done);
    }
}

int battsim_host_handle(const uint8_t *req, uint8_t len, uint8_t *resp, size_t cap)
{
    if (!req || len == 0 || len > REQ_MAX || cap < RSP_MAX) return -1;
    if (!s_task) {
        s_done = xSemaphoreCreateBinary();
        if (!s_done) return -1;
        // Core 1 is busy-polled by ADAQ capture.
        if (xTaskCreatePinnedToCore(host_task, "bs_host", 6144, NULL, 3, &s_task, 0) != pdPASS) {
            ESP_LOGE(TAG, "worker create failed");
            s_task = NULL;
            return -1;
        }
    }
    // A request that timed out may still be running: never overwrite its input.
    TickType_t t0 = xTaskGetTickCount();
    while (s_busy) {
        if ((xTaskGetTickCount() - t0) > pdMS_TO_TICKS(WAIT_MS)) return -1;
        vTaskDelay(pdMS_TO_TICKS(2));
    }
    memcpy(s_req, req, len);
    s_req_len = len;
    uint32_t job = ++s_job;
    s_busy = true;
    xTaskNotifyGive(s_task);

    TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(WAIT_MS);
    for (;;) {
        TickType_t now = xTaskGetTickCount();
        if (now >= deadline) return -1;
        if (xSemaphoreTake(s_done, deadline - now) != pdTRUE) return -1;
        if (s_done_job == job) break;   // a stale completion from a timed-out job
    }
    if (s_rsp_n < 0) return -1;
    memcpy(resp, s_rsp, (size_t)s_rsp_n);
    return s_rsp_n;
}
