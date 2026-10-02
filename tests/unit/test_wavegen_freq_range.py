"""AN-08: one waveform frequency range (0.1-100 Hz) on every surface.

A: the firmware BBP handler rejects < 0.1 Hz and the wavegen task clamps to
0.1 Hz, but the HTTP route, Python, MCP, desktop and web accepted 0.01 Hz, so
0.05 Hz failed over USB and silently ran at 0.1 Hz over HTTP."""
from __future__ import annotations

import re
from unittest.mock import MagicMock

import pytest

import bugbuster as bb
from bugbuster.constants import WaveformType
from tests.lib.srcread import REPO_ROOT


def _src(rel: str) -> str:
    return (REPO_ROOT / rel).read_text(encoding="utf-8")


def test_firmware_bbp_minimum_is_0_1_hz():
    assert "freq_hz < 0.1f" in _src("Firmware/ESP32/src/bbp/cmds/cmd_streaming.cpp")


@pytest.mark.xfail(strict=True, reason="AN-08")
def test_http_route_rejects_below_0_1_hz():
    body = _src("Firmware/ESP32/src/web/webserver.cpp")
    assert re.search(r"freq\s*<\s*0\.1\s*\|\|\s*freq\s*>\s*100", body)


@pytest.mark.xfail(strict=True, reason="AN-08")
def test_python_client_rejects_0_05_hz():
    client = bb.BugBuster.__new__(bb.BugBuster)
    client._usb = True
    client._usb_cmd = MagicMock()
    with pytest.raises(ValueError):
        client.start_waveform(0, WaveformType.SINE, freq_hz=0.05, amplitude=1.0)
    client._usb_cmd.assert_not_called()


@pytest.mark.xfail(strict=True, reason="AN-08")
def test_mcp_rejects_0_05_hz():
    from bugbuster_mcp.tools.waveform import start_waveform
    with pytest.raises(ValueError, match="0.1-100"):
        start_waveform(io=3, waveform="sine", freq_hz=0.05, amplitude=1.0)


@pytest.mark.xfail(strict=True, reason="AN-08")
def test_desktop_and_web_clamp_to_0_1_hz():
    assert "v.clamp(0.1, 100.0)" in _src("DesktopApp/BugBuster/src/tabs/wavegen.rs")
    panel = _src("Firmware/ESP32/web/src/tabs/scope/ScopePanel.tsx")
    assert "Math.max(0.1, wgFreq)" in panel
    assert "min={0.1}" in panel
