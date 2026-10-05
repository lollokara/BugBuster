"""PWR-02: an e-fuse switched on into a short asserts FLT immediately. That
edge lands inside the soft-start blackout and is suppressed, and nothing ever
looks again: check_changes() only reacts to FLT *edges*, the FLT level never
changes afterwards, and the poll task wakes only on the INT edge or a 2 s
fallback that again sees no edge. The short is never reported or
auto-disabled.

B: when a FLT is suppressed, a one-shot deadline wake is armed for the end of
the blackout; at that point FLT is evaluated as a LEVEL and the trip path runs
(event + auto-disable) within blackout + 10 ms.

Compiles the real hal/pca9535.cpp against a fake clock, fake I2C register file
and a fake esp_timer."""

from pathlib import Path

from tests.firmware_host.fwhost import compile_and_run

PCA = "Firmware/ESP32/src/hal/pca9535.cpp"

STUBS = {
    "power/standby_hw.h": "#pragma once\n#include <stdbool.h>\nstatic inline bool standby_hw_bus_gate_open(void) { return true; }\nstatic inline bool standby_hw_power_transition(void) { return false; }\n",
    "config.h": r"""#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "driver/gpio.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#define PCA9535_I2C_ADDR 0x23
#define PIN_MUX_INT GPIO_NUM_3
extern uint32_t g_now;
static inline void delay_ms(uint32_t ms) { g_now += ms; }
static inline uint32_t millis_now(void) { return g_now; }
""",
    "hat.h": "#pragma once\nvoid hat_update_leds(void);\n",
    "selftest.h": "#pragma once\nstatic inline bool selftest_is_busy(void) { return false; }\n",
    "i2c_bus.h": r"""#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
bool i2c_bus_ready(void);
bool i2c_bus_probe(uint8_t addr);
bool i2c_bus_write(uint8_t addr, const uint8_t *buf, size_t len, uint32_t timeout_ms);
bool i2c_bus_write_read(uint8_t addr, const uint8_t *w, size_t wl, uint8_t *r, size_t rl, uint32_t timeout_ms);
""",
    "driver/gpio.h": r"""#pragma once
#include <stdint.h>
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_ERR_INVALID_STATE 0x103
#define IRAM_ATTR
typedef enum { GPIO_NUM_NC = -1, GPIO_NUM_3 = 3 } gpio_num_t;
typedef enum { GPIO_MODE_INPUT = 1 } gpio_mode_t;
typedef enum { GPIO_PULLUP_ENABLE = 1 } gpio_pullup_t;
typedef enum { GPIO_INTR_NEGEDGE = 2 } gpio_int_type_t;
typedef struct { uint64_t pin_bit_mask; gpio_mode_t mode; gpio_pullup_t pull_up_en; int pull_down_en; gpio_int_type_t intr_type; } gpio_config_t;
static inline esp_err_t gpio_config(const gpio_config_t *) { return ESP_OK; }
static inline esp_err_t gpio_install_isr_service(int) { return ESP_OK; }
static inline esp_err_t gpio_isr_handler_add(gpio_num_t, void (*)(void *), void *) { return ESP_OK; }
static inline const char *esp_err_to_name(esp_err_t) { return "err"; }
""",
    "esp_timer.h": r"""#pragma once
#include <stdint.h>
#include "driver/gpio.h"
typedef struct fake_timer *esp_timer_handle_t;
typedef enum { ESP_TIMER_TASK = 0 } esp_timer_dispatch_t;
typedef struct { void (*callback)(void *); void *arg; esp_timer_dispatch_t dispatch_method; const char *name; bool skip_unhandled_events; } esp_timer_create_args_t;
esp_err_t esp_timer_create(const esp_timer_create_args_t *args, esp_timer_handle_t *out);
esp_err_t esp_timer_start_once(esp_timer_handle_t t, uint64_t timeout_us);
esp_err_t esp_timer_stop(esp_timer_handle_t t);
int64_t esp_timer_get_time(void);
""",
    "freertos/task.h": r"""#pragma once
#include "freertos/FreeRTOS.h"
typedef void *TaskHandle_t;
typedef int BaseType_t;
#define pdFALSE 0
#define pdTRUE 1
#define pdPASS 1
#define portYIELD_FROM_ISR(x) ((void)(x))
extern int g_notified;
static inline void vTaskNotifyGiveFromISR(TaskHandle_t, BaseType_t *) { g_notified = 1; }
static inline BaseType_t xTaskNotifyGive(TaskHandle_t) { g_notified = 1; return pdPASS; }
static inline uint32_t ulTaskNotifyTake(BaseType_t, uint32_t) { return 0; }
static inline BaseType_t xTaskCreatePinnedToCore(void (*)(void *), const char *, uint32_t, void *, int, TaskHandle_t *h, int) { static int t; *h = &t; return pdPASS; }
""",
}

