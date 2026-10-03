// =============================================================================
// crash_report.cpp - boot diagnostics package, coredump access, breadcrumb.
// See crash_report.h for the overview.
//
// Memory notes:
//  * The breadcrumb is ~80 B of RTC NOINIT RAM.
//  * The decoded coredump summary (~0.6 KB) is cached once per boot in PSRAM.
//  * The check/emit work runs on a throw-away 8 KB task, created only when an
//    internal block that large exists and freed as soon as it finishes. Its stack
//    has to be internal RAM: it reads flash, and with the cache disabled
//    (esp_task_stack_is_sane_cache_disabled) a PSRAM stack would panic.
//  * HTTP/BLE requests only use the cached summary and read flash in <= 768 B
//    slices, so they add no flash-wide scan on the small httpd stack.
// =============================================================================

#include "crash_report.h"
#include "crash_util.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "esp_log.h"
#include "esp_attr.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_mac.h"
#include "esp_chip_info.h"
#include "esp_app_desc.h"
#include "esp_ota_ops.h"
#include "esp_core_dump.h"
#include "esp_partition.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "cJSON.h"
#include "mbedtls/base64.h"
#include "tusb.h"

#include "config.h"
#include "tasks.h"
#include "hat.h"
#include "pca9535.h"
#include "wifi_manager.h"
#include "api_scripts.h"

static const char *TAG = "bootrpt";

#define CRUMB_PERIOD_MS     5000u
#define LINE_PAYLOAD_MAX    360u     /* bytes of JSON per log line */
#define WORKER_STACK_BYTES  8192u
#define WORKER_SPARE_BYTES  4096u    /* internal headroom to leave after the stack */
#define SPAWN_RETRY_MS      5000u
#define SPAWN_MAX_TRIES     6u
#define LOW_STACK_BYTES     512u

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------

static RTC_NOINIT_ATTR crash_crumb_t s_crumb;     // survives panic / WDT / soft resets
static crash_crumb_t s_prev;                      // copy taken at init
static bool          s_prev_valid;
static int           s_reset_reason;
static uint32_t      s_boot_count;
static uint32_t      s_streak;
static uint32_t      s_last_crumb_ms;

enum { ST_BOOTING, ST_LOAD, ST_SETTLE, ST_REPORT, ST_DONE };
static volatile int      s_state = ST_BOOTING;
static volatile bool     s_worker_busy;
static uint32_t          s_settle_deadline_ms;
static uint32_t          s_next_try_ms;
static uint32_t          s_tries;

// Coredump facts, filled by the worker (OP_LOAD) and read by API callers.
typedef struct {
    bool     loaded;
    bool     present;                 // a dump image exists in flash
    bool     valid;                   // ...and its checksum verifies
    esp_err_t check;
    size_t   addr, size;
    char     reason[96];
    bool     have_summary;
    esp_core_dump_summary_t *sum;     // PSRAM, NULL when unavailable
} DumpInfo;

static DumpInfo          s_dump;
static SemaphoreHandle_t s_lock;      // guards s_dump (incl. flash erase vs read)

enum { OP_LOAD, OP_REPORT };

// ---------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------

static void add_hex(cJSON *o, const char *key, uint32_t v)
{
    char b[12];
    snprintf(b, sizeof(b), "0x%08x", (unsigned)v);
    cJSON_AddStringToObject(o, key, b);
}

static char *err_json(const char *msg)
{
    cJSON *o = cJSON_CreateObject();
    if (!o) return NULL;
    cJSON_AddBoolToObject(o, "ok", false);
    cJSON_AddStringToObject(o, "error", msg);
    char *s = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    return s;
}

static char *take(cJSON *o)
{
    if (!o) return NULL;
    char *s = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    return s;
}

// Value of "key=" in the query part of `path`, matched at a '?'/'&' boundary so
// "xoffset=" never counts as "offset=". Returns false when absent or not a number.
static bool query_u32(const char *path, const char *key, uint32_t *out)
{
    const char *q = strchr(path, '?');
    size_t kl = strlen(key);
    while (q) {
        q++;
        if (strncmp(q, key, kl) == 0 && q[kl] == '=') {
            char *end = NULL;
            unsigned long v = strtoul(q + kl + 1, &end, 0);
            if (end == q + kl + 1) return false;
            *out = (uint32_t)v;
            return true;
        }
        q = strchr(q, '&');
    }
    return false;
}

