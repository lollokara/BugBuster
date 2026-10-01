"""PWR-06: usbpd_select_voltage over USB must actually renegotiate.

BBP USBPD_SELECT_PDO (cmd_husb.cpp) only writes the HUSB238 PDO_SELECT
register; the source keeps the old contract until GO 0x01. HTTP
/api/usbpd/select (api_core.cpp) does select + GO, so the two transports
disagreed and the MCP usb_pd_select tool silently did nothing over USB.
"""

from unittest.mock import patch

import pytest

from bugbuster import BugBuster
from bugbuster_mcp import session
from bugbuster_mcp.tools.power import register
from tests.mock import SimulatedDevice, SimulatedUSBTransport

_PWR06 = pytest.mark.xfail(strict=True, reason="PWR-06")


class _DummyMCP:
    def __init__(self):
        self.tools = {}

    def tool(self):
        def deco(fn):
            self.tools[fn.__name__] = fn
            return fn
        return deco


@_PWR06
def test_select_voltage_usb_commits_pdo():
    dev = SimulatedDevice()
    bb = BugBuster(SimulatedUSBTransport(dev))
    bb.usbpd_select_voltage(9)
    assert dev.usbpd_last_go == 0x01
    assert dev.usbpd_voltage == 2  # 9 V


@_PWR06
def test_mcp_usb_pd_select_commits_over_usb():
    session.configure(transport="usb", port="/dev/null", vlogic=3.3)
    mcp = _DummyMCP()
    register(mcp)
    dev = SimulatedDevice()
    bb = BugBuster(SimulatedUSBTransport(dev))
    with patch("bugbuster_mcp.session.get_client", return_value=bb):
        mcp.tools["usb_pd_select"](voltage_v=12)
    assert dev.usbpd_voltage == 3  # 12 V


def test_select_pdo_alone_only_stages():
    """Control: the raw BBP command must not renegotiate by itself."""
    dev = SimulatedDevice()
    bb = BugBuster(SimulatedUSBTransport(dev))
    from bugbuster.constants import CmdId
    bb._usb_cmd(CmdId.USBPD_SELECT_PDO, bytes([2]))
    assert dev.usbpd_voltage == 1
