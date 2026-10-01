"""E-fuse IMON monitor: wire contract, USB/HTTP client paths, confirm gate, MCP tools."""

import re
import struct
from unittest.mock import MagicMock, patch

import pytest

import bugbuster as bb
from bugbuster.client import EfuseImonConfirmRequired, EfuseImonStatus
from bugbuster.constants import CmdId, PowerControl
from bugbuster_mcp.tools.power import register
from tests.lib.srcread import read_source
from tests.mock import SimulatedDevice, SimulatedHTTPTransport, SimulatedUSBTransport

BBP_H = read_source("Firmware/ESP32/src/bbp/bbp.h")


def _usb_client():
    device = SimulatedDevice()
    client = bb.BugBuster(SimulatedUSBTransport(device, hat=True))
    client.connect()
    return client, device


def _http_client():
    device = SimulatedDevice()
    client = bb.BugBuster(SimulatedHTTPTransport(device, hat=True))
    client.connect()
    return client, device


def _block(efuse=1, flags=0x01, v=0.5, ma=12.5) -> bytes:
    return struct.pack("<BBff", efuse, flags, v, ma)


def _stub_usb_client(response: bytes):
    """Client whose _usb_cmd records calls and returns *response*."""
    client = bb.BugBuster.__new__(bb.BugBuster)
    client._usb = True
    client._usb_cmd = MagicMock(return_value=response)
    return client


# ---------------------------------------------------------------------------
# Firmware <-> host contract
# ---------------------------------------------------------------------------

@pytest.mark.parametrize("name,member", [
    ("BBP_CMD_EFUSE_IMON_SET", CmdId.EFUSE_IMON_SET),
    ("BBP_CMD_EFUSE_IMON_GET", CmdId.EFUSE_IMON_GET),
])
def test_cmdid_matches_bbp_h(name, member):
    m = re.search(rf"#define\s+{name}\s+0x([0-9A-Fa-f]+)", BBP_H)
    assert m, f"{name} missing from bbp.h"
    assert int(m.group(1), 16) == int(member)


# ---------------------------------------------------------------------------
# USB encode / decode
# ---------------------------------------------------------------------------

def test_usb_get_decodes_status_block():
    client = _stub_usb_client(_block(efuse=2, flags=0x07, v=1.25, ma=30.0))
    st = client.efuse_imon_get()
    client._usb_cmd.assert_called_once_with(CmdId.EFUSE_IMON_GET)
    assert st == EfuseImonStatus("ok", 2, True, True, True, pytest.approx(1.25), pytest.approx(30.0))


def test_usb_get_flags_are_independent():
    st = _stub_usb_client(_block(flags=0x04)).efuse_imon_get()
    assert (st.valid, st.saturated, st.efuse_on) == (False, False, True)


def test_usb_set_encodes_efuse_and_confirm_flag():
    client = _stub_usb_client(bytes([0]) + _block(efuse=3))
    st = client.efuse_imon_set(3, confirm_power_cycle=True)
    client._usb_cmd.assert_called_once_with(CmdId.EFUSE_IMON_SET, b"\x03\x01")
    assert st.result == "ok" and st.efuse == 3


def test_usb_set_without_confirm_sends_zero_flags():
    client = _stub_usb_client(bytes([0]) + _block(efuse=0, flags=0))
    client.efuse_imon_set(0)
    client._usb_cmd.assert_called_once_with(CmdId.EFUSE_IMON_SET, b"\x00\x00")


def test_usb_set_needs_confirm_raises_dedicated_exception():
    client = _stub_usb_client(bytes([1]) + _block(efuse=0, flags=0))
    with pytest.raises(EfuseImonConfirmRequired, match="confirm_power_cycle=True"):
        client.efuse_imon_set(2)


@pytest.mark.parametrize("code,name", [
    (2, "busy"), (3, "invalid"), (4, "slot_held"), (5, "hw_fail"), (6, "unsupported"),
])
def test_usb_set_failure_codes_raise_runtime_error(code, name):
    client = _stub_usb_client(bytes([code]) + _block(efuse=0, flags=0))
    with pytest.raises(RuntimeError, match=name) as exc:
        client.efuse_imon_set(1)
    assert not isinstance(exc.value, EfuseImonConfirmRequired)


def test_usb_short_response_is_a_protocol_error():
    from bugbuster.protocol import ProtocolError
    with pytest.raises(ProtocolError):
        _stub_usb_client(b"\x01\x01").efuse_imon_get()


@pytest.mark.parametrize("bad", [-1, 5, 255])
def test_efuse_out_of_range_is_rejected_before_io(bad):
    client = _stub_usb_client(b"")
    with pytest.raises(ValueError):
        client.efuse_imon_set(bad)
    client._usb_cmd.assert_not_called()


# ---------------------------------------------------------------------------
# HTTP
# ---------------------------------------------------------------------------

def _stub_http_client(reply: dict):
    client = bb.BugBuster.__new__(bb.BugBuster)
    client._usb = False
    client._http_get = MagicMock(return_value=reply)
    client._http_post = MagicMock(return_value=reply)
    return client


_HTTP_OK = {"ok": True, "result": "ok", "efuse": 4, "valid": True, "saturated": False,
            "efuseOn": True, "imonV": 0.25, "currentMa": 5.0}


def test_http_get_maps_camel_case_fields():
    client = _stub_http_client(_HTTP_OK)
    st = client.efuse_imon_get()
    client._http_get.assert_called_once_with("/selftest/efuse_imon")
    assert st == EfuseImonStatus("ok", 4, True, False, True, 0.25, 5.0)


