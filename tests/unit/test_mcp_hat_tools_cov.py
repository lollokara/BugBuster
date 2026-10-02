"""Coverage for bugbuster_mcp.tools.hat: every HAT tool plus the helper
branches test_mcp_hat_helpers.py does not reach."""
from __future__ import annotations

from unittest.mock import patch

import pytest

from bugbuster_mcp import session
from bugbuster_mcp.tools import hat
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


_RAILS_OK = {"count": 3, "rails": [{"rail_id": i, "enabled": False, "status": 0} for i in range(3)]}


@pytest.fixture
def env():
    mcp = _DummyMCP()
    hat.register(mcp)
    bb = make_client_mock()
    bb.hat_status_cached.return_value = {"detected": True, "hat_type": 0x02}
    bb.hat_get_status.return_value = {"detected": True, "connected": True, "fw_version": "2.1"}
    bb.hat_get_caps.return_value = {"rail_count": 3, "hw_revision": 2}
    bb.hat_get_rail_status.return_value = _RAILS_OK
    bb.hat_calibrate_status.return_value = {"state": 0}
    bb.hat_la_get_status.return_value = {"state": 0, "state_name": "idle"}
    with patch("bugbuster_mcp.session.get_client", return_value=bb):
        yield mcp.tools, bb


_NO_HAT_CALLS = [
    ("hat_get_rail_status", {}),
    ("hat_health_summary", {}),
    ("hat_preflight_rail_operation", {"rail_id": 1}),
    ("hat_set_rail_enable", {"rail_id": 1, "enable": True}),
    ("hat_set_rail_voltage", {"rail_id": 1, "voltage_mv": 5000}),
    ("hat_set_led_state", {"led_id": 1, "color_code": 2}),
    ("hat_la_set_route", {"route_id": 0}),
    ("hat_calibrate_start", {"rail_id": 1, "confirm": True}),
    ("hat_calibrate_status", {}),
    ("hat_calibrate_import", {"rail_id": 1, "points": [], "confirm": True}),
    ("hat_set_io_bank", {"dirs": 1, "ups": 0, "dns": 0}),
    ("hat_set_level_shift", {"oe": True, "dir": False}),
]


@pytest.mark.parametrize("name,kwargs", _NO_HAT_CALLS)
def test_every_tool_requires_a_hat(env, name, kwargs):
    tools, bb = env
    bb.hat_status_cached.return_value = {"detected": False}
    with pytest.raises(RuntimeError, match="No HAT"):
        tools[name](**kwargs)
    for m in ("hat_set_rail_enable", "hat_set_rail_voltage", "hat_set_led_state", "hat_la_set_route",
              "hat_calibrate_start", "hat_calibrate_import", "hat_set_io_bank", "hat_set_level_shift"):
        getattr(bb, m).assert_not_called()


# ---------------------------------------------------------------------------
# hat_get_caps (type-gated)
# ---------------------------------------------------------------------------

def test_get_caps_on_la_hat(env):
    tools, bb = env
    assert tools["hat_get_caps"]() == {"rail_count": 3, "hw_revision": 2}


def test_get_caps_refuses_daq_hat(env):
    tools, bb = env
    bb.hat_status_cached.return_value = {"detected": True, "hat_type": 0x10}
    with pytest.raises(RuntimeError, match="DAQ HAT"):
        tools["hat_get_caps"]()
    bb.hat_get_caps.assert_not_called()


def test_get_caps_without_hat(env):
    tools, bb = env
    bb.hat_status_cached.return_value = {"detected": False}
    with pytest.raises(RuntimeError):
        tools["hat_get_caps"]()
    bb.hat_get_caps.assert_not_called()


def test_get_caps_error_gets_tool_context(env):
    tools, bb = env
    bb.hat_get_caps.side_effect = OSError("uart")
    with pytest.raises(RuntimeError, match="hat_get_caps"):
        tools["hat_get_caps"]()


def test_get_rail_status_passthrough(env):
    tools, _ = env
    assert tools["hat_get_rail_status"]() is _RAILS_OK


