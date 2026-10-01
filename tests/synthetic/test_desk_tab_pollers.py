"""DESK-22..26 (tab pollers): the HAT, Voltages, IO Expander and USB-PD tabs
refetched over the device link on every ``device-state`` tick (300 ms on
HTTP), on top of their own interval polls, and the HAT tab kept probing a
bare board every 3 s forever. Each tab must poll on its own bounded,
non-overlapping schedule, and the HAT tab must back off when no HAT is fitted."""

import re
from pathlib import Path

import pytest

TABS = Path(__file__).resolve().parents[2] / "DesktopApp" / "BugBuster" / "src" / "tabs"


@pytest.mark.xfail(strict=True, reason="DESK-22")
@pytest.mark.parametrize("tab", ["hat.rs", "voltages.rs", "ioexp.rs", "usbpd.rs"])
def test_tab_does_not_refetch_on_every_state_tick(tab):
    src = (TABS / tab).read_text(encoding="utf-8")
    assert "let _ = state.get()" not in src


@pytest.mark.xfail(strict=True, reason="DESK-22")
def test_hat_probe_backs_off_on_bare_board():
    src = (TABS / "hat.rs").read_text(encoding="utf-8")
    assert re.search(r"HAT_ABSENT_POLL_MS", src)
    assert re.search(r"detected[^;]*HAT_ABSENT_POLL_MS|HAT_ABSENT_POLL_MS[^;]*detected", src, re.S)
