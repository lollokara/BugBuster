"""Coverage for bugbuster_mcp.tools.advanced: mux_control, register_access and
the idac_control branches test_mcp_idac_control.py does not reach."""
from __future__ import annotations

from unittest.mock import patch

import pytest

from bugbuster_mcp import session
from bugbuster_mcp.tools import advanced
from tests.unit._mock_client import make_client_mock


class _DummyMCP:
    def __init__(self):
        self.tools = {}

    def tool(self):
        def deco(fn):
            self.tools[fn.__name__] = fn
            return fn
        return deco


@pytest.fixture(autouse=True)
def _session_state():
    saved = (session._transport, session._port, session._host, session._vlogic,
             session._admin_token, session._active_board)
    session.configure(transport="usb", port="/dev/null", vlogic=3.3)
    yield
    (session._transport, session._port, session._host, session._vlogic,
     session._admin_token, session._active_board) = saved


@pytest.fixture
def env():
    mcp = _DummyMCP()
    advanced.register(mcp)
    bb = make_client_mock()
    with patch("bugbuster_mcp.session.get_client", return_value=bb), \
         patch("bugbuster_mcp.session.get_active_board_profile", return_value=None):
        yield mcp.tools, bb


# ---------------------------------------------------------------------------
# mux_control
# ---------------------------------------------------------------------------

@pytest.mark.parametrize("action", ["get", "set_switch", "set_all", "bogus"])
def test_mux_risk_gate_blocks_every_action(env, action):
    tools, bb = env
    with patch("bugbuster_mcp.session.get_client") as gc:
        with pytest.raises(ValueError, match="i_understand_the_risk"):
            tools["mux_control"](action, states_csv="0,0,0,0")
    gc.assert_not_called()
    bb.mux_set_all.assert_not_called()


def test_mux_get(env):
    tools, bb = env
    bb.mux_get.return_value = [1, 0, 0x80, 0]
    res = tools["mux_control"]("GET", i_understand_the_risk=True)
    assert res["action"] == "get" and res["states"] == [1, 0, 0x80, 0]
    assert "bitmask" in res["note"]


def test_mux_set_switch(env):
    tools, bb = env
    res = tools["mux_control"]("set_switch", i_understand_the_risk=True, device=3, switch=7, closed=True)
    bb.mux_set_switch.assert_called_once_with(device=3, switch=7, closed=True)
    assert res == {"action": "set_switch", "device": 3, "switch": 7, "closed": True, "success": True}


@pytest.mark.parametrize("csv,expected", [
    ("0,0,0,0", [0, 0, 0, 0]),
    (" 1 , 2 ,3,  4 ", [1, 2, 3, 4]),
    ("0x10,0b11,0o7,255", [16, 3, 7, 255]),
    ("1,2,,3,4,", [1, 2, 3, 4]),
])
def test_mux_set_all_parses_csv(env, csv, expected):
    tools, bb = env
    res = tools["mux_control"]("set_all", i_understand_the_risk=True, states_csv=csv)
    bb.mux_set_all.assert_called_once_with(expected)
    assert res == {"action": "set_all", "states": expected, "success": True}


@pytest.mark.parametrize("csv,match", [
    ("a,b,c,d", "comma-separated integers"),
    ("1.5,0,0,0", "comma-separated integers"),
    ("", "exactly 4"),
    ("0,0,0", "exactly 4"),
    ("0,0,0,0,0", "exactly 4"),
])
def test_mux_set_all_rejects_bad_csv(env, csv, match):
    tools, bb = env
    with pytest.raises(ValueError, match=match):
        tools["mux_control"]("set_all", i_understand_the_risk=True, states_csv=csv)
    bb.mux_set_all.assert_not_called()


def test_mux_unknown_action(env):
    tools, _ = env
    with pytest.raises(ValueError, match="Unknown action 'toggle'"):
        tools["mux_control"]("toggle", i_understand_the_risk=True)


def test_mux_client_error_propagates(env):
    tools, bb = env
    bb.mux_get.side_effect = TimeoutError("no reply")
    with pytest.raises(TimeoutError):
        tools["mux_control"]("get", i_understand_the_risk=True)


# ---------------------------------------------------------------------------
# register_access
# ---------------------------------------------------------------------------

def test_register_read(env):
    tools, bb = env
    bb.register_read.return_value = 0x1234
    res = tools["register_access"]("Read", 0x46, i_understand_the_risk=True)
    bb.register_read.assert_called_once_with(0x46)
    assert res == {"action": "read", "register_address": 0x46, "value": 0x1234}


def test_register_write(env):
    tools, bb = env
    res = tools["register_access"]("write", 0x10, value=0xBEEF, i_understand_the_risk=True)
    bb.register_write.assert_called_once_with(0x10, 0xBEEF)
    assert res == {"action": "write", "register_address": 0x10, "value": 0xBEEF, "success": True}


def test_register_risk_gate(env):
    tools, bb = env
    with pytest.raises(ValueError, match="i_understand_the_risk"):
        tools["register_access"]("write", 0x10, value=1)
    bb.register_write.assert_not_called()


def test_register_unknown_action_is_wrapped_with_tool_name(env):
    tools, _ = env
    with pytest.raises(RuntimeError, match="register_access.*Unknown action"):
        tools["register_access"]("poke", 0x10, i_understand_the_risk=True)


def test_register_device_error_maps_to_actionable_message(env):
    from bugbuster.transport.usb import DeviceError
    tools, bb = env
    bb.register_read.side_effect = DeviceError(0x11, 5)
    with pytest.raises(RuntimeError, match="register_access"):
        tools["register_access"]("read", 0x10, i_understand_the_risk=True)


@pytest.mark.parametrize("transport", ["http", "auto"])
def test_register_access_refused_off_usb(env, transport):
    tools, bb = env
    session.configure(transport=transport, host="10.0.0.2")
    with pytest.raises(RuntimeError):
        tools["register_access"]("read", 0x10, i_understand_the_risk=True)
    bb.register_read.assert_not_called()


# ---------------------------------------------------------------------------
# idac_control leftovers
# ---------------------------------------------------------------------------

@pytest.mark.parametrize("action,kwargs,match", [
    ("set_voltage", dict(channel=1, voltage=5.0), "i_understand_the_risk"),
    ("set_voltage", dict(channel=1, i_understand_the_risk=True), "voltage parameter"),
    ("set_code", dict(channel=1, code=3), "i_understand_the_risk"),
    ("set_code", dict(channel=1, i_understand_the_risk=True), "code parameter"),
    ("calibrate", dict(), "Unknown action"),
])
def test_idac_gates(env, action, kwargs, match):
    tools, bb = env
    with pytest.raises(ValueError, match=match):
        tools["idac_control"](action, **kwargs)
    bb.idac_set_voltage.assert_not_called()
    bb.idac_set_code.assert_not_called()


def test_idac_set_voltage_over_12v_needs_confirm(env):
    tools, bb = env
    with pytest.raises(ValueError, match="confirm"):
        tools["idac_control"]("set_voltage", channel=2, voltage=13.0, i_understand_the_risk=True)
    res = tools["idac_control"]("set_voltage", channel=2, voltage=13.0, i_understand_the_risk=True, confirm=True)
    assert res == {"action": "set_voltage", "channel": 2, "voltage": 13.0, "success": True}
    bb.idac_set_voltage.assert_called_once_with(2, 13.0)


def test_idac_set_code_result_shape(env):
    tools, _ = env
    assert tools["idac_control"]("SET_CODE", channel=2, code=127, i_understand_the_risk=True) == \
        {"action": "set_code", "channel": 2, "code": 127, "success": True}
