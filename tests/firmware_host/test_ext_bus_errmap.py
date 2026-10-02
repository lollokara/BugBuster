"""BUS-017: an I2C NACK must not be reported as TIMEOUT.

A: every failed ext_i2c_* / ext_spi_transfer returned -CMD_ERR_TIMEOUT, so an
agent could not tell "no device" (NACK, ESP_FAIL) from "bus stuck"
(ESP_ERR_TIMEOUT) or "another client holds the bus" (mutex timeout).
B: ext_bus keeps the esp_err_t of the calling task's last transfer and
bus/ext_bus_errmap.h maps it to a CmdError.
"""
from __future__ import annotations

import re

import pytest

from tests.firmware_host.fwhost import compile_and_run
from tests.lib.srcread import REPO_ROOT

SRC = REPO_ROOT / "Firmware/ESP32/src"

MAIN = r"""
#include <stdio.h>
#include "esp_err.h"
#include "ext_bus_errmap.h"
int main(void) {
    printf("ok=%d nack=%d stuck=%d busy=%d state=%d arg=%d other=%d\n",
           ext_bus_cmd_error(ESP_OK), ext_bus_cmd_error(ESP_FAIL),
           ext_bus_cmd_error(ESP_ERR_TIMEOUT), ext_bus_cmd_error(EXT_BUS_ERR_MUTEX),
           ext_bus_cmd_error(ESP_ERR_INVALID_STATE), ext_bus_cmd_error(ESP_ERR_INVALID_ARG),
           ext_bus_cmd_error(0x1234));
    return 0;
}
"""


def test_error_map_distinguishes_nack_timeout_and_busy():
    out = compile_and_run(MAIN, cxx=False, include_dirs=[SRC / "bus", SRC / "bbp"]).strip()
    # CmdError: HARDWARE=5, TIMEOUT=9, BUSY=4, INVALID_STATE=8, BAD_ARG=1
    assert out == "ok=0 nack=5 stuck=9 busy=4 state=8 arg=1 other=5", out


def test_bbp_handlers_no_longer_collapse_to_timeout():
    body = (SRC / "bbp/cmds/cmd_ext_bus.cpp").read_text(encoding="utf-8")
    assert not re.search(r"\?\s*-CMD_ERR_TIMEOUT\s*:", body)
    assert body.count("ext_bus_cmd_error(") >= 4