static void min_stack(char *name, size_t name_sz, uint32_t *free_bytes)
{
    BbTaskInfo t[BB_TASK_REGISTRY_MAX];
    size_t n = tasks_get_registry(t, BB_TASK_REGISTRY_MAX);
    *free_bytes = 0xFFFFFFFFu;
    name[0] = '\0';
    for (size_t i = 0; i < n; i++) {
        if (!t[i].handle) continue;
        if (t[i].hwm_bytes < *free_bytes) {
            *free_bytes = t[i].hwm_bytes;
            snprintf(name, name_sz, "%s", t[i].name);
        }
    }
    if (*free_bytes == 0xFFFFFFFFu) *free_bytes = 0;
}

// ---------------------------------------------------------------------------
// Breadcrumb
// ---------------------------------------------------------------------------

static void crumb_write(uint8_t phase, uint32_t now_ms)
{
    crash_crumb_t c;
    memset(&c, 0, sizeof(c));
    c.boot_count   = s_boot_count;
    c.crash_streak = s_streak;
    c.uptime_ms    = now_ms;
    c.int_free     = (uint32_t)heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    c.int_min      = (uint32_t)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);
    c.int_largest  = (uint32_t)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
    c.psram_free   = (uint32_t)heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    c.psram_min    = (uint32_t)heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM);
    c.phase        = phase;
    if (s_state != ST_BOOTING) {   // registry/HAT/WiFi are only meaningful once up
        min_stack(c.min_stack_task, sizeof(c.min_stack_task), &c.min_stack_free);
        const HatState *hs = hat_get_state();
        if (wifi_is_connected())  c.flags |= CRASH_FLAG_WIFI_STA;
        if (hs && hs->connected)  c.flags |= CRASH_FLAG_HAT;
        if (tud_mounted())        c.flags |= CRASH_FLAG_USB;
    }
    crash_crumb_seal(&c);
    memcpy(&s_crumb, &c, sizeof(c));   // sealed copy goes in whole
}

static uint8_t s_phase = CRASH_PHASE_NONE;

void crash_report_phase(uint8_t phase)
{
    s_phase = phase;
    crumb_write(phase, (uint32_t)(esp_timer_get_time() / 1000LL));
}

void crash_report_init(void)
{
    s_reset_reason = (int)esp_reset_reason();
    s_prev_valid = (s_reset_reason != ESP_RST_POWERON) && crash_crumb_valid(&s_crumb);
    if (s_prev_valid) memcpy(&s_prev, &s_crumb, sizeof(s_prev));
    else              memset(&s_prev, 0, sizeof(s_prev));

    s_boot_count = s_prev_valid ? s_prev.boot_count + 1 : 1;
    s_streak = crash_reset_is_abnormal(s_reset_reason)
             ? (s_prev_valid ? s_prev.crash_streak : 0) + 1 : 0;

    s_lock = xSemaphoreCreateMutex();
    crash_report_phase(CRASH_PHASE_EARLY);
}

// ---------------------------------------------------------------------------
// Coredump facts
// ---------------------------------------------------------------------------

static void dump_info_load(void)
{
    DumpInfo d;
    memset(&d, 0, sizeof(d));
    d.check = esp_core_dump_image_check();
    d.present = (d.check != ESP_ERR_NOT_FOUND);
    d.valid = (d.check == ESP_OK);
    if (d.valid) {
        if (esp_core_dump_image_get(&d.addr, &d.size) != ESP_OK) {
            d.valid = false;
            d.size = 0;
        }
    }
#if CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH && CONFIG_ESP_COREDUMP_DATA_FORMAT_ELF
    if (d.valid) {
        esp_core_dump_get_panic_reason(d.reason, sizeof(d.reason));
        void *mem = heap_caps_calloc(1, sizeof(esp_core_dump_summary_t),
                                     MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!mem) mem = calloc(1, sizeof(esp_core_dump_summary_t));
        if (mem) {
            d.sum = (esp_core_dump_summary_t *)mem;
            if (esp_core_dump_get_summary(d.sum) == ESP_OK) {
                d.have_summary = true;
            } else {
                free(d.sum);
                d.sum = NULL;
            }
        }
    }
#endif
    d.loaded = true;

    xSemaphoreTake(s_lock, portMAX_DELAY);
    esp_core_dump_summary_t *old = s_dump.sum;
    s_dump = d;
    xSemaphoreGive(s_lock);
    free(old);
}

