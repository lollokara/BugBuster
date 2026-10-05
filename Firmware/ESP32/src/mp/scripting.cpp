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
#include "repl_ws.h"
#include "bbp.h"
#include "tasks.h"
#include "esp_attr.h"
#include "script_runtime.h"


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
#include <stdarg.h>
#include <time.h>


static const char *TAG = "scripting";

// ---------------------------------------------------------------------------
// Internal types
// ---------------------------------------------------------------------------

typedef struct {
    uint32_t          refcount;
    SemaphoreHandle_t sem;
    char             *payload;
    size_t            len;
    bool              lint_ok;
    char              err[128];
} ScriptLintJob;

static void lint_job_release(ScriptLintJob *job)
{
    if (!job) return;
    if (__atomic_sub_fetch(&job->refcount, 1, __ATOMIC_ACQ_REL) == 0) {
        if (job->payload) free(job->payload);
        if (job->sem) vSemaphoreDelete(job->sem);
        free(job);
    }
}

typedef struct {
    uint32_t          id;
    char             *payload;   // heap-allocated copy; freed by task after run
    size_t            len;
    bool              persist;   // V2-A: true = switch to / stay in PERSISTENT mode
    bool              is_lint;
    ScriptLintJob    *lint_job;
    char              name[SCRIPT_NAME_MAX + 1];
    ScriptSource      source;
    bool              is_file;   // holds the file-script slot
} ScriptCmd;


// ---------------------------------------------------------------------------
// Module-level state (plain C statics — no MP_STATE_PORT slots per spec)
// ---------------------------------------------------------------------------

static void                *s_gc_heap       = NULL;
static QueueHandle_t        s_queue          = NULL;
static SemaphoreHandle_t    s_log_mutex      = NULL;
static SemaphoreHandle_t    s_status_mutex   = NULL;

// Log ring
static EXT_RAM_BSS_ATTR char s_log_ring[MP_LOG_RING_SIZE];
static size_t   s_log_head = 0;   // write position
static size_t   s_log_used = 0;   // bytes in ring
static uint64_t s_log_total = 0;  // absolute bytes accepted into the ring
static bool     s_log_truncated = false;
static uint32_t s_log_dropped = 0;
static uint32_t s_log_dropped_total = 0;   // cumulative, for the hub health record

// Structured-line assembler for MicroPython output (guarded by s_log_mutex).
static sr_line_asm_t s_line_asm;


// Stop flag — volatile, written by scripting_stop(), read by task/hooks
static volatile bool s_stop_requested = false;

// Status
static ScriptStatus s_status;
static uint32_t     s_next_id = 1;

// Scripting enabled flag (false if PSRAM alloc failed)
static bool s_enabled = false;

// Jobs submitted and not yet finished by taskMicroPython (queued + executing,
// lint included) and per-source transfer lease deadlines, for scripting_has_work().
static uint32_t s_work_cmds = 0;
static uint32_t s_xfer_deadline[SCRIPT_XFER_COUNT] = {0};

// IO-ownership session counter — incremented once per vm_do_init() so the
// io_owner bindings can fill io_owner_t.session_id without caring about the
// underlying script ID.  Written only from taskMicroPython; read atomically.
static volatile uint8_t s_mp_session = 0;

// V2-A persistent-mode state (owned exclusively by taskMicroPython)
static ScriptingMode     s_mode           = SCRIPTING_MODE_EPHEMERAL;
static bool              s_vm_initialized = false;  // true while VM is alive across evals
static volatile bool     s_reset_requested = false; // set by scripting_reset_vm()
static uint32_t          s_last_eval_ms   = 0;      // ms tick of last eval dequeue
static uint32_t          s_auto_reset_count = 0;    // watermark/idle auto-resets

// GC heap watermark thresholds (percentage of MP_HEAP_SIZE)
#define MP_HEAP_SOFT_WATERMARK_PCT  80u
#define MP_HEAP_HARD_WATERMARK_PCT  95u

// M05: MP_IDLE_CHECK_MS must be > 0 or xQueueReceive is called with timeout=0,
// which means the task never yields when the queue is continuously saturated,
// starving lower-priority tasks and potentially tripping the WDT.
static_assert(MP_IDLE_CHECK_MS > 0, "MP_IDLE_CHECK_MS must be > 0 to ensure task yields");

// ---------------------------------------------------------------------------
// Forward declarations for C linkage (called from C translation units)
// ---------------------------------------------------------------------------

extern "C" void  scripting_log_push(const char *str, size_t len);
extern "C" bool  scripting_stop_requested(void);
extern "C" void  scripting_vm_hook(void);
extern "C" void  scripting_init(void);
extern "C" bool  scripting_run_string(const char *src, size_t len, bool persist);
extern "C" bool  scripting_lint_string(const char *src, size_t len, char *out_err, size_t max_err);
extern "C" bool  scripting_run_file(const char *name, uint32_t *out_id);
extern "C" void  scripting_stop(void);
extern "C" size_t scripting_get_logs(char *out, size_t max);
extern "C" size_t scripting_get_logs_since(char *out, size_t max, uint64_t since, uint64_t *out_next);
extern "C" void  scripting_get_status(ScriptStatus *out);
extern "C" void  scripting_reset_vm(void);
extern "C" ScriptSubmitResult scripting_submit(const char *src, size_t len, const ScriptSubmitOpts *opts, uint32_t *out_id);
extern "C" ScriptSubmitResult scripting_submit_file(const char *name, ScriptSource source, bool replace, uint32_t *out_id);
extern "C" bool  scripting_wait_slot_free(uint32_t timeout_ms);
extern "C" void  scripting_log_event(char level, const char *fmt, ...);
extern "C" void  scripting_log(char level, const char *msg, size_t len);


