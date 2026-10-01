"""IO-2 / IO-9: ADGS2414D driver (hal/adgs2414d.cpp) compiled on the host against
a modelled 5-device daisy chain (each frame latches the clocked-in bytes and
SDO returns the previously latched bytes).

IO-2: break-before-make was defeated. After the open-all ("break") frame the
driver verified it with a readback frame that clocks out the SHADOW state -
which still held the OLD closed switches - so the old route was re-latched
for the whole dead time before the new one closed.
B: no latched state between the open-all frame and the target re-closes a
switch the transition was meant to open.

IO-9: a 200 ms SPI-bus mutex timeout silently skipped the transfer; verify
then failed and the driver declared MUX FAULT and zeroed its shadow, although
the hardware was never touched.
B: a bus timeout is not a hardware fault - fault flag stays clear, shadow
unchanged, and the write reports failure."""

from pathlib import Path

import pytest

from tests.firmware_host.fwhost import compile_and_run, extract_defines

DRV = "Firmware/ESP32/src/hal/adgs2414d.cpp"

STUBS = {
    "config.h": r"""#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <assert.h>
#include "driver/gpio.h"
#define AD74416H_DEV_ADDR 0
#define SPI_CLOCK_HZ 1000000
#define PIN_MUX_CS GPIO_NUM_21
#define PIN_LSHIFT_OE GPIO_NUM_14
#define PIN_SDO GPIO_NUM_1
#define PIN_SDI GPIO_NUM_2
#define PIN_SYNC GPIO_NUM_3
#define PIN_SCLK GPIO_NUM_4
CONFIG_DEFINES
static inline void delay_ms(uint32_t) {}
static inline void delay_us(uint32_t) {}
""",
    "driver/gpio.h": r"""#pragma once
typedef int esp_err_t;
#define ESP_OK 0
typedef enum { GPIO_NUM_1 = 1, GPIO_NUM_2, GPIO_NUM_3, GPIO_NUM_4, GPIO_NUM_14 = 14, GPIO_NUM_21 = 21 } gpio_num_t;
typedef enum { GPIO_MODE_INPUT_OUTPUT = 3 } gpio_mode_t;
static inline esp_err_t gpio_reset_pin(gpio_num_t) { return ESP_OK; }
static inline esp_err_t gpio_set_direction(gpio_num_t, gpio_mode_t) { return ESP_OK; }
static inline esp_err_t gpio_set_level(gpio_num_t, int) { return ESP_OK; }
static inline int gpio_get_level(gpio_num_t) { return 1; }
static inline const char *esp_err_to_name(esp_err_t) { return "err"; }
""",
    "driver/spi_master.h": r"""#pragma once
#include <stddef.h>
#include <stdint.h>
#include "driver/gpio.h"
typedef struct fake_spi_dev *spi_device_handle_t;
typedef enum { SPI2_HOST = 1 } spi_host_device_t;
typedef struct { int clock_speed_hz; int mode; int spics_io_num; int queue_size; } spi_device_interface_config_t;
typedef struct { size_t length; const void *tx_buffer; void *rx_buffer; } spi_transaction_t;
esp_err_t spi_bus_add_device(spi_host_device_t, const spi_device_interface_config_t *, spi_device_handle_t *);
esp_err_t spi_device_polling_transmit(spi_device_handle_t, spi_transaction_t *);
""",
    "freertos/semphr.h": r"""#pragma once
#include "freertos/FreeRTOS.h"
typedef void *SemaphoreHandle_t;
#define pdTRUE 1
#define pdFALSE 0
extern int g_take_fail;
static inline SemaphoreHandle_t xSemaphoreCreateRecursiveMutex(void) { static int m; return &m; }
static inline int xSemaphoreTakeRecursive(SemaphoreHandle_t, uint32_t) { return g_take_fail ? pdFALSE : pdTRUE; }
static inline int xSemaphoreGiveRecursive(SemaphoreHandle_t) { return pdTRUE; }
""",
}

