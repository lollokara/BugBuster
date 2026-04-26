// Minimal HAL implementation for BugBuster MicroPython port.
// Phase 1: stdout tees to scripting_log_push; mp_hal_delay_ms polls stop flag.

#include "py/mpconfig.h"
#include "py/runtime.h"
#include "py/mpstate.h"
#include "py/gc.h"
#include "py/lexer.h"
#include "py/builtin.h"
#include "py/obj.h"
#include "py/mperrno.h"
#include "shared/readline/readline.h"
#include "mphalport.h"

// Forward declarations for scripting.cpp functions.
// Plain C forward-declare — no extern "C" wrapper needed in a .c file.
// Placed outside NO_QSTR guard because the types (const char*, size_t, bool)
// require no IDF headers.
extern void scripting_log_push(const char *str, size_t len);
extern bool scripting_stop_requested(void);

// IDF headers guarded so makeqstrdefs.py pp (which defines NO_QSTR) doesn't
// need the full IDF SDK on its include path. Real compilation sees everything.
#ifndef NO_QSTR
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_rom_sys.h"

// Spinlock for atomic sections (declared extern in mphalport.h)
portMUX_TYPE mp_atomic_mux = portMUX_INITIALIZER_UNLOCKED;

// ── Ticks (use mp_uint_t to match py/mphal.h declarations) ───────────────────
// Use FreeRTOS tick count for ms (avoids esp_timer dependency at Phase 0).
// 1 FreeRTOS tick = portTICK_PERIOD_MS ms (typically 1 ms at 1 kHz).
mp_uint_t mp_hal_ticks_ms(void) {
    return (mp_uint_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
}

// For us, use CCOUNT register (Xtensa cycle counter) / CPU frequency.
// esp_rom_get_cpu_ticks_per_us() gives ticks per microsecond.
mp_uint_t mp_hal_ticks_us(void) {
    uint32_t ccount;
    __asm__ __volatile__("rsr %0,ccount" : "=a"(ccount));
    return (mp_uint_t)(ccount / esp_rom_get_cpu_ticks_per_us());
}

// ── Delays ────────────────────────────────────────────────────────────────────
void mp_hal_delay_us(mp_uint_t us) {
    esp_rom_delay_us((uint32_t)us);
}

void mp_hal_delay_ms(mp_uint_t ms) {
    // Delay tick-by-tick so we can check the stop flag cooperatively.
    for (mp_uint_t i = 0; i < ms; i++) {
        vTaskDelay(1);
        // Only raise if an nlr_buf_t is on the stack (i.e. called from script context).
        // Without this guard, raising outside an nlr frame would crash the firmware.
        if (scripting_stop_requested() && MP_STATE_THREAD(nlr_top) != NULL) {
            mp_raise_msg(&mp_type_KeyboardInterrupt, MP_ERROR_TEXT("stopped"));
        }
    }
}
#endif // NO_QSTR

// ── stdout (return mp_uint_t to match py/mphal.h declaration) ────────────────
mp_uint_t mp_hal_stdout_tx_strn(const char *str, size_t len) {
    // Phase 1: tee to the scripting log ring (and stderr for IDF console).
    scripting_log_push(str, len);
    return len;
}

// ── stdin ─────────────────────────────────────────────────────────────────────
int mp_hal_stdin_rx_chr(void) {
    // Phase 1: no interactive input source (no REPL)
    return 0;
}

// ── Interrupt character ───────────────────────────────────────────────────────
void mp_hal_set_interrupt_char(int c) {
    (void)c;
}

// ── Phase 1 port stubs ────────────────────────────────────────────────────────
// These symbols are required by MicroPython core even with VFS/REPL disabled.
// They are placed outside NO_QSTR guards because they use only MP types, not
// IDF runtime types (FreeRTOS, esp_timer, etc.).

// gc_collect — simple mark-and-sweep without walking Xtensa register windows.
// The hermetic per-eval lifecycle (gc_init+mp_deinit each run) means the GC
// heap is fresh on every script; mid-script collection is best-effort.
void gc_collect(void) {
    gc_collect_start();
    // Walk the C call stack from the current stack pointer upward.
    // mp_uint_t regs[8] is a rough register spill; for Xtensa we can't use the
    // generic setjmp trick, so just scan from SP to the task stack top.
    volatile mp_uint_t dummy;
    gc_collect_root((void **)&dummy, ((mp_uint_t)MP_STATE_THREAD(stack_top) - (mp_uint_t)&dummy) / sizeof(mp_uint_t));
    gc_collect_end();
}

// gc_get_max_new_split — called by gc_try_add_heap when MICROPY_GC_SPLIT_HEAP=1.
// Return 0: we manage one fixed heap block and never auto-split.
size_t gc_get_max_new_split(void) {
    return 0;
}

// nlr_jump_fail — called when nlr_jump finds no nlr frame on the stack.
// Should never happen in a correctly written embedding; halt the uPython task.
void nlr_jump_fail(void *val) {
    (void)val;
    for (;;) {
        vTaskDelay(portMAX_DELAY);
    }
}

// mp_hal_stdio_poll — used by sys_stdio_mphal.c for stream.poll().
// No interactive stdio in Phase 1.
uintptr_t mp_hal_stdio_poll(uintptr_t poll_flags) {
    (void)poll_flags;
    return 0;
}

// mp_import_stat, mp_lexer_new_from_file, mp_builtin_open — Phase 1 stubs.
// When MICROPY_VFS=1 the extmod VFS layer provides real implementations of
// these symbols; defining them here would cause duplicate-symbol link errors.
#if !MICROPY_VFS

// mp_import_stat — filesystem stat for the import system.
// No VFS in Phase 1: all paths report "does not exist".
mp_import_stat_t mp_import_stat(const char *path) {
    (void)path;
    return MP_IMPORT_STAT_NO_EXIST;
}

// mp_lexer_new_from_file — called by builtinimport.c even with VFS disabled.
// Raise OSError: no filesystem available in Phase 1.
mp_lexer_t *mp_lexer_new_from_file(qstr filename) {
    mp_raise_OSError(MP_ENOENT);
    return NULL; // unreachable
}

// mp_builtin_open / mp_builtin_open_obj — port must define when MICROPY_VFS=0.
// Raise OSError: no filesystem in Phase 1.
mp_obj_t mp_builtin_open(size_t n_args, const mp_obj_t *args, mp_map_t *kwargs) {
    (void)n_args; (void)args; (void)kwargs;
    mp_raise_OSError(MP_ENOENT);
    return mp_const_none; // unreachable
}
MP_DEFINE_CONST_FUN_OBJ_KW(mp_builtin_open_obj, 1, mp_builtin_open);

#endif /* !MICROPY_VFS */

// readline — called by the input() builtin.
// Return -1 to signal EOF / unsupported in Phase 1.
int readline(vstr_t *line, const char *prompt) {
    (void)line; (void)prompt;
    return -1;
}