size_t crash_report_dump_size(void)
{
    size_t n = 0;
    if (s_lock && xSemaphoreTake(s_lock, pdMS_TO_TICKS(200)) == pdTRUE) {
        n = (s_dump.loaded && s_dump.valid) ? s_dump.size : 0;
        xSemaphoreGive(s_lock);
    }
    return n;
}

esp_err_t crash_report_dump_read(size_t offset, void *buf, size_t len)
{
    if (!s_lock || xSemaphoreTake(s_lock, pdMS_TO_TICKS(500)) != pdTRUE) return ESP_ERR_TIMEOUT;
    esp_err_t err = ESP_ERR_NOT_FOUND;
    if (s_dump.loaded && s_dump.valid && offset + len <= s_dump.size) {
        const esp_partition_t *part = esp_partition_find_first(
            ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_COREDUMP, "coredump");
        if (part && s_dump.addr >= part->address &&
            s_dump.addr + s_dump.size <= part->address + part->size) {
            err = esp_partition_read(part, (s_dump.addr - part->address) + offset, buf, len);
        } else {
            err = ESP_ERR_INVALID_STATE;
        }
    }
    xSemaphoreGive(s_lock);
    return err;
}

// ---------------------------------------------------------------------------
// JSON sections
// ---------------------------------------------------------------------------

static cJSON *crumb_json(const crash_crumb_t *c)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "boot", c->boot_count);
    cJSON_AddNumberToObject(o, "streak", c->crash_streak);
    cJSON_AddNumberToObject(o, "up_ms", c->uptime_ms);
    cJSON_AddStringToObject(o, "phase", crash_phase_name(c->phase));
    cJSON_AddNumberToObject(o, "int_free", c->int_free);
    cJSON_AddNumberToObject(o, "int_min", c->int_min);
    cJSON_AddNumberToObject(o, "int_largest", c->int_largest);
    cJSON_AddNumberToObject(o, "psram_free", c->psram_free);
    cJSON_AddNumberToObject(o, "psram_min", c->psram_min);
    cJSON_AddNumberToObject(o, "min_stack_free", c->min_stack_free);
    cJSON_AddStringToObject(o, "min_stack_task", c->min_stack_task);
    cJSON_AddBoolToObject(o, "wifi_sta", (c->flags & CRASH_FLAG_WIFI_STA) != 0);
    cJSON_AddBoolToObject(o, "hat", (c->flags & CRASH_FLAG_HAT) != 0);
    cJSON_AddBoolToObject(o, "usb", (c->flags & CRASH_FLAG_USB) != 0);
    return o;
}

// "reset" block shared by the sys section and the crash summary.
static cJSON *reset_json(void)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "reason", crash_reset_reason_name(s_reset_reason));
    cJSON_AddNumberToObject(o, "code", s_reset_reason);
    cJSON_AddBoolToObject(o, "abnormal", crash_reset_is_abnormal(s_reset_reason));
    cJSON_AddNumberToObject(o, "boot", s_boot_count);
    cJSON_AddNumberToObject(o, "streak", s_streak);
    return o;
}

