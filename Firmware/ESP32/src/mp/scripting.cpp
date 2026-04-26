// =============================================================================
// scripting.cpp — MicroPython FreeRTOS task, hermetic per-eval lifecycle.
//
// Phase 1: in-memory eval only.
//   - 1 MB GC heap allocated from PSRAM.
//   - One FreeRTOS task (Core 0, priority 1) processes a depth-4 queue.
//   - Each eval: gc_init → mp_init → parse/compile/run → mp_deinit.
//   - stdout captured into a 4 KB ring buffer behind a mutex.
//   - Cooperative stop via volatile flag checked in VM hook + mp_hal_delay_ms.
// =============================================================================

#include "scripting.h"
#include "script_storage.h"
#include "config.h"

// MicroPython core headers — must be wrapped in extern "C" because MP is
// compiled as C, and its headers don't include their own extern "C" guards.
extern "C" {
#include "py/mpconfig.h"
#include "py/runtime.h"
#include "py/gc.h"
#include "py/nlr.h"
#include "py/lexer.h"
#include "py/parse.h"
#include "py/compile.h"
#include "py/obj.h"
#include "py/objexcept.h"
#include "py/mpstate.h"
#include "py/stackctrl.h"
#if MICROPY_VFS
#include "extmod/vfs.h"
#include "extmod/vfs_posix.h"
#endif
} // extern "C"

#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"

#include <string.h>
#include <stdio.h>

static const char *TAG = "scripting";

// ---------------------------------------------------------------------------
// Internal types
// ---------------------------------------------------------------------------

typedef struct {
    uint32_t id;
    char    *payload;   // heap-allocated copy; freed by task after run
    size_t   len;
} ScriptCmd;

// ---------------------------------------------------------------------------
// Module-level state (plain C statics — no MP_STATE_PORT slots per spec)
// ---------------------------------------------------------------------------

static void                *s_gc_heap       = NULL;
static QueueHandle_t        s_queue          = NULL;
static SemaphoreHandle_t    s_log_mutex      = NULL;
static SemaphoreHandle_t    s_status_mutex   = NULL;

// Log ring
static char     s_log_ring[MP_LOG_RING_SIZE];
static size_t   s_log_head = 0;   // write position
static size_t   s_log_used = 0;   // bytes in ring
static bool     s_log_truncated = false;

// Stop flag — volatile, written by scripting_stop(), read by task/hooks
static volatile bool s_stop_requested = false;

// Status
static ScriptStatus s_status;
static uint32_t     s_next_id = 1;

// Scripting enabled flag (false if PSRAM alloc failed)
static bool s_enabled = false;

// ---------------------------------------------------------------------------
// Forward declarations for C linkage (called from C translation units)
// ---------------------------------------------------------------------------

extern "C" void  scripting_log_push(const char *str, size_t len);
extern "C" bool  scripting_stop_requested(void);
extern "C" void  scripting_vm_hook(void);
extern "C" void  scripting_init(void);
extern "C" bool  scripting_run_string(const char *src, size_t len);
extern "C" bool  scripting_run_file(const char *name, uint32_t *out_id);
extern "C" void  scripting_stop(void);
extern "C" size_t scripting_get_logs(char *out, size_t max);
extern "C" void  scripting_get_status(ScriptStatus *out);

// ---------------------------------------------------------------------------
// Log ring implementation
// ---------------------------------------------------------------------------

