// BugBuster minimal machine-module port include.
//
// Included textually by extmod/modmachine.c via MICROPY_PY_MACHINE_INCLUDEFILE.
// It is NEVER compiled standalone — do not add it to the component SRCS.
//
// Why this file exists (and lives in the component dir, not the micropython
// submodule): the upstream ports/esp32/modmachine.c registers the full ESP32
// machine surface (Timer, RTC, TouchPad, hardware I2C/SPI, RTC-config-driven
// light/deep sleep) and its globals table references machine_timer_type /
// machine_rtc_type / machine_touchpad_type / machine_rtc_config — none of which
// this minimal port implements (scripts use the bugbuster.* modules for bus
// access). Pointing MICROPY_PY_MACHINE_INCLUDEFILE at the upstream file made the
// firmware fail to LINK on a clean build. Previous fixes edited the upstream
// file in-place, but that submodule keeps resetting to pristine and the edits
// are lost — re-surfacing the same link break. Keeping the trimmed port here, in
// the version-controlled component directory, makes it survive submodule resets.
//
// Exposed: machine.Pin (custom machine_pin.c), reset / reset_cause, unique_id,
// freq (read-only), idle, and a minimal light/deep sleep. Everything else is
// intentionally omitted.

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_sleep.h"
#include "esp_mac.h"

// machine.Pin is implemented by our custom machine_pin.c.
extern const mp_obj_type_t machine_pin_type;

// This minimal port has no dedicated hardware I2C/SPI peripheral drivers, but we
// still expose `machine.I2C` / `machine.SPI` (in addition to the SoftI2C/SoftSPI
// classes) by aliasing the hardware type names the shared machine-module globals
// table references to the software implementations. This keeps `machine.I2C(...)`
// / `machine.SPI(...)` working as bit-banged buses — matching the documented
// behaviour — while resolving the otherwise-undefined machine_i2c_type /
// machine_spi_type symbols. (Scoped to this translation unit only.)
extern const mp_obj_type_t mp_machine_soft_i2c_type;
extern const mp_obj_type_t mp_machine_soft_spi_type;
#define machine_i2c_type mp_machine_soft_i2c_type
#define machine_spi_type mp_machine_soft_spi_type

// Extra entries merged into the machine module's globals table (expanded at the
// end of extmod/modmachine.c's table). Pin only — no Timer/RTC/TouchPad.
#define MICROPY_PY_MACHINE_EXTRA_GLOBALS \
    { MP_ROM_QSTR(MP_QSTR_Pin), MP_ROM_PTR(&machine_pin_type) },

// ── Low-level helpers required by extmod/modmachine.c ───────────────────────

static void mp_machine_idle(void) {
    taskYIELD();
}

MP_NORETURN static void mp_machine_reset(void) {
    esp_restart();
}

static mp_int_t mp_machine_reset_cause(void) {
    return (mp_int_t)esp_reset_reason();
}

static mp_obj_t mp_machine_unique_id(void) {
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    return mp_obj_new_bytes(mac, sizeof(mac));
}

static mp_obj_t mp_machine_get_freq(void) {
    // Runtime CPU-frequency scaling is not supported here, so the configured
    // default is authoritative.
    return mp_obj_new_int(CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ * 1000000);
}

static void mp_machine_set_freq(size_t n_args, const mp_obj_t *args) {
    (void)n_args;
    (void)args;
    mp_raise_NotImplementedError(MP_ERROR_TEXT("setting machine.freq() is not supported"));
}

static void mp_machine_lightsleep(size_t n_args, const mp_obj_t *args) {
    esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
    if (n_args != 0) {
        esp_sleep_enable_timer_wakeup((uint64_t)mp_obj_get_int(args[0]) * 1000ULL);
    }
    esp_light_sleep_start();
}

MP_NORETURN static void mp_machine_deepsleep(size_t n_args, const mp_obj_t *args) {
    esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
    if (n_args != 0) {
        esp_sleep_enable_timer_wakeup((uint64_t)mp_obj_get_int(args[0]) * 1000ULL);
    }
    esp_deep_sleep_start();
}