# ---------------------------------------------------------------------------
# hat_health_summary
# ---------------------------------------------------------------------------

def test_health_summary_healthy(env):
    tools, _ = env
    res = tools["hat_health_summary"]()
    assert res["healthy"] is True and res["degraded"] is False
    assert res["fw_version"] == "2.1"
    assert [r["name"] for r in res["rails"]] == ["3V3_ADJ", "VADJ3", "VADJ4"]
    assert res["capabilities"]["rail_count"] == 3
    assert res["logic_analyzer"]["state_name"] == "idle"


def test_health_summary_tolerates_every_optional_read_failing(env):
    tools, bb = env
    for m in ("hat_get_caps", "hat_get_rail_status", "hat_calibrate_status", "hat_la_get_status"):
        getattr(bb, m).side_effect = RuntimeError(m)
    res = tools["hat_health_summary"]()
    assert res["present"] is True and res["rails"] == []
    assert res["calibration"]["state"] == 0
    assert res["capabilities"]["rail_count"] is None


def test_health_summary_status_read_failure_propagates(env):
    tools, bb = env
    bb.hat_get_status.side_effect = TimeoutError("hat")
    with pytest.raises(TimeoutError):
        tools["hat_health_summary"]()


def test_build_health_summary_camelcase_and_bad_rails():
    res = hat.build_hat_health_summary(
        status={"present": True, "connected": True, "fwMajor": 1, "fwMinor": 4, "laRoute": 1},
        caps={"railCount": 2, "laRouteCount": 2},
        rail_status={"rails": [{"railId": 7, "voltageMv": 3300, "currentMa": 12, "status": 9},
                               {"railId": "x"}, {}]},
        cal_status={"state": 3, "railId": 1, "persistState": 2, "validationFlags": 4},
        la_status={"stateName": "DONE", "usbMounted": True, "stopReason": 1},
    )
    assert res["fw_version"] == "1.4"
    assert res["rails"] == [{"rail_id": 7, "name": "rail_7", "enabled": False, "voltage_mv": 3300,
                             "current_ma": 12, "status": 9, "status_name": "unknown_9"}]
    assert res["degraded"] is True and res["healthy"] is False
    assert res["calibration"]["failed"] is True and res["calibration"]["rail_id"] == 1
    assert res["logic_analyzer"] == {"state": None, "state_name": "DONE", "route": 1,
                                     "usb_mounted": True, "stop_reason": 1}


def test_build_health_summary_empty():
    res = hat.build_hat_health_summary()
    assert res["present"] is False and res["healthy"] is False and res["fw_version"] is None


# ---------------------------------------------------------------------------
# preflight
# ---------------------------------------------------------------------------

def test_preflight_tool_ok(env):
    tools, _ = env
    res = tools["hat_preflight_rail_operation"](2, target_voltage_mv=12000)
    assert res == {"ok": True, "rail_id": 2, "rail_name": "VADJ4", "target_voltage_mv": 12000,
                   "reasons": [], "warnings": []}


def test_preflight_tool_ignores_read_failures(env):
    tools, bb = env
    for m in ("hat_get_caps", "hat_get_rail_status", "hat_calibrate_status"):
        getattr(bb, m).side_effect = RuntimeError(m)
    assert tools["hat_preflight_rail_operation"](1)["ok"] is True


@pytest.mark.parametrize("rail,mv,caps,needle", [
    (5, None, None, "rail_id must be"),
    (0, 1600, None, "3V3_ADJ target"),
    (0, 5100, None, "3V3_ADJ target"),
    (2, 1700, None, "VADJ3/VADJ4 target"),
    (1, 36001, None, "VADJ3/VADJ4 target"),
    (2, None, {"railCount": 2}, "only 2 rail"),
])
def test_preflight_blocking_reasons(rail, mv, caps, needle):
    res = hat.build_hat_rail_preflight(rail, target_voltage_mv=mv, caps=caps)
    assert res["ok"] is False and any(needle in r for r in res["reasons"])


