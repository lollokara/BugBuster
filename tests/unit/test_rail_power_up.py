"""PWR-REFAC + PWR-11: one firmware-owned rail power-up sequence.

Before: target_power_up, enter_bootloader and release_bootloader each sent
6-10 separate PCA/IDAC frames with host-side sleeps, so a dropped link or a
crashed host mid-sequence could leave a rail on with the e-fuses in any state,
and the >12 V confirmation lived only in the MCP layer.

After: BBP RAIL_POWER_UP (0x93) / POST /api/power/rail_up does
e-fuses off -> [VADJ off + discharge] -> set V -> VADJ on -> settle -> e-fuses
armed through the blackout gate -> PG/fault readback, refuses >12 V without
confirm, and reports the applied (possibly clamped) voltage. Each MCP tool
sends exactly one frame."""

from unittest.mock import MagicMock, patch

import pytest

from bugbuster import BugBuster
from bugbuster.transport.usb import DeviceError
from bugbuster_mcp import session
from bugbuster_mcp.tools.target import register
from tests.mock import SimulatedDevice, SimulatedHTTPTransport, SimulatedUSBTransport

RAIL_POWER_UP = 0x93
RAW_POWER_CMDS = {0xB1, 0xB2, 0xA2}   # PCA_SET_CONTROL, PCA_SET_PORT, IDAC_SET_VOLTAGE


class _DummyMCP:
    def __init__(self):
        self.tools = {}

    def tool(self):
        def deco(fn):
            self.tools[fn.__name__] = fn
            return fn
        return deco


def _spy(dev):
    sent = []
    orig = dev.dispatch

    def dispatch(cmd_id, payload):
        sent.append(int(cmd_id))
        return orig(cmd_id, payload)
    dev.dispatch = dispatch
    return sent


@pytest.fixture
def tools():
    session.configure(transport="usb", port="/dev/null", vlogic=3.3)
    mcp = _DummyMCP()
    register(mcp)
    return mcp.tools


def _sim():
    dev = SimulatedDevice()
    sent = _spy(dev)
    return dev, sent, BugBuster(SimulatedUSBTransport(dev))


def test_client_rail_power_up_reports_applied_voltage():
    _dev, sent, bb = _sim()
    res = bb.rail_power_up(1, 5.0, settle_ms=0)
    assert sent.count(RAIL_POWER_UP) == 1
    assert res["rail"] == 1 and abs(res["applied_v"] - 5.0) < 0.01
    assert res["efuse_faults"] == [False, False] and res["pg"] is True


def test_over_12v_needs_confirm_in_firmware():
    _dev, _sent, bb = _sim()
    with pytest.raises(DeviceError):
        bb._usb_cmd(RAIL_POWER_UP, bytes([1]) + (13000).to_bytes(2, "little") + (0).to_bytes(2, "little") + b"\x00")
    assert bb.rail_power_up(1, 13.0, settle_ms=0, confirm=True)["applied_v"] > 12.0


def test_http_route():
    dev = SimulatedDevice()
    res = BugBuster(SimulatedHTTPTransport(dev)).rail_power_up(2, 3.3, settle_ms=0)
    assert res["rail"] == 2 and abs(res["applied_v"] - 3.3) < 0.01


def test_target_power_up_is_one_frame(tools):
    _dev, sent, bb = _sim()
    with patch("bugbuster_mcp.session.get_client", return_value=bb):
        res = tools["target_power_up"](supply_voltage=5.0, rail=2, settle_ms=0)
    assert res["success"] is True
    assert sent.count(RAIL_POWER_UP) == 1
    assert not RAW_POWER_CMDS & set(sent), sent


def test_release_bootloader_is_one_power_frame(tools):
    _dev, sent, bb = _sim()
    hal = MagicMock()
    with patch("bugbuster_mcp.session.get_client", return_value=bb), \
         patch("bugbuster_mcp.session.get_hal", return_value=hal):
        tools["release_bootloader"](boot_io=2, rail=1)
    assert sent.count(RAIL_POWER_UP) == 1
    assert not RAW_POWER_CMDS & set(sent), sent


def test_enter_bootloader_is_one_power_frame(tools):
    _dev, sent, bb = _sim()
    hal = MagicMock()
    hal._mux_state = [0, 0, 0, 0]
    with patch("bugbuster_mcp.session.get_client", return_value=bb), \
         patch("bugbuster_mcp.session.get_hal", return_value=hal):
        tools["enter_bootloader"](boot_io=1, tx_io=2, rx_io=3, settle_ms=0)
    assert sent.count(RAIL_POWER_UP) == 1
    assert not RAW_POWER_CMDS & set(sent), sent
