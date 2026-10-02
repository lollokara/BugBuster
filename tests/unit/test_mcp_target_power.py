"""MCP-22: target power-up tools must switch the right e-fuses and report trips.

Per bus_planner.cpp's route table, VADJ1 feeds EFUSE1+EFUSE2 (IO 1-6) and
VADJ2 feeds EFUSE3+EFUSE4 (IO 7-12). The tools mapped rail 2 to EFUSE2, left
EFUSE2 off for rail 1, and read get_status()["power"], a key get_status() never
returns, so "success" was always true.
"""

from unittest.mock import patch

import pytest

from bugbuster import BugBuster
from bugbuster.constants import PowerControl
from bugbuster_mcp import session
from bugbuster_mcp.tools.target import register
from tests.mock import SimulatedDevice, SimulatedHTTPTransport, SimulatedUSBTransport
from tests.unit._mock_client import make_client_mock

class _DummyMCP:
    def __init__(self):
        self.tools = {}

    def tool(self):
        def deco(fn):
            self.tools[fn.__name__] = fn
            return fn
        return deco


@pytest.fixture
def tools():
    session.configure(transport="usb", port="/dev/null", vlogic=3.3)
    mcp = _DummyMCP()
    register(mcp)
    return mcp.tools


def _enabled_efuses(bb):
    return {c.args[0] for c in bb.power_set.call_args_list
            if c.args[0] in (PowerControl.EFUSE1, PowerControl.EFUSE2,
                             PowerControl.EFUSE3, PowerControl.EFUSE4)
            and c.kwargs.get("on", c.args[1] if len(c.args) > 1 else None)}


@pytest.mark.parametrize("rail,expected", [
    (1, {PowerControl.EFUSE1, PowerControl.EFUSE2}),
    (2, {PowerControl.EFUSE3, PowerControl.EFUSE4}),
])
def test_power_up_enables_both_efuses_of_the_rail(tools, rail, expected):
    # PWR-REFAC: the e-fuse sequence now runs in firmware (RAIL_POWER_UP);
    # the tool must target the right rail with both of its e-fuses (mask 0).
    bb = make_client_mock()
    bb.rail_power_up.return_value = {"rail": rail, "applied_v": 5.0, "clamped": False,
                                     "pg": True, "efuse_faults": [False, False]}
    with patch("bugbuster_mcp.session.get_client", return_value=bb):
        res = tools["target_power_up"](supply_voltage=5.0, rail=rail, settle_ms=0)
    bb.rail_power_up.assert_called_once()
    args, kwargs = bb.rail_power_up.call_args
    assert args[0] == rail and kwargs.get("efuse_mask", 0) == 0
    assert set(res["efuses"]) == {e.name for e in expected}
    bb.power_set.assert_not_called()


def test_tripped_efuse_reports_failure(tools):
    dev = SimulatedDevice()
    dev.efuse_faults = [False, False, True, False]   # EFUSE3 = rail 2
    bb = BugBuster(SimulatedUSBTransport(dev))
    with patch("bugbuster_mcp.session.get_client", return_value=bb):
        res = tools["target_power_up"](supply_voltage=5.0, rail=2, settle_ms=0)
    assert res["success"] is False
    assert any("EFUSE3" in w.upper() or "eFuse3" in w for w in res["warnings"])


def test_over_12v_needs_confirm(tools):
    bb = make_client_mock()
    with patch("bugbuster_mcp.session.get_client", return_value=bb):
        with pytest.raises(ValueError):
            tools["target_power_up"](supply_voltage=14.0, rail=1, settle_ms=0)
    bb.power_set.assert_not_called()


def test_http_power_status_has_documented_keys():
    dev = SimulatedDevice()
    dev.efuse_faults = [False, True, False, False]
    st = BugBuster(SimulatedHTTPTransport(dev)).power_get_status()
    assert st["efuse_faults"] == [False, True, False, False]
    assert st["vadj1_pg"] is True and st["logic_pg"] is True
