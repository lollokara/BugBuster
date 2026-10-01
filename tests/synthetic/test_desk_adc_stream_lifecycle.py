"""DESK-28: the desktop host emitted a 30 Hz ``adc-stream`` event that no
frontend code listened to (and kept re-emitting the last payload from the
idle-timeout branch after the stream stopped), and the ADC tab started the
firmware ADC stream but never stopped it when the tab closed."""

from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[2]
DESK = ROOT / "DesktopApp" / "BugBuster"


@pytest.mark.xfail(strict=True, reason="DESK-28")
def test_no_unlistened_adc_stream_event():
    host = (DESK / "src-tauri" / "src" / "connection_manager.rs").read_text(encoding="utf-8")
    assert '"adc-stream"' not in host


@pytest.mark.xfail(strict=True, reason="DESK-28")
def test_adc_tab_stops_stream_on_close():
    tab = (DESK / "src" / "tabs" / "adc.rs").read_text(encoding="utf-8")
    cleanup = tab.split("on_cleanup", 1)
    assert len(cleanup) == 2, "ADC tab has no on_cleanup"
    assert "stop_adc_stream" in cleanup[1].split("});", 1)[0]
