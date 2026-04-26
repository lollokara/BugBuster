// Minimal HAL header for BugBuster MicroPython port (Phase 0)

#ifndef MICROPY_INCLUDED_BUGBUSTER_MPHALPORT_H
#define MICROPY_INCLUDED_BUGBUSTER_MPHALPORT_H

// All IDF/FreeRTOS headers are guarded behind NO_QSTR so makeqstrdefs.py pp
// can preprocess this file without needing the full IDF SDK include paths.
// Real compilation never defines NO_QSTR, so the full definitions are used.
#ifndef NO_QSTR

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_rom_sys.h"

// ── Atomic section (uses FreeRTOS spinlock) ───────────────────────────────────
extern portMUX_TYPE mp_atomic_mux;

static inline mp_uint_t mp_begin_atomic_section(void) {
    portENTER_CRITICAL(&mp_atomic_mux);
    return 0;
}

static inline void mp_end_atomic_section(mp_uint_t state) {
    (void)state;
    portEXIT_CRITICAL(&mp_atomic_mux);
}

#define MICROPY_BEGIN_ATOMIC_SECTION()      mp_begin_atomic_section()
#define MICROPY_END_ATOMIC_SECTION(state)   mp_end_atomic_section(state)

// ── mp_hal_ticks_cpu — inline asm, defined here to suppress mphal.h default ──
// Use a #define sentinel so mphal.h's #ifndef guard skips its own declaration.
__attribute__((always_inline)) static inline mp_uint_t mp_hal_ticks_cpu_impl(void) {
    uint32_t ccount;
    __asm__ __volatile__("rsr %0,ccount" : "=a"(ccount));
    return (mp_uint_t)ccount;
}
#define mp_hal_ticks_cpu() mp_hal_ticks_cpu_impl()

// ── Quiet timing ──────────────────────────────────────────────────────────────
#define mp_hal_quiet_timing_enter()         MICROPY_BEGIN_ATOMIC_SECTION()
#define mp_hal_quiet_timing_exit(irq_state) MICROPY_END_ATOMIC_SECTION(irq_state)

// ── fast delay ───────────────────────────────────────────────────────────────
#define mp_hal_delay_us_fast(us) esp_rom_delay_us(us)

#endif // NO_QSTR

// Declared outside NO_QSTR guard: uses no IDF types, needed by py/modmicropython.c
void mp_hal_set_interrupt_char(int c);

#endif // MICROPY_INCLUDED_BUGBUSTER_MPHALPORT_H
