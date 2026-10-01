"""C6-20: the C6 menu resends every setting (c6_config_send, 20 keys) on each
edit, and the P4 settings store re-applied every one of them to hardware even
when the value had not changed - so changing the display brightness restarted
the acquisition (sample rate / range / filter re-applied).

B: an unchanged value arriving from the C6 is NOT re-applied. Every other
source (CLI, S3, local firmware) still re-applies an unchanged value, because
those callers rely on the re-assert (e.g. a retried `vdut on` after a USB-PD
refusal - M3.0 caller classification)."""

from pathlib import Path

import pytest

from tests.firmware_host.fwhost import compile_and_run

P4 = "Firmware/DAQ_HAT/ESP32P4/src/config"
COMMON = "Firmware/DAQ_HAT/common"

STUBS = {
    "nvs.h": r"""#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>   /* ESP-IDF headers pull this in transitively */
typedef int esp_err_t;
typedef uint32_t nvs_handle_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_NVS_NO_FREE_PAGES 0x110d
#define ESP_ERR_NVS_NEW_VERSION_FOUND 0x1110
typedef enum { NVS_READONLY, NVS_READWRITE } nvs_open_mode_t;
static inline esp_err_t nvs_open(const char *n, nvs_open_mode_t m, nvs_handle_t *h) { (void)n; (void)m; (void)h; return ESP_FAIL; }
static inline esp_err_t nvs_set_i32(nvs_handle_t h, const char *k, int32_t v) { (void)h; (void)k; (void)v; return ESP_OK; }
static inline esp_err_t nvs_get_i32(nvs_handle_t h, const char *k, int32_t *v) { (void)h; (void)k; (void)v; return ESP_FAIL; }
static inline esp_err_t nvs_set_str(nvs_handle_t h, const char *k, const char *v) { (void)h; (void)k; (void)v; return ESP_OK; }
static inline esp_err_t nvs_get_str(nvs_handle_t h, const char *k, char *v, size_t *l) { (void)h; (void)k; (void)v; (void)l; return ESP_FAIL; }
static inline esp_err_t nvs_commit(nvs_handle_t h) { (void)h; return ESP_OK; }
static inline void nvs_close(nvs_handle_t h) { (void)h; }
static inline esp_err_t nvs_erase_all(nvs_handle_t h) { (void)h; return ESP_OK; }
""",
    "nvs_flash.h": r"""#pragma once
#include "nvs.h"
static inline esp_err_t nvs_flash_init(void) { return ESP_OK; }
static inline esp_err_t nvs_flash_erase(void) { return ESP_OK; }
static inline const char *esp_err_to_name(esp_err_t e) { (void)e; return "err"; }
""",
    "freertos/semphr.h": r"""#pragma once
#include "freertos/FreeRTOS.h"
typedef void *SemaphoreHandle_t;
#define portMAX_DELAY 0xFFFFFFFFu
static inline SemaphoreHandle_t xSemaphoreCreateMutex(void) { static int m; return &m; }
static inline int xSemaphoreTake(SemaphoreHandle_t s, uint32_t t) { (void)s; (void)t; return 1; }
static inline int xSemaphoreGive(SemaphoreHandle_t s) { (void)s; return 1; }
""",
}

MAIN = r"""
#include <stdio.h>
#include "daq_settings.h"
static int g_apply = 0;
static void on_apply(uint16_t k, int32_t v, const char *s, void *u) { (void)k; (void)v; (void)s; (void)u; g_apply++; }
int main(void) {
    daq_settings_init();
    daq_settings_set_callbacks(on_apply, NULL, NULL, NULL);
    int32_t rate = 0; daq_settings_get_i32(DAQ_K_SAMPLE_RATE_IDX, &rate);
    int32_t other = rate == 0 ? 1 : 0;

    g_apply = 0;   // C6 resends an unchanged value (menu edit elsewhere)
    daq_settings_set_i32(DAQ_K_SAMPLE_RATE_IDX, rate, DAQ_SRC_C6);
    int c6_same = g_apply;

    g_apply = 0;   // C6 changes the value: must apply
    daq_settings_set_i32(DAQ_K_SAMPLE_RATE_IDX, other, DAQ_SRC_C6);
    int c6_changed = g_apply;

    g_apply = 0;   // CLI / S3 re-assert of the same value: must still apply
    daq_settings_set_i32(DAQ_K_SAMPLE_RATE_IDX, other, DAQ_SRC_S3);
    daq_settings_set_i32(DAQ_K_SAMPLE_RATE_IDX, other, DAQ_SRC_LOCAL);
    int reassert = g_apply;
    printf("c6_same=%d c6_changed=%d reassert=%d\n", c6_same, c6_changed, reassert);
    return 0;
}
"""


def _run(tmp_path: Path) -> str:
    for name, text in STUBS.items():
        p = tmp_path / name
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_text(text, encoding="utf-8")
    return compile_and_run(MAIN, cxx=False,
                           sources=[f"{P4}/daq_settings.c", f"{COMMON}/daq_config_registry.c"],
                           include_dirs=[tmp_path, P4, COMMON]).strip()


@pytest.mark.xfail(strict=True, reason="C6-20")
def test_unchanged_c6_resend_is_not_reapplied(tmp_path):
    assert _run(tmp_path).startswith("c6_same=0 ")


def test_changed_values_and_non_c6_reasserts_still_apply(tmp_path):
    out = _run(tmp_path)
    assert "c6_changed=1" in out and "reassert=2" in out, out
