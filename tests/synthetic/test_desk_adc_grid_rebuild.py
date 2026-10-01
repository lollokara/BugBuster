"""DESK-23: the ADC tab rebuilt its whole four-card grid (selects, options,
sparklines) on every ``device-state`` tick because the grid closure tracked the
full DeviceState. The grid must rebuild only when a channel's configuration
changes; live readings update in place."""

from pathlib import Path

import pytest

ADC = Path(__file__).resolve().parents[2] / "DesktopApp" / "BugBuster" / "src" / "tabs" / "adc.rs"


@pytest.mark.xfail(strict=True, reason="DESK-23")
def test_adc_grid_tracks_config_not_every_tick():
    src = ADC.read_text(encoding="utf-8")
    assert "let ds = state.get();" not in src
    assert "Memo::new" in src