def test_preflight_calibration_states():
    running_any = hat.build_hat_rail_preflight(1, cal_status={"state": 1})
    assert running_any["ok"] is False
    failed = hat.build_hat_rail_preflight(1, cal_status={"state": 3})
    assert failed["ok"] is True and "last calibration failed" in failed["warnings"][0]
    faulted = hat.build_hat_rail_preflight(1, rail_status={"rails": [{"rail_id": 1, "status": 3}]})
    assert faulted["reasons"] == ["VADJ3 status is busy"]


# ---------------------------------------------------------------------------
# hat_set_rail_enable
# ---------------------------------------------------------------------------

def test_rail_enable_happy(env):
    tools, bb = env
    bb.hat_set_rail_enable.return_value = {"count": 3}
    assert tools["hat_set_rail_enable"](0, True) == {"count": 3}
    bb.hat_set_rail_enable.assert_called_once_with(0, True)


def test_rail_enable_invalid_rail(env):
    tools, bb = env
    with pytest.raises(ValueError, match="Invalid rail_id 3"):
        tools["hat_set_rail_enable"](3, True)
    bb.hat_get_rail_status.assert_not_called()


def test_rail_enable_blocked_by_preflight(env):
    tools, bb = env
    bb.hat_get_rail_status.return_value = {"rails": [{"rail_id": 1, "status": 1}]}
    with pytest.raises(RuntimeError, match="VADJ3 status is fault"):
        tools["hat_set_rail_enable"](1, True)
    bb.hat_set_rail_enable.assert_not_called()


def test_rail_enable_warning_needs_confirm(env):
    tools, bb = env
    bb.hat_calibrate_status.return_value = {"state": 1, "rail_id": 2}
    bb.hat_set_rail_enable.return_value = {"count": 3}
    with pytest.raises(ValueError, match="confirm=True"):
        tools["hat_set_rail_enable"](1, True)
    bb.hat_set_rail_enable.assert_not_called()
    res = tools["hat_set_rail_enable"](1, True, confirm=True)
    assert res["warnings"] == ["calibration is currently running on rail 2"]


# ---------------------------------------------------------------------------
# hat_set_rail_voltage
# ---------------------------------------------------------------------------

def test_rail_voltage_happy(env):
    tools, bb = env
    bb.hat_set_rail_voltage.return_value = {"rails": []}
    res = tools["hat_set_rail_voltage"](1, 12000)
    bb.hat_set_rail_voltage.assert_called_once_with(1, 12000)
    assert res == {"success": True, "rail_id": 1, "voltage_mv": 12000, "rail_status": {"rails": []}}


@pytest.mark.parametrize("rail,mv,match", [(0, 3300, "Invalid rail_id 0"), (3, 3300, "Invalid rail_id 3"),
                                           (1, 0, "positive"), (2, -5, "positive"),
                                           (1, 15001, "confirm=True")])
def test_rail_voltage_validation(env, rail, mv, match):
    tools, bb = env
    with pytest.raises(ValueError, match=match):
        tools["hat_set_rail_voltage"](rail, mv)
    bb.hat_get_rail_status.assert_not_called()
    bb.hat_set_rail_voltage.assert_not_called()


def test_rail_voltage_above_15v_with_confirm(env):
    tools, bb = env
    tools["hat_set_rail_voltage"](2, 27000, confirm=True)
    bb.hat_set_rail_voltage.assert_called_once_with(2, 27000)


def test_rail_voltage_preflight_blocks(env):
    tools, bb = env
    with pytest.raises(RuntimeError, match="VADJ3/VADJ4 target"):
        tools["hat_set_rail_voltage"](1, 1500)
    bb.hat_set_rail_voltage.assert_not_called()


def test_rail_voltage_surfaces_preflight_warnings(env):
    tools, bb = env
    bb.hat_calibrate_status.return_value = {"state": 3}
    bb.hat_set_rail_voltage.return_value = {}
    res = tools["hat_set_rail_voltage"](1, 5000)
    assert "last calibration failed" in " ".join(res.get("warnings", []))