// Must be called with s_log_mutex held.
static void log_push_locked(const char *str, size_t len)
{
    if (len == 0) return;

    // How many bytes fit?
    size_t free_space = MP_LOG_RING_SIZE - s_log_used;
    if (free_space == 0) {
        if (!s_log_truncated) {
            s_log_truncated = true;
            // We can't push anything — ring is full
        }
        return;
    }

    size_t to_write = len;
    bool will_truncate = false;
    static const char trunc_marker[] = "[truncated]\n";
    static const size_t trunc_len = sizeof(trunc_marker) - 1;

    if (to_write > free_space) {
        // Write what fits, leave room for the truncation marker if possible
        if (free_space > trunc_len) {
            to_write = free_space - trunc_len;
            will_truncate = true;
        } else {
            to_write = free_space;
        }
    }

    // Write bytes into ring (linear, since we use head+used as linear index
    // for simplicity — drain clears the ring so wrap is rare)
    size_t write_pos = s_log_head;
    for (size_t i = 0; i < to_write; i++) {
        s_log_ring[(write_pos + i) % MP_LOG_RING_SIZE] = str[i];
    }
    s_log_head = (s_log_head + to_write) % MP_LOG_RING_SIZE;
    s_log_used += to_write;

    if (will_truncate && !s_log_truncated) {
        s_log_truncated = true;
        // Append the marker
        for (size_t i = 0; i < trunc_len; i++) {
            s_log_ring[(s_log_head + i) % MP_LOG_RING_SIZE] = trunc_marker[i];
        }
        s_log_head = (s_log_head + trunc_len) % MP_LOG_RING_SIZE;
        s_log_used += trunc_len;
    }
}

// ---------------------------------------------------------------------------
// Public API — scripting_log_push (called from mphalport.c)
// ---------------------------------------------------------------------------