def test_http_set_posts_efuse_and_confirm():
    client = _stub_http_client(_HTTP_OK)
    client.efuse_imon_set(4, confirm_power_cycle=True)
    client._http_post.assert_called_once_with("/selftest/efuse_imon",
                                              {"efuse": 4, "confirm": True})


def test_http_needs_confirm_raises():
    client = _stub_http_client({**_HTTP_OK, "ok": False, "result": "needs_confirm"})
    with pytest.raises(EfuseImonConfirmRequired):
        client.efuse_imon_set(4)


def test_http_busy_raises_runtime_error():
    client = _stub_http_client({**_HTTP_OK, "ok": False, "result": "busy"})
    with pytest.raises(RuntimeError, match="busy"):
        client.efuse_imon_set(4)


def test_http_error_body_surfaces_message():
    client = _stub_http_client({"ok": False, "error": "efuse (0-4) required"})
    with pytest.raises(RuntimeError, match="efuse"):
        client.efuse_imon_set(1)


# ---------------------------------------------------------------------------
# Simulator end-to-end (both transports)
# ---------------------------------------------------------------------------

@pytest.mark.parametrize("make_client", [_usb_client, _http_client])
def test_sim_attach_when_efuse_off_needs_no_confirm(make_client):
    client, _ = make_client()
    st = client.efuse_imon_set(2)
    assert (st.efuse, st.valid, st.efuse_on) == (2, True, False)
    assert client.efuse_imon_get().efuse == 2
    assert client.efuse_imon_set(0).efuse == 0
    client.disconnect()


@pytest.mark.parametrize("make_client", [_usb_client, _http_client])
def test_sim_attach_to_enabled_efuse_requires_confirm(make_client):
    client, _ = make_client()
    client.power_set(PowerControl.EFUSE3, True)
    with pytest.raises(EfuseImonConfirmRequired):
        client.efuse_imon_set(3)
    assert client.efuse_imon_get().efuse == 0
    st = client.efuse_imon_set(3, confirm_power_cycle=True)
    assert (st.efuse, st.efuse_on) == (3, True)
    client.disconnect()


@pytest.mark.parametrize("make_client", [_usb_client, _http_client])
def test_sim_detach_from_enabled_efuse_requires_confirm(make_client):
    client, _ = make_client()
    client.efuse_imon_set(1)
    client.power_set(PowerControl.EFUSE1, True)
    with pytest.raises(EfuseImonConfirmRequired):
        client.efuse_imon_set(0)
    assert client.efuse_imon_get().efuse == 1
    client.efuse_imon_set(0, confirm_power_cycle=True)
    assert client.efuse_imon_get().efuse == 0
    client.disconnect()


def test_sim_http_rejects_bad_body():
    from tests.mock import http_routes
    device = SimulatedDevice()
    reply = http_routes.dispatch(device, "POST", "/api/selftest/efuse_imon",
                                 {}, {"efuse": 9},
                                 {"X-BugBuster-Admin-Token": device.admin_token})
    assert reply["ok"] is False and "error" in reply


# ---------------------------------------------------------------------------
# MCP tools
# ---------------------------------------------------------------------------

class _DummyMCP:
    def __init__(self):
        self.tools = {}

    def tool(self):
        def decorator(func):
            self.tools[func.__name__] = func
            return func
        return decorator


@pytest.fixture
def tools():
    mcp = _DummyMCP()
    register(mcp)
    return mcp.tools


def test_mcp_monitor_without_flag_returns_explanation_not_exception(tools):
    fake = MagicMock()
    fake.efuse_imon_set.side_effect = EfuseImonConfirmRequired("x")
    with patch("bugbuster_mcp.session.get_client", return_value=fake):
        res = tools["efuse_current_monitor"](2)
    fake.efuse_imon_set.assert_called_once_with(2, confirm_power_cycle=False)
    assert res["success"] is False
    assert res["needs_power_cycle_confirmation"] is True
    assert "i_understand_power_cycle=True" in res["message"]


def test_mcp_monitor_with_flag_forwards_confirm_and_returns_status(tools):
    fake = MagicMock()
    fake.efuse_imon_set.return_value = EfuseImonStatus("ok", 2, True, False, True, 0.5, 10.0)
    with patch("bugbuster_mcp.session.get_client", return_value=fake):
        res = tools["efuse_current_monitor"](2, i_understand_power_cycle=True)
    fake.efuse_imon_set.assert_called_once_with(2, confirm_power_cycle=True)
    assert res == {"success": True, "efuse": 2, "valid": True, "saturated": False,
                   "efuse_on": True, "imon_v": 0.5, "current_ma": 10.0}


@pytest.mark.parametrize("bad", [-1, 5])
def test_mcp_monitor_rejects_bad_efuse(tools, bad):
    with patch("bugbuster_mcp.session.get_client") as get_client:
        with pytest.raises(ValueError):
            tools["efuse_current_monitor"](bad)
    get_client.assert_not_called()


def test_mcp_get_returns_status_dict(tools):
    fake = MagicMock()
    fake.efuse_imon_get.return_value = EfuseImonStatus("ok", 0, False, False, False, 0.0, 0.0)
    with patch("bugbuster_mcp.session.get_client", return_value=fake):
        res = tools["efuse_current_get"]()
    assert res["efuse"] == 0 and res["valid"] is False and "result" not in res


def test_mcp_tool_docstrings_state_the_rules(tools):
    doc = tools["efuse_current_monitor"].__doc__
    assert "ONE e-fuse" in doc and "power-cycled" in doc and "refuses" in doc
