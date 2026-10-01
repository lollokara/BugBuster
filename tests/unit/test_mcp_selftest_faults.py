"""MCP-21 (with IO-6, IO-7, IO-18): selftest and check_faults must be able to fail.

They read keys the client never returns: selftest used boot_ok (client:
boot.passed) and a "3v3" supply (client: supplies_ok); check_faults used
channel_alerts (client: channels[].alert). An e-fuse that tripped and was
auto-disabled (pca9535.cpp clears FLT when it switches the fuse off) only
survives in the fault log, which check_faults never inspected.
"""

from unittest.mock import patch

import pytest

from bugbuster import BugBuster
from bugbuster_mcp import session
from bugbuster_mcp.tools.discovery import register
from tests.mock import SimulatedDevice, SimulatedUSBTransport


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


def _run(tools, name, dev):
    bb = BugBuster(SimulatedUSBTransport(dev))
    with patch("bugbuster_mcp.session.get_client", return_value=bb):
        return tools[name]()


def test_selftest_reports_failed_boot_test(tools):
    dev = SimulatedDevice()
    dev.selftest_boot_passed = False
    res = _run(tools, "selftest", dev)
    assert res["all_pass"] is False


def test_selftest_passes_on_healthy_device(tools):
    """Control: a healthy simulator must still pass."""
    res = _run(tools, "selftest", SimulatedDevice())
    assert res["all_pass"] is True, res["warnings"]


def test_check_faults_reports_channel_alert(tools):
    dev = SimulatedDevice()
    dev.channels[2]["channel_alert"] = 0x0010
    res = _run(tools, "check_faults", dev)
    assert res["has_faults"] is True
    assert any("Channel 2" in f for f in res["faults"])


def test_check_faults_reports_auto_disabled_efuse(tools):
    dev = SimulatedDevice()
    # EFUSE3 tripped (0 = efuse_trip, channel 2 = EFUSE3), firmware auto-disabled
    # it so FLT and the enable are both low now.
    dev.pca_fault_log = [(0, 2, 1000), (1, 2, 1001)]
    res = _run(tools, "check_faults", dev)
    assert res["has_faults"] is True
    assert any("3" in f and "auto" in f.lower() for f in res["faults"])


def test_check_faults_clean_device(tools):
    res = _run(tools, "check_faults", SimulatedDevice())
    assert res["has_faults"] is False
