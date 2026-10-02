"""Coverage for bugbuster_mcp.tools.onboarding: WiFi join and Quick Setup slots."""
from __future__ import annotations

from types import SimpleNamespace
from unittest.mock import patch

import pytest

from bugbuster_mcp.tools import onboarding
from tests.unit._mock_client import make_client_mock


class _DummyMCP:
    def __init__(self):
        self.tools = {}

    def tool(self):
        def decorator(func):
            self.tools[func.__name__] = func
            return func
        return decorator


@pytest.fixture
def env():
    mcp = _DummyMCP()
    onboarding.register(mcp)
    bb = make_client_mock()
    with patch("bugbuster_mcp.session.get_client", return_value=bb) as gc:
        yield SimpleNamespace(t=mcp.tools, bb=bb, gc=gc)


def test_registers_every_onboarding_tool(env):
    assert set(env.t) == {"wifi_scan", "wifi_connect", "quicksetup_list",
                          "quicksetup_get", "quicksetup_save",
                          "quicksetup_apply", "quicksetup_delete"}


# ---------------------------------------------------------------------------
# wifi_scan
# ---------------------------------------------------------------------------
def test_wifi_scan_returns_networks_and_count(env):
    nets = [{"ssid": "lab", "rssi": -48, "auth": "WPA2"},
            {"ssid": "guest", "rssi": -71, "auth": "OPEN"}]
    env.bb.wifi_scan.return_value = nets
    out = env.t["wifi_scan"]()
    env.bb.wifi_scan.assert_called_once_with()
    assert out == {"networks": nets, "count": 2}


def test_wifi_scan_with_no_networks(env):
    env.bb.wifi_scan.return_value = []
    assert env.t["wifi_scan"]() == {"networks": [], "count": 0}


def test_wifi_scan_propagates_device_errors(env):
    env.bb.wifi_scan.side_effect = RuntimeError("radio busy")
    with pytest.raises(RuntimeError, match="radio busy"):
        env.t["wifi_scan"]()


# ---------------------------------------------------------------------------
# wifi_connect
# ---------------------------------------------------------------------------
def test_wifi_connect_success_returns_status(env):
    status = {"mode": "AP+STA", "ssid": "lab", "ip": "192.168.1.50",
              "rssi_dbm": -52, "connected": True}
    env.bb.wifi_connect.return_value = True
    env.bb.wifi_get_status.return_value = status
    out = env.t["wifi_connect"](ssid="lab", password="hunter2hunter2")
    env.bb.wifi_connect.assert_called_once_with("lab", "hunter2hunter2")
    env.bb.wifi_get_status.assert_called_once_with()
    assert out == {"success": True, "ssid": "lab", "status": status}


def test_wifi_connect_open_network_passes_empty_password(env):
    env.bb.wifi_connect.return_value = True
    env.bb.wifi_get_status.return_value = {"connected": True}
    env.t["wifi_connect"](ssid="cafe", password="")
    env.bb.wifi_connect.assert_called_once_with("cafe", "")


def test_wifi_connect_failure_still_reports_status(env):
    env.bb.wifi_connect.return_value = False
    env.bb.wifi_get_status.return_value = {"connected": False}
    out = env.t["wifi_connect"](ssid="lab", password="wrong-password")
    assert out["success"] is False
    assert out["status"] == {"connected": False}


@pytest.mark.parametrize("raw, expected", [(1, True), (0, False), (b"\x01", True)])
def test_wifi_connect_coerces_result_to_bool(env, raw, expected):
    env.bb.wifi_connect.return_value = raw
    env.bb.wifi_get_status.return_value = {}
    assert env.t["wifi_connect"](ssid="x", password="y")["success"] is expected


def test_wifi_connect_survives_status_read_failure(env):
    env.bb.wifi_connect.return_value = True
    env.bb.wifi_get_status.side_effect = RuntimeError("timeout")
    out = env.t["wifi_connect"](ssid="lab", password="pw")
    assert out["success"] is True
    assert out["status"] == {"error": "timeout"}


