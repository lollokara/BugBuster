"""DAQ-06: the Sample Rate setting advertised rates the hardware never runs at.

A: the registry offered 10k/50k/100k/250k/1M SPS, but with MCLK 16.384 MHz
(fMOD = MCLK/2) adaq7769_set_output_data_rate() lands on the nearest Wideband
decimation (or Sinc5 /16), so they really ran at 8k/64k/128k/256k/512k. The
Sinc5 /8 branch (1.024 MSPS) needs a target >= fMOD/6 and is unreachable from
the registry.

B: the registry advertises exactly what each option achieves; the MCP tool
accepts the real rates (and still maps the old nominal values onto the same
option). T3 (bench) measures each rate from stream sample counts.
"""

import re

import pytest

from tests.lib.srcread import REPO_ROOT

REG = (REPO_ROOT / "Firmware/DAQ_HAT/common/daq_config_registry.c").read_text(encoding="utf-8")
CFG = (REPO_ROOT / "Firmware/DAQ_HAT/ESP32P4/include/config.h").read_text(encoding="utf-8")


def _achieved(target: float, mclk: int) -> float:
    """Mirror of adaq7769_set_output_data_rate()'s selection (MCLK_DIV_2)."""
    fmod = mclk / 2
    if target >= fmod / 12:
        return fmod / 8 if target >= fmod / 6 else fmod / 16
    if target >= fmod / 1024:
        return min((fmod / d for d in (32, 64, 128, 256, 512, 1024)), key=lambda o: abs(o - target))
    return fmod / round(fmod / target)


def _table():
    body = re.search(r"DAQ_SAMPLE_RATE_SPS\[DAQ_SR_COUNT\]\s*=\s*\{([^}]*)\}", REG).group(1)
    return [int(x.rstrip("uU")) for x in re.findall(r"\d+u?", body)]


def _labels():
    body = re.search(r"OPT_SR\[\]\s*=\s*\{([^}]*)\}", REG).group(1)
    return re.findall(r'"([^"]+)"', body)


MCLK = int(re.search(r"#define\s+ADAQ_MCLK_HZ\s+(\d+)", CFG).group(1))


@pytest.mark.xfail(strict=True, reason="DAQ-06")
def test_every_advertised_rate_is_what_the_adc_runs_at():
    for sps in _table():
        assert _achieved(sps, MCLK) == sps, f"{sps} SPS advertised, {_achieved(sps, MCLK):.0f} achieved"


@pytest.mark.xfail(strict=True, reason="DAQ-06")
def test_labels_match_the_rates():
    for sps, label in zip(_table(), _labels(), strict=True):
        assert label == f"{sps // 1000} ksps", (sps, label)


@pytest.mark.xfail(strict=True, reason="DAQ-06")
def test_mcp_accepts_real_rates_and_legacy_aliases():
    from bugbuster_mcp.tools import daq_power
    assert list(daq_power._SAMPLE_RATES_SPS) == _table()
    for legacy, idx in ((10_000, 0), (250_000, 3), (1_000_000, 4)):
        assert daq_power._rate_index(legacy) == idx
    assert daq_power._rate_index(256_000) == 3