# ---------------------------------------------------------------------------
# LEDs, LA route, IO bank, level shifter, calibration
# ---------------------------------------------------------------------------

@pytest.mark.parametrize("ok,needle", [(True, "LED 8 set to color code 99."), (False, "Failed to set LED 8.")])
def test_led_state(env, ok, needle):
    tools, bb = env
    bb.hat_set_led_state.return_value = ok
    assert tools["hat_set_led_state"](8, 99) == {"success": ok, "message": needle}
    bb.hat_set_led_state.assert_called_once_with(8, 99)


@pytest.mark.parametrize("led", [0, 9, -1])
def test_led_state_range(env, led):
    tools, bb = env
    with pytest.raises(ValueError, match="between 1 and 8"):
        tools["hat_set_led_state"](led, 1)
    bb.hat_set_led_state.assert_not_called()


@pytest.mark.parametrize("route,label", [(0, "low-speed"), (1, "high-speed")])
def test_la_route(env, route, label):
    tools, bb = env
    bb.hat_la_set_route.return_value = True
    assert tools["hat_la_set_route"](route) == {"success": True, "route_id": route,
                                                "message": f"LA route set to {label}."}
    bb.hat_la_set_route.assert_called_once_with(route)


def test_la_route_invalid(env):
    tools, bb = env
    with pytest.raises(ValueError, match="Invalid route_id 2"):
        tools["hat_la_set_route"](2)
    bb.hat_la_set_route.assert_not_called()


def test_la_route_failure_message(env):
    tools, bb = env
    bb.hat_la_set_route.return_value = False
    res = tools["hat_la_set_route"](1)
    assert res["success"] is False and "set to" not in res["message"]


@pytest.mark.parametrize("ok", [True, False])
def test_io_bank(env, ok):
    tools, bb = env
    bb.hat_set_io_bank.return_value = ok
    assert tools["hat_set_io_bank"](0x0F, 0x30, 0xC0) == {"success": ok}
    bb.hat_set_io_bank.assert_called_once_with(0x0F, 0x30, 0xC0)


def test_level_shift_passthrough(env):
    tools, bb = env
    bb.hat_set_level_shift.return_value = {"oe": True, "dir": False}
    assert tools["hat_set_level_shift"](True, False) == {"oe": True, "dir": False}
    bb.hat_set_level_shift.assert_called_once_with(True, False)


def test_calibrate_start(env):
    tools, bb = env
    bb.hat_calibrate_start.return_value = {"state": 1}
    with pytest.raises(ValueError, match="confirm=True"):
        tools["hat_calibrate_start"](1)
    with pytest.raises(ValueError, match="Invalid rail_id 0"):
        tools["hat_calibrate_start"](0, confirm=True)
    assert tools["hat_calibrate_start"](2, confirm=True) == {"status": {"state": 1}}
    bb.hat_calibrate_start.assert_called_once_with(2)


def test_calibrate_status_passthrough(env):
    tools, bb = env
    bb.hat_calibrate_status.return_value = {"state": 2, "progress": 100}
    assert tools["hat_calibrate_status"]() == {"state": 2, "progress": 100}


def test_calibrate_import(env):
    tools, bb = env
    bb.hat_calibrate_import.return_value = True
    with pytest.raises(ValueError, match="Invalid rail_id 4"):
        tools["hat_calibrate_import"](4, [], confirm=True)
    with pytest.raises(ValueError, match="dac_code"):
        tools["hat_calibrate_import"](0, [{"dac_code": 1}], confirm=True)
    bb.hat_calibrate_import.assert_not_called()
    res = tools["hat_calibrate_import"](0, [{"dac_code": "-8", "measured_v": "3.4", "x": 1}], confirm=True)
    assert res == {"success": True}
    bb.hat_calibrate_import.assert_called_once_with(0, [{"dac_code": -8, "measured_v": 3.4}])
