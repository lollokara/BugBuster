"""PWR-16: every ds4424_set_voltage() committed the setpoint to NVS, so a
script or slider sweeping VADJ wrote flash on every step.

B: setpoints are coalesced (hal/ds4424_nvs_debounce.h) and written once after
the writes stop (one-shot timer); a value equal to what is stored is not
rewritten. Host-compiled debouncer + a static check on the driver.
"""

import re

import pytest

from tests.firmware_host.fwhost import compile_and_run, extract_function
from tests.lib.srcread import REPO_ROOT

HAL = REPO_ROOT / "Firmware/ESP32/src/hal"

MAIN = r"""
#include <stdio.h>
#include "ds4424_nvs_debounce.h"
static int writes = 0; static int last_mv = -1;
static void w(uint8_t ch, int32_t mv, void *u) { (void)ch; (void)u; writes++; last_mv = (int)mv; }
int main(void) {
    ds4424_nvs_debounce_t d; ds4424_nvs_debounce_init(&d);
    for (int i = 0; i < 10; i++) ds4424_nvs_debounce_mark(&d, 1, 3000 + i * 100);
    ds4424_nvs_debounce_flush(&d, w, NULL);
    int sweep = writes, sweep_last = last_mv;
    writes = 0;
    ds4424_nvs_debounce_mark(&d, 1, 3900);          /* same as stored */
    ds4424_nvs_debounce_flush(&d, w, NULL);
    int same = writes;
    writes = 0;
    ds4424_nvs_debounce_mark(&d, 0, 5000);
    ds4424_nvs_debounce_mark(&d, 2, 1200);
    ds4424_nvs_debounce_flush(&d, w, NULL);
    printf("sweep=%d last=%d same=%d two=%d\n", sweep, sweep_last, same, writes);
    return 0;
}
"""


@pytest.mark.xfail(strict=True, reason="PWR-16")
def test_debouncer_coalesces_a_sweep_into_one_write():
    out = compile_and_run(MAIN, cxx=False, include_dirs=[HAL]).strip()
    assert out == "sweep=1 last=3900 same=0 two=2", out


@pytest.mark.xfail(strict=True, reason="PWR-16")
def test_set_voltage_does_not_commit_nvs_inline():
    body = extract_function(HAL / "ds4424.cpp", r"bool ds4424_set_voltage\(")
    body = re.sub(r"//[^\n]*", "", body)
    assert "save_voltage_to_nvs" not in body
    assert "ds4424_nvs_debounce_mark" in body
