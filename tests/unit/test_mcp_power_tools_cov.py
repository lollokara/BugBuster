"""Coverage for bugbuster_mcp.tools.power: usb_pd_status, usb_pd_select,
power_control, wifi_status, efuse_current_get/monitor, wifi_set_ap_password."""
from __future__ import annotations

from unittest.mock import patch

import pytest

from bugbuster.client import ApPasswordResult, EfuseImonConfirmRequired, EfuseImonStatus
from bugbuster.constants import PowerControl
from bugbuster_mcp import session
from bugbuster_mcp.tools import power
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
    power.register(mcp)
    bb = make_client_mock()
    bb.power_get_status.return_value = {}
    with patch("bugbuster_mcp.session.get_client", return_value=bb):
        yield mcp.tools, bb


def test_usb_pd_status_passthrough(env):
    tools, bb = env
    st = {"present": True, "negotiated_voltage_v": 20.0, "available_pdos": [5, 9, 20]}
    bb.usbpd_get_status.return_value = st
    assert tools["usb_pd_status"]() is st


@pytest.mark.parametrize("v", [5, 9, 12, 15, 18, 20])
def test_usb_pd_select_valid(env, v):
    tools, bb = env
    res = tools["usb_pd_select"](v)
    bb.usbpd_select_voltage.assert_called_once_with(v)
    assert res["success"] is True and res["voltage_v"] == v and f"{v} V" in res["message"]


@pytest.mark.parametrize("v", [0, 3, 7, 24])
def test_usb_pd_select_invalid(env, v):
    tools, bb = env
    with patch("bugbuster_mcp.session.get_client") as gc:
        with pytest.raises(ValueError, match="not a standard USB PD voltage"):
            tools["usb_pd_select"](v)
    gc.assert_not_called()
    bb.usbpd_select_voltage.assert_not_called()


def test_power_control_risk_gate(env):
    tools, bb = env
    with pytest.raises(ValueError, match="i_understand_the_risk"):
        tools["power_control"]("vadj1", True)
    bb.power_set.assert_not_called()


@pytest.mark.parametrize("name,ctrl", [
    ("vadj1", 0), ("VADJ2", 1), ("v15a", 2), ("15v", 2), (" mux ", 3), ("usb_hub", 4),
    ("efuse1", 5), ("efuse2", 6), ("efuse3", 7), ("efuse4", 8),
])
def test_power_control_mapping(env, name, ctrl):
    tools, bb = env
    res = tools["power_control"](name, False, i_understand_the_risk=True)
    bb.power_set.assert_called_once_with(PowerControl(ctrl), False)
    assert res == {"control": name.lower().strip(), "enable": False, "success": True}


def test_power_control_unknown(env):
    tools, bb = env
    with pytest.raises(ValueError, match="Valid controls: 15v, efuse1"):
        tools["power_control"]("vadj3", True, i_understand_the_risk=True)
    bb.power_set.assert_not_called()


def test_power_control_reports_faults(env):
    tools, bb = env
    bb.power_get_status.return_value = {"efuse_faults": [False, False, True, False]}
    res = tools["power_control"]("efuse3", True, i_understand_the_risk=True)
    assert "E-fuse 3" in res["warnings"][0]


def test_wifi_status_passthrough(env):
    tools, bb = env
    st = {"mode": "AP+STA", "sta_ip": "10.0.0.7", "connected": True}
    bb.wifi_get_status.return_value = st
    assert tools["wifi_status"]() is st


def _imon(**kw):
    base = dict(result="ok", efuse=2, valid=True, saturated=False, efuse_on=True,
                imon_v=0.5, current_ma=123.0)
    base.update(kw)
    return EfuseImonStatus(**base)


def test_efuse_current_get_drops_result_field(env):
    tools, bb = env
    bb.efuse_imon_get.return_value = _imon()
    assert tools["efuse_current_get"]() == {"efuse": 2, "valid": True, "saturated": False,
                                            "efuse_on": True, "imon_v": 0.5, "current_ma": 123.0}


def test_efuse_current_monitor_paths(env):
    tools, bb = env
    with pytest.raises(ValueError, match="efuse must be"):
        tools["efuse_current_monitor"](5)
    bb.efuse_imon_set.side_effect = EfuseImonConfirmRequired("needs confirm")
    res = tools["efuse_current_monitor"](1)
    assert res["success"] is False and res["needs_power_cycle_confirmation"] is True
    bb.efuse_imon_set.side_effect = None
    bb.efuse_imon_set.return_value = _imon(efuse=1)
    res = tools["efuse_current_monitor"](1, i_understand_power_cycle=True)
    bb.efuse_imon_set.assert_called_with(1, confirm_power_cycle=True)
    assert res["success"] is True and res["efuse"] == 1 and "result" not in res


@pytest.mark.parametrize("result,persisted,needle", [
    (ApPasswordResult(True, True), True, "updated and applied"),
    (ApPasswordResult(True, False), False, "NOT saved"),
    (ApPasswordResult(False, False), False, "Failed"),
])
def test_wifi_set_ap_password_outcomes(env, result, persisted, needle):
    tools, bb = env
    bb.wifi_set_ap_password.return_value = result
    res = tools["wifi_set_ap_password"]("correcthorse", confirm=True)
    assert res["success"] is bool(result) and res["persisted"] is persisted and needle in res["message"]


@pytest.mark.parametrize("pw,confirm,match", [("longenough", False, "confirm=True"),
                                              ("short", True, "8-63"), ("x" * 64, True, "8-63")])
def test_wifi_set_ap_password_gates(env, pw, confirm, match):
    tools, bb = env
    with pytest.raises(ValueError, match=match):
        tools["wifi_set_ap_password"](pw, confirm=confirm)
    bb.wifi_set_ap_password.assert_not_called()