void scripting_log_push(const char *str, size_t len)
{
    // Tee to stderr for IDF console visibility
    fwrite(str, 1, len, stderr);

    if (!s_log_mutex) return;
    if (xSemaphoreTake(s_log_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        log_push_locked(str, len);
        xSemaphoreGive(s_log_mutex);
    }
}

// ---------------------------------------------------------------------------
// Public API — scripting_get_logs
// ---------------------------------------------------------------------------

size_t scripting_get_logs(char *out, size_t max)
{
    if (!out || max == 0 || !s_log_mutex) return 0;

    if (xSemaphoreTake(s_log_mutex, pdMS_TO_TICKS(50)) != pdTRUE) return 0;

    // Ring tail = (head - used + RING_SIZE) % RING_SIZE
    size_t to_copy = s_log_used < max ? s_log_used : max;
    size_t tail = (s_log_head + MP_LOG_RING_SIZE - s_log_used) % MP_LOG_RING_SIZE;

    for (size_t i = 0; i < to_copy; i++) {
        out[i] = s_log_ring[(tail + i) % MP_LOG_RING_SIZE];
    }

    // Clear the drained bytes
    s_log_used -= to_copy;
    if (s_log_used == 0) {
        s_log_head = 0;
        s_log_truncated = false;
    }

    xSemaphoreGive(s_log_mutex);
    return to_copy;
}

// ---------------------------------------------------------------------------
// Stop flag
// ---------------------------------------------------------------------------

bool scripting_stop_requested(void)
{
    return s_stop_requested;
}

void scripting_stop(void)
{
    s_stop_requested = true;
}

// ---------------------------------------------------------------------------
// VM hook — called at every back-edge by the MICROPY_VM_HOOK_LOOP macro.
// Raises KeyboardInterrupt only when an nlr_buf_t is on the stack.
// ---------------------------------------------------------------------------

void scripting_vm_hook(void)
{
    if (!s_stop_requested) return;
    // Only raise when called from script context (nlr_top != NULL)
    if (MP_STATE_THREAD(nlr_top) != NULL) {
        mp_raise_msg(&mp_type_KeyboardInterrupt, MP_ERROR_TEXT("stopped"));
    }
}

// ---------------------------------------------------------------------------
// Status helpers
// ---------------------------------------------------------------------------

static void status_set_running(uint32_t id)
{
    if (xSemaphoreTake(s_status_mutex, pdMS_TO_TICKS(20)) == pdTRUE) {
        s_status.is_running = true;
        s_status.current_script_id = id;
        xSemaphoreGive(s_status_mutex);
    }
}

static void status_set_done(bool had_error, const char *err_msg)
{
    if (xSemaphoreTake(s_status_mutex, pdMS_TO_TICKS(20)) == pdTRUE) {
        s_status.is_running = false;
        s_status.current_script_id = 0;
        s_status.total_runs++;
        if (had_error) {
            s_status.total_errors++;
            if (err_msg) {
                strncpy(s_status.last_error_msg, err_msg, sizeof(s_status.last_error_msg) - 1);
                s_status.last_error_msg[sizeof(s_status.last_error_msg) - 1] = '\0';
            }
        }
        xSemaphoreGive(s_status_mutex);
    }
}

void scripting_get_status(ScriptStatus *out)
{
    if (!out) return;
    if (!s_status_mutex) {
        memset(out, 0, sizeof(*out));
        return;
    }
    if (xSemaphoreTake(s_status_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        memcpy(out, &s_status, sizeof(s_status));
        xSemaphoreGive(s_status_mutex);
    }
}

// ---------------------------------------------------------------------------
// MicroPython task — one hermetic eval per dequeued ScriptCmd
// ---------------------------------------------------------------------------

static void taskMicroPython(void *pvParam)
{
    (void)pvParam;

    // Set stack top/limit before the loop so gc_collect and MICROPY_STACK_CHECK
    // have valid bounds. Without this, stack_top is zero and gc_collect scans
    // from &dummy to address 0 — undefined behaviour and likely a crash.
    volatile int stack_dummy;
    mp_stack_set_top((void *)&stack_dummy);
    mp_stack_set_limit(MP_TASK_STACK - 1024);

    ScriptCmd cmd;

    for (;;) {
        if (xQueueReceive(s_queue, &cmd, portMAX_DELAY) != pdTRUE) continue;

        // Clear stop flag at the start of each new script
        s_stop_requested = false;
        status_set_running(cmd.id);

        bool had_error = false;
        char err_msg[64] = {0};

        // Re-initialize GC heap and interpreter for each eval (hermetic)
        gc_init(s_gc_heap, (char *)s_gc_heap + MP_HEAP_SIZE);
        mp_init();

#if MICROPY_VFS
        // Mount VfsPosix at "/" so scripts can open files via their full SPIFFS
        // paths (e.g. "/spiffs/scripts/foo.py").  VfsPosix root "" means no
        // prefix is prepended — absolute paths pass through to the POSIX layer.
        // Must run after mp_init(), inside the task that owns the MP state.
        {
            // No root arg: VfsPosix uses paths as-is (empty root prefix).
            mp_obj_t vfs = MP_OBJ_TYPE_GET_SLOT(&mp_type_vfs_posix, make_new)(
                &mp_type_vfs_posix, 0, 0, NULL);
            mp_obj_t mount_args[2] = { vfs, mp_obj_new_str("/", 1) };
            mp_vfs_mount(NULL, 2, mount_args, (mp_map_t *)&mp_const_empty_map);
        }
#endif

        nlr_buf_t nlr;
        if (nlr_push(&nlr) == 0) {
            mp_lexer_t *lex = mp_lexer_new_from_str_len(
                MP_QSTR__lt_string_gt_, cmd.payload, cmd.len, 0);
            qstr source_name = lex->source_name;
            mp_parse_tree_t pt = mp_parse(lex, MP_PARSE_FILE_INPUT);
            mp_obj_t module_fun = mp_compile(&pt, source_name, false);
            mp_call_function_0(module_fun);
            nlr_pop();
        } else {
            // Exception was raised — print traceback to log ring via mp_hal_stdout
            had_error = true;
            mp_obj_t exc = MP_OBJ_FROM_PTR(nlr.ret_val);
            mp_obj_print_exception(&mp_plat_print, exc);

            // Also capture a short form into status
            mp_obj_type_t *type = (mp_obj_type_t *)mp_obj_get_type(exc);
            if (type && type->name) {
                const char *type_name = qstr_str(type->name);
                snprintf(err_msg, sizeof(err_msg), "%s", type_name);
            } else {
                snprintf(err_msg, sizeof(err_msg), "exception");
            }
        }

        mp_deinit();

        // Free the payload copy
        free(cmd.payload);

        status_set_done(had_error, err_msg);
    }
}

// ---------------------------------------------------------------------------
// scripting_init — call once after cmd_registry_init()
// ---------------------------------------------------------------------------

void scripting_init(void)
{
    memset(&s_status, 0, sizeof(s_status));

    // Allocate GC heap from PSRAM
    s_gc_heap = heap_caps_malloc(MP_HEAP_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_gc_heap) {
        ESP_LOGE(TAG, "Failed to allocate %u bytes for MicroPython GC heap from PSRAM — "
                       "scripting disabled", (unsigned)MP_HEAP_SIZE);
        s_enabled = false;
        return;
    }
    ESP_LOGI(TAG, "MicroPython GC heap: %u KB allocated from PSRAM @ %p",
             (unsigned)(MP_HEAP_SIZE / 1024), s_gc_heap);

    // Mutexes
    s_log_mutex    = xSemaphoreCreateMutex();
    s_status_mutex = xSemaphoreCreateMutex();
    if (!s_log_mutex || !s_status_mutex) {
        ESP_LOGE(TAG, "Failed to create scripting mutexes — scripting disabled");
        s_enabled = false;
        return;
    }

    // Script command queue
    s_queue = xQueueCreate(MP_QUEUE_DEPTH, sizeof(ScriptCmd));
    if (!s_queue) {
        ESP_LOGE(TAG, "Failed to create scripting queue — scripting disabled");
        s_enabled = false;
        return;
    }

    // FreeRTOS task: Core 0, priority 1 (below command processor at 2, above idle)
    BaseType_t ok = xTaskCreatePinnedToCore(
        taskMicroPython, "uPython",
        MP_TASK_STACK / sizeof(StackType_t),
        NULL, 1, NULL, 0);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "Failed to create MicroPython task — scripting disabled");
        s_enabled = false;
        return;
    }

    s_enabled = true;
    ESP_LOGI(TAG, "Scripting engine ready (queue depth %u)", (unsigned)MP_QUEUE_DEPTH);
}

