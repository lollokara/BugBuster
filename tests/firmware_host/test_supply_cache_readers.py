"""PWR-10: the "cached" supply-voltage readers (BBP
SELFTEST_SUPPLY_VOLTAGES_CACHED, GET /api/selftest/supplies/cached and the
api_core status path) each ran a selftest monitor step - a U23 MUX switch plus
an ADC measurement of ~0.6 s - on the calling BBP/HTTP task before answering.

B: they return the cache; the main loop's 0.5 Hz background step is the only
sampler.
"""

import re

import pytest

from tests.firmware_host.fwhost import extract_function
from tests.lib.srcread import REPO_ROOT

SRC = REPO_ROOT / "Firmware/ESP32/src"


def _code(t):
    return re.sub(r"//[^\n]*", "", t)


@pytest.mark.parametrize("path,sig", [
    ("bbp/cmds/cmd_selftest.cpp", r"static int handler_selftest_supply_voltages_cached\("),
    ("web/webserver.cpp", r"static esp_err_t handle_get_selftest_supplies_cached\("),
])
def test_cached_supply_readers_do_not_measure(path, sig):
    assert "selftest_monitor_step" not in _code(extract_function(SRC / path, sig))


def test_api_core_does_not_measure_on_read():
    assert "selftest_monitor_step" not in _code((SRC / "net/api_core.cpp").read_text(encoding="utf-8"))
