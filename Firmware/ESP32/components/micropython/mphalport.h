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

// ── C-level pin HAL ──────────────────────────────────────────────────────────
#include "py/obj.h"
#define MP_HAL_PIN_FMT "%u"
#define mp_hal_pin_obj_t int
#define mp_hal_pin_name(pin) (pin)
// Real compilation uses the genuine ESP-IDF gpio types/prototypes. The old
// hand-rolled `typedef int gpio_num_t` stubs collided with the real enums in
// any translation unit that also pulls in driver/gpio.h — e.g.
// extmod/modmachine.c, which includes the esp32 port's lightsleep code (and
// hence soc/gpio_num.h + hal/gpio_types.h) once Bluetooth/coexistence is in
// the sdkconfig. Including driver/gpio.h here gives every micropython TU the
// same real types; the int stubs survive only for the NO_QSTR qstr-extraction
// pass, which is preprocessed without the IDF include paths. The Python-facing
// GPIO_MODE_* values are identical (INPUT=1, INPUT_OUTPUT=3, OUTPUT_OD=6,
// INPUT_OUTPUT_OD=7), so machine.Pin behaviour is unchanged.
#ifdef NO_QSTR
typedef int gpio_num_t;
typedef int gpio_mode_t;
enum {
    GPIO_MODE_DISABLE = 0,
    GPIO_MODE_INPUT = 1,
    GPIO_MODE_OUTPUT = 2,
    GPIO_MODE_INPUT_OUTPUT = 3,
    GPIO_MODE_OUTPUT_OD = 6,
    GPIO_MODE_INPUT_OUTPUT_OD = 7,
};
void gpio_set_direction(gpio_num_t gpio_num, gpio_mode_t mode);
void gpio_set_level(gpio_num_t gpio_num, int level);
int gpio_get_level(gpio_num_t gpio_num);
#else
#include "driver/gpio.h"
#endif
// esp_rom_gpio_pad_select_gpio has the same signature in esp_rom_gpio.h; declare
// it directly so we don't have to add the esp_rom include path to this component.
void esp_rom_gpio_pad_select_gpio(uint32_t gpio_num);
gpio_num_t machine_pin_get_id(mp_obj_t pin_in);
#define mp_hal_get_pin_obj(o) machine_pin_get_id(o)

static inline void mp_hal_pin_input(mp_hal_pin_obj_t pin) {
    esp_rom_gpio_pad_select_gpio(pin);
    gpio_set_direction(pin, GPIO_MODE_INPUT);
}
static inline void mp_hal_pin_output(mp_hal_pin_obj_t pin) {
    esp_rom_gpio_pad_select_gpio(pin);
    gpio_set_direction(pin, GPIO_MODE_INPUT_OUTPUT);
}
static inline void mp_hal_pin_open_drain(mp_hal_pin_obj_t pin) {
    esp_rom_gpio_pad_select_gpio(pin);
    gpio_set_direction(pin, GPIO_MODE_INPUT_OUTPUT_OD);
}
static inline void mp_hal_pin_od_low(mp_hal_pin_obj_t pin) {
    gpio_set_level(pin, 0);
}
static inline void mp_hal_pin_od_high(mp_hal_pin_obj_t pin) {
    gpio_set_level(pin, 1);
}
static inline int mp_hal_pin_read(mp_hal_pin_obj_t pin) {
    return gpio_get_level(pin);
}
static inline int mp_hal_pin_read_output(mp_hal_pin_obj_t pin) {
    return gpio_get_level(pin);
}
static inline void mp_hal_pin_write(mp_hal_pin_obj_t pin, int v) {
    gpio_set_level(pin, v);
}

// Declared outside NO_QSTR guard: uses no IDF types, needed by py/modmicropython.c
void mp_hal_set_interrupt_char(int c);

// V2-D: native exec pool — declared outside NO_QSTR (uses only void*/size_t).
// bb_native_code_commit: allocates EXEC SRAM, copies buf, tracks for free_all.
// bb_native_code_free_all: frees all tracked exec allocations (call after mp_deinit).
#include <stddef.h>
void *bb_native_code_commit(void *buf, size_t len, void *reloc);
void  bb_native_code_free_all(void);

// ── VFS POSIX support ────────────────────────────────────────────────────────
#include <errno.h>
#define MP_HAL_RETRY_SYSCALL(ret, syscall, raise) \
    do { \
        ret = syscall; \
    } while (ret == -1 && errno == EINTR); \
    if (ret == -1) { \
        int err = errno; \
        raise; \
    }

#include "poll.h"
static inline int poll(struct pollfd *fds, nfds_t nfds, int timeout) {
    (void)fds; (void)nfds; (void)timeout;
    return 0;
}

#endif // MICROPY_INCLUDED_BUGBUSTER_MPHALPORT_H
