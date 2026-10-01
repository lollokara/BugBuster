"""AN-12: CMD_ADC_CONFIG held the shared SPI bus mutex across delay_ms(20)
after restarting conversions, so every other SPI user (ADC poll, DAC writes,
MUX) stalled ~25 ms per ADC reconfiguration.

B: the 5 ms stop settle stays under the lock (the part must be idle before
config writes); the 20 ms first-conversion wait runs with the bus released,
and the lock is taken again only for clearAllAlerts().
"""

import re

import pytest

from tests.lib.srcread import REPO_ROOT

SRC = (REPO_ROOT / "Firmware/ESP32/src/tasks.cpp").read_text(encoding="utf-8")


def _case(label: str) -> str:
    start = SRC.index(f"case {label}:")
    end = SRC.index("// ----", start)
    return re.sub(r"//[^\n]*", "", SRC[start:end])


@pytest.mark.xfail(strict=True, reason="AN-12")
def test_adc_config_does_not_hold_the_spi_bus_across_the_20ms_settle():
    body = _case("CMD_ADC_CONFIG")
    give = body.index("xSemaphoreGiveRecursive(g_spi_bus_mutex)")
    settle = body.index("delay_ms(20)")
    assert give < settle, "SPI bus still held during delay_ms(20)"
