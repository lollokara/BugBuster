"""AN-04: the BBP CMD_SET_DAC_VOLTAGE path only runs
setVoutRangePreservingOutput() (park output + range settle) when the
unipolar/bipolar range actually changes. tasks_apply_dac_voltage() - used by
HTTP/API, MicroPython and the CLI - ran it on EVERY write, so each same-range
HTTP write parked the output and waited out the settle (and restarted ADC
conversion). Both paths must share the guard."""

import re
from pathlib import Path

import pytest

TASKS = Path(__file__).resolve().parents[2] / "Firmware" / "ESP32" / "src" / "tasks.cpp"


def _body(sig: str) -> str:
    text = TASKS.read_text(encoding="utf-8")
    start = text.index(sig)
    i = text.index("{", start)
    depth = 0
    for j in range(i, len(text)):
        depth += {"{": 1, "}": -1}.get(text[j], 0)
        if depth == 0:
            return text[i:j + 1]
    raise AssertionError(sig)


GUARD = re.compile(r"dacBipolar\s*!=\s*bipolar")


def test_bbp_path_has_guard():
    assert GUARD.search(_body("case CMD_SET_DAC_VOLTAGE:"))


@pytest.mark.xfail(strict=True, reason="AN-04")
def test_api_path_has_guard():
    body = _body("bool tasks_apply_dac_voltage(")
    assert GUARD.search(body), "tasks_apply_dac_voltage re-parks the output on every write"