def test_wifi_connect_connect_errors_propagate(env):
    env.bb.wifi_connect.side_effect = RuntimeError("link down")
    with pytest.raises(RuntimeError, match="link down"):
        env.t["wifi_connect"](ssid="lab", password="pw")
    env.bb.wifi_get_status.assert_not_called()


# ---------------------------------------------------------------------------
# quicksetup_list / quicksetup_get
# ---------------------------------------------------------------------------
def test_quicksetup_list_returns_slots_and_count(env):
    slots = [{"index": i, "occupied": i == 1, "hash": i * 3} for i in range(4)]
    env.bb.quicksetup_list.return_value = slots
    out = env.t["quicksetup_list"]()
    env.bb.quicksetup_list.assert_called_once_with()
    assert out == {"slots": slots, "count": 4}


def test_quicksetup_list_empty(env):
    env.bb.quicksetup_list.return_value = []
    assert env.t["quicksetup_list"]() == {"slots": [], "count": 0}


def test_quicksetup_get_returns_slot_and_config(env):
    cfg = {"channels": [{"fn": 1}], "vadj1": 5.0}
    env.bb.quicksetup_get.return_value = cfg
    out = env.t["quicksetup_get"](slot=2)
    env.bb.quicksetup_get.assert_called_once_with(2)
    assert out == {"slot": 2, "config": cfg}


def test_quicksetup_get_free_slot_is_none(env):
    env.bb.quicksetup_get.return_value = None
    assert env.t["quicksetup_get"](slot=0) == {"slot": 0, "config": None}


# ---------------------------------------------------------------------------
# Destructive tools: save / apply / delete
# ---------------------------------------------------------------------------
_DESTRUCTIVE = [
    ("quicksetup_save", "quicksetup_save", {"slot": 1, "hash": 7}, "overwrites"),
    ("quicksetup_apply", "quicksetup_apply", {"ok": True, "applied": True}, "reconfigures"),
    ("quicksetup_delete", "quicksetup_delete", {"ok": True, "deleted": True}, "erases"),
]
_IDS = [d[0] for d in _DESTRUCTIVE]


@pytest.mark.parametrize("tool, method, result, _word", _DESTRUCTIVE, ids=_IDS)
def test_destructive_tool_forwards_slot_and_returns_device_result(env, tool, method, result, _word):
    getattr(env.bb, method).return_value = result
    out = env.t[tool](slot=3, i_understand_the_risk=True)
    getattr(env.bb, method).assert_called_once_with(3)
    assert out == result


@pytest.mark.parametrize("tool, method, result, word", _DESTRUCTIVE, ids=_IDS)
def test_destructive_tool_refuses_without_acknowledgement(env, tool, method, result, word):
    with pytest.raises(ValueError, match=tool) as exc:
        env.t[tool](slot=1)
    assert word in str(exc.value)
    assert "i_understand_the_risk=True" in str(exc.value)
    env.gc.assert_not_called()
    getattr(env.bb, method).assert_not_called()


@pytest.mark.parametrize("tool, method, result, _word", _DESTRUCTIVE, ids=_IDS)
def test_destructive_tool_refuses_explicit_false(env, tool, method, result, _word):
    with pytest.raises(ValueError):
        env.t[tool](slot=1, i_understand_the_risk=False)
    getattr(env.bb, method).assert_not_called()


@pytest.mark.parametrize("tool, method, result, _word", _DESTRUCTIVE, ids=_IDS)
def test_destructive_tool_propagates_firmware_errors(env, tool, method, result, _word):
    getattr(env.bb, method).side_effect = RuntimeError("firmware error")
    with pytest.raises(RuntimeError, match="firmware error"):
        env.t[tool](slot=0, i_understand_the_risk=True)


@pytest.mark.parametrize("tool, method, result, _word", _DESTRUCTIVE, ids=_IDS)
@pytest.mark.parametrize("slot", [4, 256])
def test_destructive_tool_rejects_out_of_range_slot(env, tool, method, result, _word, slot):
    getattr(env.bb, method).return_value = result
    with pytest.raises(ValueError):
        env.t[tool](slot=slot, i_understand_the_risk=True)
    getattr(env.bb, method).assert_not_called()