static cJSON *sec_sys(void)
{
    cJSON *o = cJSON_CreateObject();
    const esp_app_desc_t *d = esp_app_get_description();
    cJSON_AddStringToObject(o, "fw", d->version);
    cJSON_AddStringToObject(o, "idf", d->idf_ver);
    char built[40];
    snprintf(built, sizeof(built), "%s %s", d->date, d->time);
    cJSON_AddStringToObject(o, "built", built);
    cJSON_AddStringToObject(o, "elf", esp_app_get_elf_sha256_str());

    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    char m[16];
    snprintf(m, sizeof(m), "%02x%02x%02x%02x%02x%02x", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    cJSON_AddStringToObject(o, "mac", m);

    const esp_partition_t *run = esp_ota_get_running_partition();
    if (run) {
        cJSON_AddStringToObject(o, "part", run->label);
        esp_ota_img_states_t st;
        if (esp_ota_get_state_partition(run, &st) == ESP_OK) {
            const char *n = st == ESP_OTA_IMG_NEW ? "new" : st == ESP_OTA_IMG_PENDING_VERIFY ? "pending"
                          : st == ESP_OTA_IMG_VALID ? "valid" : st == ESP_OTA_IMG_INVALID ? "invalid"
                          : st == ESP_OTA_IMG_ABORTED ? "aborted" : "undefined";
            cJSON_AddStringToObject(o, "ota", n);
        }
    }
    esp_chip_info_t ci;
    esp_chip_info(&ci);
    cJSON_AddNumberToObject(o, "chip_rev", ci.revision);
    cJSON_AddNumberToObject(o, "up_ms", (double)(esp_timer_get_time() / 1000LL));
    cJSON_AddItemToObject(o, "reset", reset_json());
    if (s_prev_valid) {
        cJSON_AddStringToObject(o, "prev_phase", crash_phase_name(s_prev.phase));
    }
    return o;
}

static void add_pool(cJSON *parent, const char *key, uint32_t caps, bool with_total)
{
    cJSON *p = cJSON_AddObjectToObject(parent, key);
    cJSON_AddNumberToObject(p, "free", (double)heap_caps_get_free_size(caps));
    cJSON_AddNumberToObject(p, "min", (double)heap_caps_get_minimum_free_size(caps));
    cJSON_AddNumberToObject(p, "largest", (double)heap_caps_get_largest_free_block(caps));
    if (with_total) cJSON_AddNumberToObject(p, "total", (double)heap_caps_get_total_size(caps));
}

static cJSON *sec_mem(void)
{
    cJSON *o = cJSON_CreateObject();
    add_pool(o, "int", MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT, true);
    add_pool(o, "dma", MALLOC_CAP_DMA, false);
    add_pool(o, "psram", MALLOC_CAP_SPIRAM, true);

    BbTaskInfo t[BB_TASK_REGISTRY_MAX];
    size_t n = tasks_get_registry(t, BB_TASK_REGISTRY_MAX);
    cJSON *arr = cJSON_AddArrayToObject(o, "tasks");
    int low = 0;
    for (size_t i = 0; i < n; i++) {
        cJSON *e = cJSON_CreateObject();
        cJSON_AddStringToObject(e, "n", t[i].name);
        cJSON_AddNumberToObject(e, "size", t[i].declared_bytes);
        cJSON_AddNumberToObject(e, "hwm", t[i].hwm_bytes);   // bytes never used
        if (!t[i].handle) {
            cJSON_AddBoolToObject(e, "run", false);
        } else if (t[i].hwm_bytes < LOW_STACK_BYTES) {
            cJSON_AddBoolToObject(e, "low", true);
            low++;
        }
        cJSON_AddItemToArray(arr, e);
    }
    cJSON_AddNumberToObject(o, "low_stacks", low);
    cJSON_AddNumberToObject(o, "up_ms", (double)(esp_timer_get_time() / 1000LL));
    return o;
}

static cJSON *sec_net(void)
{
    cJSON *o = cJSON_CreateObject();
    bool sta = wifi_is_connected();
    cJSON_AddBoolToObject(o, "sta", sta);
    if (sta) {
        cJSON_AddStringToObject(o, "ip", wifi_get_sta_ip());
        cJSON_AddNumberToObject(o, "rssi", wifi_get_rssi());
    }
    cJSON_AddStringToObject(o, "ap_ip", wifi_get_ap_ip());
    cJSON_AddBoolToObject(o, "usb", tud_mounted());
    return o;
}

static cJSON *sec_hw(void)
{
    cJSON *o = cJSON_CreateObject();
    const HatState *hs = hat_get_state();
    cJSON *h = cJSON_AddObjectToObject(o, "hat");
    cJSON_AddStringToObject(h, "type", hat_type_name(hs->type));
    cJSON_AddBoolToObject(h, "detected", hs->detected);
    cJSON_AddBoolToObject(h, "connected", hs->connected);
    cJSON_AddBoolToObject(h, "degraded", hs->degraded);
    cJSON_AddNumberToObject(h, "timeouts", hs->consecutive_timeouts);
    if (hs->connected) {
        char fw[12];
        snprintf(fw, sizeof(fw), "%d.%d", (int)hs->fw_version_major, (int)hs->fw_version_minor);
        cJSON_AddStringToObject(h, "fw", fw);
    }
    cJSON_AddBoolToObject(o, "pca", pca9535_present());
    if (g_stateMutex && xSemaphoreTake(g_stateMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        add_hex(o, "alert", g_deviceState.alertStatus);
        add_hex(o, "supply_alert", g_deviceState.supplyAlertStatus);
        xSemaphoreGive(g_stateMutex);
    }
    return o;
}

// Crash summary. `full` adds the register file (large) for the API / bundle.
static cJSON *sec_crash(bool full)
{
    cJSON *o = cJSON_CreateObject();
    xSemaphoreTake(s_lock, portMAX_DELAY);
    DumpInfo d = s_dump;                       // plain copy; `sum` stays valid until clear
    esp_core_dump_summary_t sum;
    bool have_sum = d.have_summary && d.sum;
    if (have_sum) memcpy(&sum, d.sum, sizeof(sum));
    xSemaphoreGive(s_lock);

    cJSON_AddBoolToObject(o, "ready", d.loaded);
    cJSON_AddBoolToObject(o, "present", d.present);
    cJSON_AddBoolToObject(o, "valid", d.valid);
    if (d.present && !d.valid) cJSON_AddStringToObject(o, "check", esp_err_to_name(d.check));
    cJSON_AddItemToObject(o, "reset", reset_json());

    if (d.valid) {
        cJSON_AddNumberToObject(o, "size", (double)d.size);
        cJSON_AddNumberToObject(o, "chunk_max", CRASH_CHUNK_MAX);
        if (d.reason[0]) cJSON_AddStringToObject(o, "panic", d.reason);
    }
#if CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH && CONFIG_ESP_COREDUMP_DATA_FORMAT_ELF
    if (have_sum) {
        cJSON_AddStringToObject(o, "task", sum.exc_task);
        add_hex(o, "tcb", sum.exc_tcb);
        add_hex(o, "pc", sum.exc_pc);
        cJSON_AddNumberToObject(o, "exccause", sum.ex_info.exc_cause);
        const char *cn = crash_exccause_name(sum.ex_info.exc_cause);
        if (cn) cJSON_AddStringToObject(o, "exccause_name", cn);
        add_hex(o, "vaddr", sum.ex_info.exc_vaddr);

        cJSON *bt = cJSON_AddArrayToObject(o, "bt");
        uint32_t depth = sum.exc_bt_info.depth > 16 ? 16 : sum.exc_bt_info.depth;
        for (uint32_t i = 0; i < depth; i++) {
            char b[12];
            snprintf(b, sizeof(b), "0x%08x", (unsigned)sum.exc_bt_info.bt[i]);
            cJSON_AddItemToArray(bt, cJSON_CreateString(b));
        }
        cJSON_AddBoolToObject(o, "bt_corrupt", sum.exc_bt_info.corrupted);

        // Does the stored dump belong to the firmware now running? If not, the
        // local ELF cannot symbolise it -- the sha says which build to fetch.
        const char *run_sha = esp_app_get_elf_sha256_str();
        cJSON_AddStringToObject(o, "dump_elf", (const char *)sum.app_elf_sha256);
        cJSON_AddBoolToObject(o, "elf_match",
                              strncmp((const char *)sum.app_elf_sha256, run_sha, strlen(run_sha)) == 0);
        if (full) {
            cJSON *a = cJSON_AddArrayToObject(o, "regs");   // a0..a15 at the exception
            for (int i = 0; i < 16; i++) {
                char b[12];
                snprintf(b, sizeof(b), "0x%08x", (unsigned)sum.ex_info.exc_a[i]);
                cJSON_AddItemToArray(a, cJSON_CreateString(b));
            }
        }
    }
#endif
    if (s_prev_valid) cJSON_AddItemToObject(o, "pre", crumb_json(&s_prev));
    return o;
}

// ---------------------------------------------------------------------------
// Boot report emission
// ---------------------------------------------------------------------------

static void emit(const char *sec, cJSON *obj, bool warn)
{
    char *s = cJSON_PrintUnformatted(obj);
    cJSON_Delete(obj);
    if (!s) return;
    size_t len = strlen(s);
    size_t parts = crash_part_count(len, LINE_PAYLOAD_MAX);
    for (size_t i = 0; i < parts; i++) {
        size_t off = i * LINE_PAYLOAD_MAX;
        size_t n = len - off < LINE_PAYLOAD_MAX ? len - off : LINE_PAYLOAD_MAX;
        if (warn) {
            ESP_LOGW(TAG, "BOOTRPT %u %s %u/%u %.*s", (unsigned)s_boot_count, sec,
                     (unsigned)(i + 1), (unsigned)parts, (int)n, s + off);
        } else {
            ESP_LOGI(TAG, "BOOTRPT %u %s %u/%u %.*s", (unsigned)s_boot_count, sec,
                     (unsigned)(i + 1), (unsigned)parts, (int)n, s + off);
        }
        vTaskDelay(pdMS_TO_TICKS(5));   // let the console / log shipper drain
    }
    cJSON_free(s);
}

static void emit_report(void)
{
    uint32_t t0 = (uint32_t)(esp_timer_get_time() / 1000LL);
    emit("sys", sec_sys(), false);
    emit("mem", sec_mem(), false);
    emit("net", sec_net(), false);
    emit("hw", sec_hw(), false);

    char *sc = api_scripts_status("/api/scripts/status", NULL);
    if (sc) {
        cJSON *j = cJSON_Parse(sc);
        cJSON_free(sc);
        if (j) emit("scripts", j, false);
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool crashed = s_dump.valid;
    xSemaphoreGive(s_lock);
    // A stored dump or an abnormal reset is worth a warning-level line; a clean
    // boot still gets a short crash line so the platform sees "none" explicitly.
    emit("crash", sec_crash(false), crashed || crash_reset_is_abnormal(s_reset_reason));

    ESP_LOGI(TAG, "BOOTRPT %u end %u/%u {\"ms\":%u}", (unsigned)s_boot_count, 1u, 1u,
             (unsigned)((uint32_t)(esp_timer_get_time() / 1000LL) - t0));
}

// ---------------------------------------------------------------------------
// Worker + scheduler
// ---------------------------------------------------------------------------

static void worker_task(void *arg)
{
    int op = (int)(intptr_t)arg;
    if (op == OP_LOAD) {
        dump_info_load();
        s_state = ST_SETTLE;
    } else {
        emit_report();
        s_state = ST_DONE;
    }
    s_worker_busy = false;
    vTaskDelete(NULL);
}

static void spawn_worker(int op, uint32_t now_ms)
{
    if (s_worker_busy || now_ms < s_next_try_ms) return;
    s_next_try_ms = now_ms + SPAWN_RETRY_MS;
    if (s_tries >= SPAWN_MAX_TRIES) {
        ESP_LOGW(TAG, "giving up on boot report step %d (no internal RAM for the worker)", op);
        s_state = ST_DONE;
        return;
    }
    s_tries++;
    if (heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) <
        WORKER_STACK_BYTES + WORKER_SPARE_BYTES) {
        return;   // retry on a later tick
    }
    s_worker_busy = true;
    if (xTaskCreate(worker_task, "bootrpt", WORKER_STACK_BYTES, (void *)(intptr_t)op, 1, NULL) != pdPASS) {
        s_worker_busy = false;
        return;
    }
    s_tries = 0;
}

void crash_report_boot_complete(void)
{
    crash_report_phase(CRASH_PHASE_RUNNING);
    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000LL);
    s_settle_deadline_ms = now + CRASH_REPORT_SETTLE_MS;
    s_state = ST_LOAD;
}

void crash_report_tick(uint32_t now_ms)
{
    if (s_state == ST_BOOTING) return;
    if (now_ms - s_last_crumb_ms >= CRUMB_PERIOD_MS) {
        s_last_crumb_ms = now_ms;
        crumb_write(s_phase, now_ms);
    }
    if (s_state == ST_LOAD) {
        spawn_worker(OP_LOAD, now_ms);
    } else if (s_state == ST_SETTLE && (int32_t)(now_ms - s_settle_deadline_ms) >= 0) {
        s_state = ST_REPORT;
    }
    if (s_state == ST_REPORT) {
        spawn_worker(OP_REPORT, now_ms);
    }
}

// ---------------------------------------------------------------------------
// API
// ---------------------------------------------------------------------------

static char *api_chunk(uint32_t offset, uint32_t len)
{
    size_t total = crash_report_dump_size();
    if (total == 0) return err_json("no coredump stored");
    size_t n = 0;
    if (!crash_clamp_range(total, offset, len, &n)) return err_json("offset/len out of range");

    uint8_t *raw = (uint8_t *)malloc(n);
    unsigned char *enc = (unsigned char *)malloc(((n + 2) / 3) * 4 + 1);
    char *out = NULL;
    if (raw && enc) {
        esp_err_t err = crash_report_dump_read(offset, raw, n);
        size_t olen = 0;
        if (err != ESP_OK) {
            out = err_json(esp_err_to_name(err));
        } else if (mbedtls_base64_encode(enc, ((n + 2) / 3) * 4 + 1, &olen, raw, n) != 0) {
            out = err_json("base64 failed");
        } else {
            enc[olen] = '\0';
            cJSON *o = cJSON_CreateObject();
            cJSON_AddBoolToObject(o, "ok", true);
            cJSON_AddNumberToObject(o, "offset", offset);
            cJSON_AddNumberToObject(o, "len", (double)n);
            cJSON_AddNumberToObject(o, "total", (double)total);
            cJSON_AddBoolToObject(o, "eof", offset + n >= total);
            cJSON_AddStringToObject(o, "data", (const char *)enc);
            out = take(o);
        }
    } else {
        out = err_json("out of memory");
    }
    free(raw);
    free(enc);
    return out;
}

char *crash_report_api_get(const char *path)
{
    if (!path || !s_lock) return err_json("crash report not initialised");
    uint32_t v = 0;
    if (query_u32(path, "offset", &v)) {
        uint32_t len = CRASH_CHUNK_MAX;
        query_u32(path, "len", &len);
        return api_chunk(v, len);
    }
    if (query_u32(path, "report", &v) && v) {
        // Same sections as the boot log lines, as one object.
        cJSON *o = cJSON_CreateObject();
        cJSON_AddItemToObject(o, "sys", sec_sys());
        cJSON_AddItemToObject(o, "mem", sec_mem());
        cJSON_AddItemToObject(o, "net", sec_net());
        cJSON_AddItemToObject(o, "hw", sec_hw());
        char *sc = api_scripts_status("/api/scripts/status", NULL);
        if (sc) {
            cJSON *j = cJSON_Parse(sc);
            cJSON_free(sc);
            if (j) cJSON_AddItemToObject(o, "scripts", j);
        }
        cJSON_AddItemToObject(o, "crash", sec_crash(true));
        return take(o);
    }
    cJSON *o = sec_crash(true);
    cJSON_AddBoolToObject(o, "ok", true);
    return take(o);
}

char *crash_report_api_clear(void)
{
    if (!s_lock) return err_json("crash report not initialised");
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool had = s_dump.present;
    esp_err_t err = ESP_OK;
    if (s_dump.loaded && had) {
        err = esp_core_dump_image_erase();
        if (err == ESP_OK) {
            free(s_dump.sum);
            memset(&s_dump, 0, sizeof(s_dump));
            s_dump.loaded = true;
            s_dump.check = ESP_ERR_NOT_FOUND;
        }
    } else if (!s_dump.loaded) {
        err = ESP_ERR_INVALID_STATE;     // boot-time check not done: do not guess
    }
    if (err == ESP_OK) s_prev_valid = false;   // the pre-crash snapshot goes with it
    xSemaphoreGive(s_lock);

    if (err == ESP_ERR_INVALID_STATE) return err_json("crash info not ready, retry shortly");
    if (err != ESP_OK) return err_json(esp_err_to_name(err));
    cJSON *o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "ok", true);
    cJSON_AddBoolToObject(o, "had_dump", had);
    return take(o);
}