// V2-D: native exec pool cleanup — implemented in mphalport.c (C linkage).
extern "C" void  bb_native_code_free_all(void);

// ---------------------------------------------------------------------------
// Log ring implementation
// ---------------------------------------------------------------------------

// Must be called with s_log_mutex held.
static void log_push_locked(const char *str, size_t len)
{
    if (len == 0) return;

    // Keep the newest output. Browser log polling is non-destructive, so this
    // ring must overwrite old bytes instead of becoming permanently full.
    for (size_t i = 0; i < len; i++) {
        s_log_ring[s_log_head] = str[i];
        s_log_head = (s_log_head + 1) % MP_LOG_RING_SIZE;
        if (s_log_used < MP_LOG_RING_SIZE) {
            s_log_used++;
        } else {
            s_log_truncated = true;
        }
        s_log_total++;
    }
}

static uint32_t now_ms(void)
{
    return (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
}

static scripting_log_tee_fn s_log_tee;
void scripting_set_log_tee(scripting_log_tee_fn fn) { s_log_tee = fn; }

static void ring_emit(void *ctx, const char *line, size_t len)
{
    (void)ctx;
    log_push_locked(line, len);
    if (s_log_tee) s_log_tee(line, len);
}


uint32_t scripting_log_dropped_total(void)
{
    return __atomic_load_n(&s_log_dropped_total, __ATOMIC_RELAXED) + __atomic_load_n(&s_log_dropped, __ATOMIC_RELAXED);
}

static void emit_drop_warning_locked(uint32_t ts)
{
    uint32_t dropped = __atomic_exchange_n(&s_log_dropped, 0, __ATOMIC_RELAXED);
    __atomic_fetch_add(&s_log_dropped_total, dropped, __ATOMIC_RELAXED);
    if (dropped > 0) {
        char drop_msg[64];
        int dn = snprintf(drop_msg, sizeof(drop_msg), "%lu log lines dropped (busy)", (unsigned long)dropped);
        if (dn > 0) {
            char drop_line[SR_LINE_MAX + 40];
            size_t dlen = sr_format_line(drop_line, sizeof(drop_line), ts, 'W', "sys", drop_msg, (size_t)dn);
            ring_emit(NULL, drop_line, dlen);
        }
    }
}


// Level stamped on MicroPython output ('E' while a traceback prints).
static void log_set_level(char level)
{
    if (!s_log_mutex) return;
    if (xSemaphoreTake(s_log_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        sr_line_set_level(&s_line_asm, level, now_ms(), ring_emit, NULL);
        xSemaphoreGive(s_log_mutex);
    }
}

// Close a dangling partial line (script ended without '\n').
static void log_flush(void)
{
    if (!s_log_mutex) return;
    if (xSemaphoreTake(s_log_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        sr_line_flush(&s_line_asm, now_ms(), ring_emit, NULL);
        xSemaphoreGive(s_log_mutex);
    }
}

void scripting_log_event(char level, const char *fmt, ...)
{
    char text[160];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(text, sizeof(text), fmt, ap);
    va_end(ap);
    if (n < 0) return;
    size_t tlen = (size_t)n < sizeof(text) ? (size_t)n : sizeof(text) - 1;
    ESP_LOGI(TAG, "%s", text);
    if (!s_log_mutex) return;
    char line[SR_LINE_MAX + 40];
    if (xSemaphoreTake(s_log_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        uint32_t ts = now_ms();
        emit_drop_warning_locked(ts);
        sr_line_flush(&s_line_asm, ts, ring_emit, NULL);
        size_t len = sr_format_line(line, sizeof(line), ts, level, "sys", text, tlen);
        log_push_locked(line, len);
        xSemaphoreGive(s_log_mutex);
    } else {
        __atomic_fetch_add(&s_log_dropped, 1, __ATOMIC_RELAXED);
    }
}

void scripting_log(char level, const char *msg, size_t len)
{
    if (level != 'E' && level != 'W' && level != 'I' && level != 'D') {
        level = 'I';
    }
    if (!msg) {
        msg = "";
        len = 0;
    }

    // Count non-empty segments separated by \r or \n
    size_t non_empty_segments = 0;
    size_t seg_start = 0;
    for (size_t i = 0; i <= len; i++) {
        if (i == len || msg[i] == '\n' || msg[i] == '\r') {
            if (i > seg_start) {
                non_empty_segments++;
            }
            seg_start = i + 1;
        }
    }

    // Tee to stderr (CDC #0) and browser REPL terminal
    if (non_empty_segments == 0) {
        if (!bbpCdcClaimed()) {
            fputc('\n', stderr);
        }
        repl_ws_forward("\n", 1);
    } else {
        seg_start = 0;
        for (size_t i = 0; i <= len; i++) {
            if (i == len || msg[i] == '\n' || msg[i] == '\r') {
                size_t seg_len = i - seg_start;
                if (seg_len > 0) {
                    if (!bbpCdcClaimed()) {
                        fwrite(msg + seg_start, 1, seg_len, stderr);
                        fputc('\n', stderr);
                    }
                    repl_ws_forward(msg + seg_start, seg_len);
                    repl_ws_forward("\n", 1);
                }
                seg_start = i + 1;
            }
        }
    }

    if (!s_log_mutex) return;
    size_t lines_to_emit = (non_empty_segments == 0) ? 1 : non_empty_segments;
    if (xSemaphoreTake(s_log_mutex, pdMS_TO_TICKS(10)) != pdTRUE) {
        __atomic_fetch_add(&s_log_dropped, (uint32_t)lines_to_emit, __ATOMIC_RELAXED);
        return;
    }

    uint32_t ts = now_ms();
    emit_drop_warning_locked(ts);
    sr_line_flush(&s_line_asm, ts, ring_emit, NULL);

    char line[SR_LINE_MAX + 40];
    if (non_empty_segments == 0) {
        size_t n = sr_format_line(line, sizeof(line), ts, level, "mpy", "", 0);
        ring_emit(NULL, line, n);
    } else {
        seg_start = 0;
        for (size_t i = 0; i <= len; i++) {
            if (i == len || msg[i] == '\n' || msg[i] == '\r') {
                size_t seg_len = i - seg_start;
                if (seg_len > 0) {
                    size_t n = sr_format_line(line, sizeof(line), ts, level, "mpy", msg + seg_start, seg_len);
                    ring_emit(NULL, line, n);
                }
                seg_start = i + 1;
            }
        }
    }

    sr_line_set_level(&s_line_asm, 'I', ts, ring_emit, NULL);
    xSemaphoreGive(s_log_mutex);
}

// ---------------------------------------------------------------------------
// Public API — scripting_log_push (called from mphalport.c)
// ---------------------------------------------------------------------------

void scripting_log_push(const char *str, size_t len)
{
    // Tee to stderr (CDC #0) for console visibility, except while a BBP host owns it:
    // raw bytes there corrupt the COBS stream. Output still reaches the log ring.
    if (!bbpCdcClaimed()) fwrite(str, 1, len, stderr);

    // Also feed the browser REPL terminal. repl_ws_forward() is non-blocking
    // and becomes a no-op until a WebSocket session is authenticated.
    repl_ws_forward(str, len);

    // The ring stores structured lines; the REPL terminal and stderr above keep
    // the raw bytes so interactive output still looks like a terminal.
    if (!s_log_mutex) return;
    if (xSemaphoreTake(s_log_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        emit_drop_warning_locked(now_ms());
        sr_line_feed(&s_line_asm, now_ms(), str, len, ring_emit, NULL);
        xSemaphoreGive(s_log_mutex);
    } else {
        __atomic_fetch_add(&s_log_dropped, 1, __ATOMIC_RELAXED);
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

size_t scripting_get_logs_since(char *out, size_t max, uint64_t since, uint64_t *out_next)
{
    if (!out || max == 0 || !s_log_mutex) {
        if (out_next) *out_next = 0;
        return 0;
    }

    if (xSemaphoreTake(s_log_mutex, pdMS_TO_TICKS(50)) != pdTRUE) {
        if (out_next) *out_next = since;
        return 0;
    }

    const uint64_t base = s_log_total - s_log_used;
    uint64_t start = since < base ? base : since;
    if (start > s_log_total) start = s_log_total;

    uint64_t available = s_log_total - start;
    size_t to_copy = available < max ? (size_t)available : max;
    size_t tail = (s_log_head + MP_LOG_RING_SIZE - s_log_used) % MP_LOG_RING_SIZE;
    size_t offset = (size_t)(start - base);

    for (size_t i = 0; i < to_copy; i++) {
        out[i] = s_log_ring[(tail + offset + i) % MP_LOG_RING_SIZE];
    }

    if (out_next) *out_next = start + to_copy;
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
    if (s_status_mutex && xSemaphoreTake(s_status_mutex, pdMS_TO_TICKS(20)) == pdTRUE) {
        if (s_status.is_running) s_status.state = SCRIPT_STATE_STOPPING;
        xSemaphoreGive(s_status_mutex);
    }
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

static void status_set_running(const ScriptCmd *cmd)
{
    if (xSemaphoreTake(s_status_mutex, portMAX_DELAY) == pdTRUE) {
        s_status.is_running = true;
        s_status.current_script_id = cmd->id;
        strncpy(s_status.name, cmd->name, SCRIPT_NAME_MAX);
        s_status.name[SCRIPT_NAME_MAX] = '\0';
        s_status.source = cmd->source;
        s_status.state = SCRIPT_STATE_RUNNING;
        time_t now = time(NULL);
        s_status.started_at_epoch = now > (time_t)1700000000;   // wall clock has been set
        s_status.started_at = s_status.started_at_epoch ? (uint32_t)now : now_ms();
        xSemaphoreGive(s_status_mutex);
    }
}

// portMAX_DELAY: a missed release would hold the file-script slot forever.
static void status_set_done(const ScriptCmd *cmd, ScriptExit exit_kind, const char *err_msg)
{
    if (xSemaphoreTake(s_status_mutex, portMAX_DELAY) == pdTRUE) {
        s_status.is_running = false;
        s_status.last_script_id = s_status.current_script_id;  // capture before clear
        s_status.current_script_id = 0;
        s_status.total_runs++;
        s_status.last_exit = exit_kind;
        s_status.state = (exit_kind == SCRIPT_EXIT_ERROR) ? SCRIPT_STATE_ERROR : SCRIPT_STATE_DONE;
        if (exit_kind == SCRIPT_EXIT_ERROR) {
            s_status.total_errors++;
            if (err_msg) {
                strncpy(s_status.last_error_msg, err_msg, sizeof(s_status.last_error_msg) - 1);
                s_status.last_error_msg[sizeof(s_status.last_error_msg) - 1] = '\0';
            }
        }
        if (s_status.file_slot_id == cmd->id) {   // a replace may already own the slot
            s_status.file_slot_id = 0;
            s_status.file_slot_name[0] = '\0';
        }
        xSemaphoreGive(s_status_mutex);
    }
}


// Update V2-A persistent-mode fields that require MP task context (gc_info etc.).
// MUST be called from taskMicroPython only (while VM is alive).
// Caller must hold s_status_mutex.
static void status_update_mp_fields_locked(void)
{
    s_status.mode             = s_mode;
    s_status.auto_reset_count = s_auto_reset_count;
    s_status.last_eval_at_ms  = s_last_eval_ms;

    if (s_last_eval_ms == 0) {
        s_status.idle_for_ms = 0;
    } else {
        uint32_t now_ms = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
        s_status.idle_for_ms = now_ms - s_last_eval_ms;
    }

    if (s_vm_initialized) {
        gc_info_t info;
        gc_info(&info);
        size_t used = info.total - info.free;
        s_status.globals_bytes_est = (uint32_t)used;

        // Count globals
        mp_obj_dict_t *gdict = mp_globals_get();
        s_status.globals_count = gdict ? (uint32_t)gdict->map.used : 0;

        // Soft watermark: used >= 80% of heap
        s_status.watermark_soft_hit = (used * 100u >= (size_t)MP_HEAP_SIZE * MP_HEAP_SOFT_WATERMARK_PCT);
    } else {
        s_status.globals_bytes_est  = 0;
        s_status.globals_count      = 0;
        s_status.watermark_soft_hit = false;
    }
}

// Update non-MP persistent fields that are safe to call from any context.
// Caller must hold s_status_mutex.
static void status_update_safe_fields_locked(void)
{
    s_status.mode             = s_mode;
    s_status.auto_reset_count = s_auto_reset_count;
    s_status.last_eval_at_ms  = s_last_eval_ms;

    if (s_last_eval_ms == 0) {
        s_status.idle_for_ms = 0;
    } else {
        uint32_t now_ms = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
        s_status.idle_for_ms = now_ms - s_last_eval_ms;
    }
    // globals_bytes_est, globals_count, watermark_soft_hit are updated by the
    // MP task via status_update_mp_fields_locked(); leave them as-is here.
}

void scripting_get_status(ScriptStatus *out)
{
    if (!out) return;
    if (!s_status_mutex) {
        memset(out, 0, sizeof(*out));
        return;
    }
    if (xSemaphoreTake(s_status_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        // Refresh non-MP fields (safe from any thread)
        status_update_safe_fields_locked();
        memcpy(out, &s_status, sizeof(s_status));
        xSemaphoreGive(s_status_mutex);
    }
}

// ---------------------------------------------------------------------------
// MicroPython task — one hermetic eval per dequeued ScriptCmd
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// vm_init / vm_deinit helpers — called from taskMicroPython only
// ---------------------------------------------------------------------------

static void vm_do_init(void)
{
    // Bump session counter before mp_init so any binding called during module
    // init sees the new session_id.  Wraps at 255 intentionally.
    s_mp_session = (uint8_t)(s_mp_session + 1u);

    gc_init(s_gc_heap, (char *)s_gc_heap + MP_HEAP_SIZE);
    mp_init();

#if MICROPY_VFS
    {
        mp_obj_t vfs = MP_OBJ_TYPE_GET_SLOT(&mp_type_vfs_posix, make_new)(
            &mp_type_vfs_posix, 0, 0, NULL);
        mp_obj_t mount_args[2] = { vfs, mp_obj_new_str("/", 1) };
        mp_vfs_mount(2, mount_args, (mp_map_t *)&mp_const_empty_map);
    }
#endif

    s_vm_initialized = true;
}

static void vm_do_deinit(void)
{
    mp_deinit();
    bb_native_code_free_all();
    tasks_reset_hardware();
    s_vm_initialized = false;
}

typedef struct {
    char *buf;
    size_t len;
    size_t max;
} string_printer_t;

static void string_printer_strn(void *data, const char *str, size_t len)
{
    string_printer_t *sp = (string_printer_t *)data;
    if (sp->len + len < sp->max) {
        memcpy(sp->buf + sp->len, str, len);
        sp->len += len;
        sp->buf[sp->len] = '\0';
    } else if (sp->len + 1 < sp->max) {
        size_t fit = sp->max - sp->len - 1;
        memcpy(sp->buf + sp->len, str, fit);
        sp->len += fit;
        sp->buf[sp->len] = '\0';
    }
}


static void taskMicroPython(void *pvParam)
{
    (void)pvParam;

    // Set stack top/limit before the loop so gc_collect and MICROPY_STACK_CHECK
    // have valid bounds. Without this, stack_top is zero and gc_collect scans
    // from &dummy to address 0 — undefined behaviour and likely a crash.
    volatile int stack_dummy;
    mp_stack_set_top((void *)&stack_dummy);
    mp_stack_set_limit(MP_TASK_STACK - 1024);

    ScriptCmd cmd = {};

    for (;;) {
        // ---------------------------------------------------------------
        // In PERSISTENT mode use a timed receive so we can check idle
        // timeout and reset requests even when no scripts are queued.
        // In EPHEMERAL mode keep portMAX_DELAY (zero overhead).
        // ---------------------------------------------------------------
        TickType_t wait_ticks = (s_mode == SCRIPTING_MODE_PERSISTENT)
                                ? pdMS_TO_TICKS(MP_IDLE_CHECK_MS)
                                : portMAX_DELAY;

        if (xQueueReceive(s_queue, &cmd, wait_ticks) != pdTRUE) {
            // Timeout — only happens in PERSISTENT mode.
            // Check: explicit reset request OR idle timeout OR hard watermark.
            if (s_mode != SCRIPTING_MODE_PERSISTENT) continue;

            bool do_reset = s_reset_requested;

            if (!do_reset && s_vm_initialized && s_last_eval_ms != 0) {
                uint32_t now_ms = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
                if ((now_ms - s_last_eval_ms) >= MP_PERSISTENT_IDLE_MS) {
                    ESP_LOGI(TAG, "Persistent VM: idle timeout, resetting");
                    do_reset = true;
                }
            }

            if (!do_reset && s_vm_initialized) {
                gc_info_t info;
                gc_info(&info);
                size_t used = info.total - info.free;
                if (used * 100u >= (size_t)MP_HEAP_SIZE * MP_HEAP_HARD_WATERMARK_PCT) {
                    ESP_LOGW(TAG, "Persistent VM: hard watermark hit (%zu/%u bytes), resetting",
                             used, (unsigned)MP_HEAP_SIZE);
                    do_reset = true;
                }
            }

            if (do_reset && s_vm_initialized) {
                vm_do_deinit();
                s_mode = SCRIPTING_MODE_EPHEMERAL;
                s_reset_requested = false;
                s_auto_reset_count++;
                if (xSemaphoreTake(s_status_mutex, pdMS_TO_TICKS(20)) == pdTRUE) {
                    status_update_mp_fields_locked();
                    xSemaphoreGive(s_status_mutex);
                }
                ESP_LOGI(TAG, "Persistent VM reset complete (auto_resets=%u)", (unsigned)s_auto_reset_count);
            } else {
                s_reset_requested = false;
            }
            continue;
        }

        // ---------------------------------------------------------------
        // We dequeued a ScriptCmd.
        // ---------------------------------------------------------------

        // Honor a pending reset request BEFORE running this eval. Otherwise
        // a script enqueued immediately after /api/scripts/reset would run
        // on the old persistent VM and the reset would silently no-op.
        if (s_reset_requested) {
            if (s_vm_initialized) vm_do_deinit();
            s_mode = SCRIPTING_MODE_EPHEMERAL;
            s_reset_requested = false;
            s_auto_reset_count++;
            ESP_LOGI(TAG, "Persistent VM reset honored before eval id=%u", (unsigned)cmd.id);
        }

        ScriptExit exit_kind = SCRIPT_EXIT_OK;
        char err_msg[64] = {0};


        // Initialize VM if not already alive (ephemeral always; persistent on first use)
        if (!s_vm_initialized) {
            vm_do_init();
        }

        if (cmd.is_lint) {
            ScriptLintJob *job = cmd.lint_job;
            if (job) {
                bool lint_ok = false;
                nlr_buf_t nlr;
                if (nlr_push(&nlr) == 0) {
                    mp_lexer_t *lex = mp_lexer_new_from_str_len(
                        MP_QSTR__lt_string_gt_, job->payload, job->len, 0);
                    qstr source_name = lex->source_name;
                    mp_parse_tree_t pt = mp_parse(lex, MP_PARSE_FILE_INPUT);
                    mp_compile(&pt, source_name, false);
                    nlr_pop();
                    lint_ok = true;
                } else {
                    lint_ok = false;
                    mp_obj_t exc = MP_OBJ_FROM_PTR(nlr.ret_val);
                    string_printer_t sp_data = { job->err, 0, sizeof(job->err) };
                    mp_print_t custom_print = { &sp_data, string_printer_strn };
                    mp_obj_print_exception(&custom_print, exc);
                }
                job->lint_ok = lint_ok;
                if (job->sem) xSemaphoreGive(job->sem);
                lint_job_release(job);
            }
        } else {
            // In EPHEMERAL mode with persist=true: switch to persistent mode.
            // In PERSISTENT mode: persist flag is sticky (ignored if false).
            if (cmd.persist && s_mode == SCRIPTING_MODE_EPHEMERAL) {
                s_mode = SCRIPTING_MODE_PERSISTENT;
            }

            s_last_eval_ms = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);

            // Clear stop flag at the start of each new script
            s_stop_requested = false;
            status_set_running(&cmd);
            if (cmd.is_file) {
                scripting_log_event('I', "start %s id=%lu source=%s", cmd.name,
                                    (unsigned long)cmd.id, sr_source_name(cmd.source));
            }


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
                // A stop request surfaces as KeyboardInterrupt: that is a
                // requested exit, not an error.
                mp_obj_t exc = MP_OBJ_FROM_PTR(nlr.ret_val);
                mp_obj_type_t *type = (mp_obj_type_t *)mp_obj_get_type(exc);
                bool stopped = s_stop_requested &&
                    mp_obj_is_subclass_fast(MP_OBJ_FROM_PTR(type),
                                            MP_OBJ_FROM_PTR(&mp_type_KeyboardInterrupt));
                exit_kind = stopped ? SCRIPT_EXIT_STOPPED : SCRIPT_EXIT_ERROR;

                log_set_level(stopped ? 'W' : 'E');
                mp_obj_print_exception(&mp_plat_print, exc);
                log_set_level('I');

                if (type && type->name) {
                    snprintf(err_msg, sizeof(err_msg), "%s", qstr_str(type->name));
                } else {
                    snprintf(err_msg, sizeof(err_msg), "exception");
                }
            }

        }

        // In EPHEMERAL mode: tear down VM after every eval.
        // In PERSISTENT mode: keep VM alive; check hard watermark.
        if (s_mode == SCRIPTING_MODE_EPHEMERAL) {
            vm_do_deinit();
        } else if (!cmd.is_lint) {
            // Check watermarks immediately after eval
            gc_info_t info;
            gc_info(&info);
            size_t used = info.total - info.free;
            // Soft watermark: collect to recover fragmented blocks (V2-A spec §4)
            if (used * 100u >= (size_t)MP_HEAP_SIZE * MP_HEAP_SOFT_WATERMARK_PCT) {
                ESP_LOGD(TAG, "Persistent VM: soft watermark after eval (%zu/%u), collecting",
                         used, (unsigned)MP_HEAP_SIZE);
                gc_collect();
            }
            // Hard watermark: reset VM to prevent OOM on next eval
            if (used * 100u >= (size_t)MP_HEAP_SIZE * MP_HEAP_HARD_WATERMARK_PCT) {
                ESP_LOGW(TAG, "Persistent VM: hard watermark after eval (%zu/%u), resetting",
                         used, (unsigned)MP_HEAP_SIZE);
                vm_do_deinit();
                s_mode = SCRIPTING_MODE_EPHEMERAL;
                s_auto_reset_count++;
            }
        }

        // Free the payload copy (lint payload is owned by ScriptLintJob and freed by lint_job_release)
        if (!cmd.is_lint) {
            free(cmd.payload);
        }

        if (!cmd.is_lint) {
            log_flush();
            // Update persistent-mode status fields under mutex
            if (xSemaphoreTake(s_status_mutex, pdMS_TO_TICKS(20)) == pdTRUE) {
                status_update_mp_fields_locked();
                xSemaphoreGive(s_status_mutex);
            }

            status_set_done(&cmd, exit_kind, err_msg);
            if (cmd.is_file) {
                if (exit_kind == SCRIPT_EXIT_OK) {
                    scripting_log_event('I', "done %s id=%lu", cmd.name, (unsigned long)cmd.id);
                } else if (exit_kind == SCRIPT_EXIT_STOPPED) {
                    scripting_log_event('W', "stopped %s id=%lu", cmd.name, (unsigned long)cmd.id);
                } else {
                    scripting_log_event('E', "error %s id=%lu: %s", cmd.name,
                                        (unsigned long)cmd.id, err_msg);
                }
            }
        }


        __atomic_sub_fetch(&s_work_cmds, 1, __ATOMIC_SEQ_CST);

        // M05: yield at least one tick between back-to-back evals so lower-
        // priority tasks (idle, WDT feed) get CPU time even when the queue is
        // continuously saturated.  xQueueReceive with a non-zero timeout already
        // yields when the queue is empty; this covers the always-full case.
        vTaskDelay(1);
    }
}

// ---------------------------------------------------------------------------
// scripting_init — call once after cmd_registry_init()
// ---------------------------------------------------------------------------

void scripting_init(void)
{
    memset(&s_status, 0, sizeof(s_status));
    s_log_dropped = 0;
    sr_line_init(&s_line_asm, "mpy", 'I');


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

static char *copy_payload(const char *src, size_t len)
{
    // PSRAM if large, internal heap if small
    char *payload = (len > 1024)
        ? (char *)heap_caps_malloc(len + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
        : (char *)malloc(len + 1);
    if (!payload) return NULL;
    memcpy(payload, src, len);
    payload[len] = '\0';
    return payload;
}

// Caller holds s_status_mutex.
static void claim_slot_locked(const ScriptCmd *cmd)
{
    s_status.file_slot_id = cmd->id;
    strncpy(s_status.file_slot_name, cmd->name, SCRIPT_NAME_MAX);
    s_status.file_slot_name[SCRIPT_NAME_MAX] = '\0';
}

static void release_slot(uint32_t id)
{
    if (xSemaphoreTake(s_status_mutex, portMAX_DELAY) == pdTRUE) {
        if (s_status.file_slot_id == id) {
            s_status.file_slot_id = 0;
            s_status.file_slot_name[0] = '\0';
        }
        xSemaphoreGive(s_status_mutex);
    }
}

bool scripting_wait_slot_free(uint32_t timeout_ms)
{
    if (!s_status_mutex) return true;
    TickType_t t0 = xTaskGetTickCount();
    for (;;) {
        bool free_now = false;
        if (xSemaphoreTake(s_status_mutex, pdMS_TO_TICKS(20)) == pdTRUE) {
            free_now = (s_status.file_slot_id == 0);
            xSemaphoreGive(s_status_mutex);
        }
        if (free_now) return true;
        if ((xTaskGetTickCount() - t0) >= pdMS_TO_TICKS(timeout_ms)) return false;
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

ScriptSubmitResult scripting_submit(const char *src, size_t len, const ScriptSubmitOpts *opts,
                                    uint32_t *out_id)
{
    if (!s_enabled || !src || len == 0 || !s_queue || !opts) return SCRIPT_SUBMIT_DISABLED;

    ScriptCmd cmd = {};
    cmd.id      = __atomic_fetch_add(&s_next_id, 1, __ATOMIC_RELAXED);
    cmd.persist = opts->persist;
    cmd.source  = opts->source;
    cmd.is_file = opts->is_file;
    const char *nm = opts->name ? opts->name
                   : (opts->source == SCRIPT_SRC_REPL ? "<repl>" : "<eval>");
    strncpy(cmd.name, nm, SCRIPT_NAME_MAX);
    cmd.name[SCRIPT_NAME_MAX] = '\0';

    // Admission and the slot claim are one critical section, so two racing
    // file runs (BLE + HTTP) cannot both see a free slot.
    uint32_t holder_id = 0;
    char holder[SCRIPT_NAME_MAX + 1];
    xSemaphoreTake(s_status_mutex, portMAX_DELAY);
    sr_admit_t adm = sr_admit(s_status.file_slot_id != 0, opts->is_file, opts->replace);
    holder_id = s_status.file_slot_id;
    strncpy(holder, s_status.file_slot_name, SCRIPT_NAME_MAX);
    holder[SCRIPT_NAME_MAX] = '\0';
    if (adm == SR_ADMIT_START && opts->is_file) claim_slot_locked(&cmd);
    xSemaphoreGive(s_status_mutex);

    if (adm == SR_ADMIT_BUSY) return SCRIPT_SUBMIT_BUSY;

    if (adm == SR_ADMIT_REPLACE) {
        scripting_log_event('W', "replace %s id=%lu with %s", holder,
                            (unsigned long)holder_id, cmd.name);
        scripting_stop();
        if (!scripting_wait_slot_free(MP_REPLACE_STOP_TIMEOUT_MS)) {
            // Never delete taskMicroPython: the holder may own s_hat_mutex or a
            // bus lock. The reset flag tears the VM down before the queued
            // script runs, once the holder returns from its C call.
            scripting_log_event('W', "%s did not stop in %u ms; VM reset queued", holder,
                                (unsigned)MP_REPLACE_STOP_TIMEOUT_MS);
            scripting_reset_vm();
        }
        xSemaphoreTake(s_status_mutex, portMAX_DELAY);
        bool taken_by_other = s_status.file_slot_id != 0 && s_status.file_slot_id != holder_id;
        if (!taken_by_other) claim_slot_locked(&cmd);
        xSemaphoreGive(s_status_mutex);
        if (taken_by_other) return SCRIPT_SUBMIT_BUSY;
    }

    cmd.payload = copy_payload(src, len);
    if (!cmd.payload) {
        if (opts->is_file) release_slot(cmd.id);
        ESP_LOGE(TAG, "scripting_submit: payload alloc failed (%zu bytes)", len);
        return SCRIPT_SUBMIT_QUEUE_FULL;
    }
    cmd.len = len;

    __atomic_add_fetch(&s_work_cmds, 1, __ATOMIC_SEQ_CST);
    if (xQueueSend(s_queue, &cmd, 0) != pdTRUE) {
        __atomic_sub_fetch(&s_work_cmds, 1, __ATOMIC_SEQ_CST);
        free(cmd.payload);
        if (opts->is_file) release_slot(cmd.id);
        ESP_LOGW(TAG, "scripting_submit: queue full");
        return SCRIPT_SUBMIT_QUEUE_FULL;
    }
    if (out_id) *out_id = cmd.id;
    return SCRIPT_SUBMIT_OK;
}

bool scripting_run_string(const char *src, size_t len, bool persist)
{
    ScriptSubmitOpts o = {};
    o.source  = SCRIPT_SRC_MANUAL;
    o.persist = persist;
    return scripting_submit(src, len, &o, NULL) == SCRIPT_SUBMIT_OK;
}


bool scripting_lint_string(const char *src, size_t len, char *out_err, size_t max_err)
{
    if (!s_enabled || !src || !s_queue) {
        if (out_err && max_err > 0) {
            snprintf(out_err, max_err, "Scripting engine not enabled");
        }
        return false;
    }

    // Allocate payload copy
    char *payload = (char *)malloc(len + 1);
    if (!payload) {
        if (out_err && max_err > 0) {
            snprintf(out_err, max_err, "Out of memory");
        }
        return false;
    }
    memcpy(payload, src, len);
    payload[len] = '\0';

    // Check if a script is currently running
    bool running = false;
    if (xSemaphoreTake(s_status_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        running = s_status.is_running;
        xSemaphoreGive(s_status_mutex);
    }
    if (running) {
        free(payload);
        if (out_err && max_err > 0) {
            snprintf(out_err, max_err, "Interpreter is busy running a script");
        }
        return false;
    }

    ScriptLintJob *job = (ScriptLintJob *)calloc(1, sizeof(ScriptLintJob));
    if (!job) {
        free(payload);
        if (out_err && max_err > 0) {
            snprintf(out_err, max_err, "Out of memory");
        }
        return false;
    }

    SemaphoreHandle_t sem = xSemaphoreCreateBinary();
    if (!sem) {
        free(payload);
        free(job);
        if (out_err && max_err > 0) {
            snprintf(out_err, max_err, "Failed to create semaphore");
        }
        return false;
    }

    job->refcount = 2; // 1 for caller, 1 for taskMicroPython
    job->sem      = sem;
    job->payload  = payload;
    job->len      = len;
    job->lint_ok  = false;

    if (out_err && max_err > 0) {
        out_err[0] = '\0';
    }

    ScriptCmd cmd = {};
    cmd.id        = 0;
    cmd.payload   = payload;
    cmd.len       = len;
    cmd.persist   = false;
    cmd.is_lint   = true;
    cmd.lint_job  = job;

    __atomic_add_fetch(&s_work_cmds, 1, __ATOMIC_SEQ_CST);
    if (xQueueSend(s_queue, &cmd, pdMS_TO_TICKS(100)) != pdTRUE) {
        __atomic_sub_fetch(&s_work_cmds, 1, __ATOMIC_SEQ_CST);
        job->payload = NULL; // prevent double-free
        free(payload);
        vSemaphoreDelete(sem);
        free(job);
        if (out_err && max_err > 0) {
            snprintf(out_err, max_err, "Scripting queue is full");
        }
        return false;
    }

    bool taken = (xSemaphoreTake(sem, pdMS_TO_TICKS(5000)) == pdTRUE);
    bool lint_ok = false;
    if (taken) {
        lint_ok = job->lint_ok;
        if (!lint_ok && out_err && max_err > 0) {
            snprintf(out_err, max_err, "%s", job->err);
        }
    } else {
        if (out_err && max_err > 0) {
            snprintf(out_err, max_err, "Interpreter timed out (5s)");
        }
    }

    lint_job_release(job);
    return lint_ok;
}


// ---------------------------------------------------------------------------
// scripting_reset_vm — request VM teardown from any context
// ---------------------------------------------------------------------------

void scripting_reset_vm(void)
{
    s_reset_requested = true;
}

// ---------------------------------------------------------------------------
// Aggregate work predicate (standby inhibitor) — see scripting.h
// ---------------------------------------------------------------------------

bool scripting_has_work(void)
{
    uint32_t deadlines[SCRIPT_XFER_COUNT];
    for (int i = 0; i < SCRIPT_XFER_COUNT; i++) {
        deadlines[i] = __atomic_load_n(&s_xfer_deadline[i], __ATOMIC_ACQUIRE);
    }
    uint32_t cmds = __atomic_load_n(&s_work_cmds, __ATOMIC_SEQ_CST);
    return sr_has_work(cmds, deadlines, SCRIPT_XFER_COUNT, now_ms());
}

void scripting_transfer_activity(ScriptXferSource src, bool active)
{
    if ((unsigned)src >= SCRIPT_XFER_COUNT) return;
    __atomic_store_n(&s_xfer_deadline[src], active ? sr_xfer_deadline(now_ms()) : 0u,
                     __ATOMIC_RELEASE);
}

// ---------------------------------------------------------------------------
// scripting_get_mp_session — read-only accessor for the session counter
// ---------------------------------------------------------------------------

uint8_t scripting_get_mp_session(void)
{
    return s_mp_session;
}

ScriptSubmitResult scripting_submit_file(const char *name, ScriptSource source, bool replace,
                                         uint32_t *out_id)
{
    if (!s_enabled || !name || !s_queue) return SCRIPT_SUBMIT_DISABLED;

    uint8_t *buf = (uint8_t *)heap_caps_malloc(SCRIPT_BODY_MAX + 1,
                                               MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) {
        ESP_LOGE(TAG, "scripting_submit_file: buf alloc failed");
        return SCRIPT_SUBMIT_QUEUE_FULL;
    }
    size_t file_len = SCRIPT_BODY_MAX;
    char err[80] = {0};
    if (!script_storage_read(name, buf, &file_len, err, sizeof(err)) || file_len == 0) {
        ESP_LOGE(TAG, "scripting_submit_file: read '%s' failed: %s", name, err);
        heap_caps_free(buf);
        return SCRIPT_SUBMIT_NOT_FOUND;
    }

    ScriptSubmitOpts o = {};
    o.name    = name;
    o.source  = source;
    o.is_file = true;
    o.replace = replace;
    // File-runs from a PERSISTENT VM stay persistent (V2-A contract).
    o.persist = (s_mode == SCRIPTING_MODE_PERSISTENT);
    ScriptSubmitResult r = scripting_submit((const char *)buf, file_len, &o, out_id);
    heap_caps_free(buf);
    return r;
}

bool scripting_run_file(const char *name, uint32_t *out_id)
{
    return scripting_submit_file(name, SCRIPT_SRC_MANUAL, false, out_id) == SCRIPT_SUBMIT_OK;
}

