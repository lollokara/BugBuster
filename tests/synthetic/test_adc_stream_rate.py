"""AN-10 / AN-11: the BBP ADC stream advertised the converter rate (9.6 kSPS)
but delivered ~18 samples/s on hardware: the conversion sequence still
included the slow diagnostic slots, the ``div`` argument was echoed but never
applied, and the poll task slept on a 1 ms tick instead of waking on ADC_RDY.

T3 metric (tmp script, recorded in CHANGELOG): VIN ch0 at 9.6 kSPS, div 1."""

import re
from pathlib import Path

import pytest

SRC = Path(__file__).resolve().parents[2] / "Firmware" / "ESP32" / "src"
BBP = (SRC / "bbp" / "bbp.cpp")
TASKS = (SRC / "tasks.cpp")


def _fn(text: str, sig: str) -> str:
    start = text.index(sig)
    return text[start: text.index("\n}\n", start)]


@pytest.mark.xfail(strict=True, reason="AN-10")
def test_stream_start_drops_diagnostic_slots():
    src = BBP.read_text(encoding="utf-8")
    assert "tasks_scope_mode_enter" in _fn(src, "void bbpStartAdcStream(")
    assert "tasks_scope_mode_exit" in _fn(src, "void bbpStopAdcStream(")


@pytest.mark.xfail(strict=True, reason="AN-10")
def test_stream_divider_is_applied():
    src = BBP.read_text(encoding="utf-8")
    push = _fn(src, "void bbpPushAdcSample(")
    assert re.search(r"s_adcStreamDiv", push), "producer ignores div"


@pytest.mark.xfail(strict=True, reason="AN-11")
def test_adc_poll_wakes_on_adc_rdy():
    src = TASKS.read_text(encoding="utf-8")
    assert "PIN_ADC_RDY" in src and "gpio_isr_handler_add" in src
    poll = _fn(src, "static void taskAdcPoll(")
    assert "ulTaskNotifyTake" in poll