MAIN = r"""
#include <stdio.h>
#include <string.h>
#include "adgs2414d.h"
#include "driver/spi_master.h"
int g_take_fail = 0;
static uint8_t g_latched[ADGS_NUM_DEVICES];
static uint8_t g_hist[64][ADGS_NUM_DEVICES]; static int g_nh = 0;
struct fake_spi_dev { int x; };
static fake_spi_dev g_dev;
esp_err_t spi_bus_add_device(spi_host_device_t, const spi_device_interface_config_t *, spi_device_handle_t *h) { *h = &g_dev; return ESP_OK; }
esp_err_t spi_device_polling_transmit(spi_device_handle_t, spi_transaction_t *t) {
    size_t n = t->length / 8;
    const uint8_t *tx = (const uint8_t *)t->tx_buffer; uint8_t *rx = (uint8_t *)t->rx_buffer;
    if (n != ADGS_NUM_DEVICES) return ESP_OK;              // 2-byte daisy-entry frame
    for (size_t k = 0; k < n; k++) rx[k] = g_latched[n - 1 - k];
    for (size_t i = 0; i < n; i++) g_latched[i] = tx[n - 1 - i];
    if (g_nh < 64) memcpy(g_hist[g_nh++], g_latched, ADGS_NUM_DEVICES);
    return ESP_OK;
}
// IO-2: after the first frame with device `dev` fully open in `group`,
// does any later frame before the target re-close the old bit?
static int reclosed(int start, int dev, uint8_t old_bit, uint8_t target) {
    int opened = -1;
    for (int i = start; i < g_nh; i++) {
        if (opened < 0 && (g_hist[i][dev] & old_bit) == 0) { opened = i; continue; }
        if (opened >= 0 && g_hist[i][dev] == target) return 0;
        if (opened >= 0 && (g_hist[i][dev] & old_bit)) return 1;
    }
    return opened < 0 ? 2 : 0;
}
int main(void) {
    adgs_init();
    uint8_t s[4] = { 0x01, 0, 0, 0 };                    // dev0 S1 closed
    adgs_set_all_safe(s);
    int start = g_nh;
    uint8_t t[4] = { 0x04, 0, 0, 0 };                    // move to S3 (same group)
    adgs_set_all_safe(t);
    printf("all_safe reclose=%d final=0x%02X\n", reclosed(start, 0, 0x01, 0x04), g_latched[0]);

    start = g_nh;
    adgs_set_switch_safe(1, 1, true);                    // dev1 S2
    adgs_set_switch_safe(1, 3, true);                    // dev1 S4, same group -> S2 must open first
    int r2 = 0;
    for (int i = start; i < g_nh; i++) if (g_hist[i][1] == 0x08) { r2 = 0; break; }
    r2 = reclosed(start + 1, 1, 0x02, 0x08);
    printf("switch_safe reclose=%d final=0x%02X\n", r2, g_latched[1]);

    // IO-9: bus mutex unavailable for the whole write.
    uint8_t before = adgs_get_state(0);
    g_take_fail = 1;
    uint8_t u[4] = { 0x10, 0, 0, 0 };
    bool ok = adgs_set_all_safe(u);
    g_take_fail = 0;
    printf("busy ok=%d faulted=%d shadow_kept=%d\n", ok, adgs_is_faulted(), adgs_get_state(0) == before);
    return 0;
}
"""


def _run(tmp_path: Path) -> list[str]:
    cfg = extract_defines("Firmware/ESP32/src/config.h", [
        "ADGS_NUM_DEVICES", "ADGS_MAIN_DEVICES", "ADGS_SELFTEST_DEV", "ADGS_DEAD_TIME_MS",
        "ADGS_NUM_SWITCHES",
        "ADGS_HAS_SELFTEST", "U17_DEVICE_IDX", "U17_S3_MASK", "U23_SW_ADC_CH_D"])
    for name, text in STUBS.items():
        p = tmp_path / name
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_text(text.replace("CONFIG_DEFINES", cfg), encoding="utf-8")
    out = compile_and_run(MAIN, sources=[DRV], cxx=True, defines=["BB_IO_OWNERSHIP=1"],
                          include_dirs=[tmp_path, "Firmware/ESP32/src/hal"])
    return out.strip().splitlines()


@pytest.mark.xfail(strict=True, reason="IO-2")
def test_set_all_break_before_make(tmp_path):
    assert _run(tmp_path)[0] == "all_safe reclose=0 final=0x04"


@pytest.mark.xfail(strict=True, reason="IO-2")
def test_set_switch_break_before_make(tmp_path):
    assert _run(tmp_path)[1] == "switch_safe reclose=0 final=0x08"


@pytest.mark.xfail(strict=True, reason="IO-9")
def test_bus_timeout_is_not_a_mux_fault(tmp_path):
    assert _run(tmp_path)[2] == "busy ok=0 faulted=0 shadow_kept=1"