MAIN = r"""
#include <stdio.h>
#include <string.h>
#include "pca9535.h"
#include "ds4424.h"
#include "esp_timer.h"
uint32_t g_now = 1000;
int g_notified = 0;
static uint8_t g_regs[256];
static bool g_short = false;           // EFUSE1 output shorted -> FLT_1 low
void hat_update_leds(void) {}
bool i2c_bus_ready(void) { return true; }
bool i2c_bus_probe(uint8_t) { return true; }
bool i2c_bus_write(uint8_t, const uint8_t *b, size_t n, uint32_t) { if (n >= 2) g_regs[b[0]] = b[1]; return true; }
bool i2c_bus_write_read(uint8_t, const uint8_t *w, size_t, uint8_t *r, size_t, uint32_t) {
    uint8_t reg = w[0];
    if (reg == 0x01) {                  // INPUT1: all FLT high (ok) unless shorted
        uint8_t v = 0xAA;
        if (g_short && (g_regs[0x03] & 0x01)) v &= (uint8_t)~0x02;
        *r = v; return true;
    }
    if (reg == 0x00) { *r = 0x12; return true; }   // both PG good
    *r = g_regs[reg]; return true;
}
static DS4424State g_ds = {};
const DS4424State *ds4424_get_state(void) { return &g_ds; }
bool ds4424_set_voltage(uint8_t, float) { return true; }

static void (*g_tcb)(void *) = nullptr; static void *g_targ = nullptr;
static int64_t g_deadline_us = -1;
struct fake_timer { int x; };
static fake_timer g_timer;
esp_err_t esp_timer_create(const esp_timer_create_args_t *a, esp_timer_handle_t *out) { g_tcb = a->callback; g_targ = a->arg; *out = &g_timer; return ESP_OK; }
esp_err_t esp_timer_start_once(esp_timer_handle_t, uint64_t us) { g_deadline_us = (int64_t)g_now * 1000 + (int64_t)us; return ESP_OK; }
esp_err_t esp_timer_stop(esp_timer_handle_t) { g_deadline_us = -1; return ESP_OK; }
int64_t esp_timer_get_time(void) { return (int64_t)g_now * 1000; }

static int g_trips = 0; static uint32_t g_trip_at = 0;
static void on_fault(const PcaFaultEvent *e) {
    if (e->type == PCA_FAULT_EFUSE_TRIP && e->channel == 0) { g_trips++; g_trip_at = g_now; }
}

int main(void) {
    pca9535_init();
    pca9535_install_isr();
    pca9535_register_fault_callback(on_fault);
    g_short = true;
    uint32_t t0 = g_now;
    pca9535_user_arm_efuse(0, true);
    g_now += 1; pca9535_update();       // INT edge from FLT asserting -> suppressed
    for (int ms = 0; ms < 400 && !g_trips; ms++) {
        g_now += 1;
        if (g_deadline_us >= 0 && (int64_t)g_now * 1000 >= g_deadline_us) {
            g_deadline_us = -1;
            if (g_tcb) g_tcb(g_targ);
        }
        if (g_notified) { g_notified = 0; pca9535_update(); }
    }
    const PCA9535State *st = pca9535_get_state();
    printf("trips=%d latency=%u efuse_en=%d\n", g_trips, g_trips ? g_trip_at - t0 : 0u, st->efuse_en[0]);
    return 0;
}
"""


def _run(tmp_path: Path) -> dict[str, int]:
    for name, text in STUBS.items():
        p = tmp_path / name
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_text(text, encoding="utf-8")
    out = compile_and_run(MAIN, sources=[PCA], cxx=True,
                          include_dirs=[tmp_path, "Firmware/ESP32/src/hal"])
    return {k: int(v) for k, v in (kv.split("=") for kv in out.split())}


def test_short_present_at_enable_trips_after_blackout(tmp_path):
    r = _run(tmp_path)
    assert r["trips"] == 1, r
    assert r["latency"] <= 110, r          # blackout (100 ms) + 10 ms
    assert r["efuse_en"] == 0, r           # auto-disabled