// ---------------------------------------------------------------------------
// scripting_run_string — copy src and enqueue
// ---------------------------------------------------------------------------

bool scripting_run_string(const char *src, size_t len)
{
    if (!s_enabled || !src || len == 0 || !s_queue) return false;

    // Allocate payload copy — PSRAM if large, internal heap if small
    char *payload;
    if (len > 1024) {
        payload = (char *)heap_caps_malloc(len + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    } else {
        payload = (char *)malloc(len + 1);
    }
    if (!payload) {
        ESP_LOGE(TAG, "scripting_run_string: payload alloc failed (%zu bytes)", len);
        return false;
    }
    memcpy(payload, src, len);
    payload[len] = '\0';

    ScriptCmd cmd;
    cmd.id      = s_next_id++;
    cmd.payload = payload;
    cmd.len     = len;

    if (xQueueSend(s_queue, &cmd, 0) != pdTRUE) {
        free(payload);
        ESP_LOGW(TAG, "scripting_run_string: queue full");
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// scripting_run_file — load script from SPIFFS and enqueue
// ---------------------------------------------------------------------------

bool scripting_run_file(const char *name, uint32_t *out_id)
{
    if (!s_enabled || !name || !s_queue) return false;

    // Allocate read buffer from PSRAM
    uint8_t *buf = (uint8_t *)heap_caps_malloc(SCRIPT_BODY_MAX + 1,
                                               MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) {
        ESP_LOGE(TAG, "scripting_run_file: buf alloc failed");
        return false;
    }

    size_t file_len = SCRIPT_BODY_MAX;
    char err[80] = {0};
    bool read_ok = script_storage_read(name, buf, &file_len, err, sizeof(err));
    if (!read_ok) {
        ESP_LOGE(TAG, "scripting_run_file: read '%s' failed: %s", name, err);
        free(buf);
        return false;
    }
    buf[file_len] = '\0';

    ScriptCmd cmd;
    cmd.id      = s_next_id++;
    cmd.payload = (char *)buf;
    cmd.len     = file_len;

    if (out_id) *out_id = cmd.id;

    if (xQueueSend(s_queue, &cmd, 0) != pdTRUE) {
        free(buf);
        ESP_LOGW(TAG, "scripting_run_file: queue full");
        return false;
    }
    return true;
}
