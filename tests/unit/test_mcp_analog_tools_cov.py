"""Coverage for bugbuster_mcp.tools.analog: read_voltage, read_current,
read_resistance, write_voltage, write_current (observe_adc lives in
test_mcp_observe.py)."""
from __future__ import annotations

from unittest.mock import patch

import pytest

from bugbuster.hal import PortMode
from bugbuster_mcp import session
from bugbuster_mcp.tools import analog, io_owner
from tests.unit._mock_client import make_client_mock, make_hal_mock


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
    analog.register(mcp)
    bb = make_client_mock()
    bb.power_get_status.return_value = {}
    hal = make_hal_mock()
    hal._io_mode = {}
    with patch("bugbuster_mcp.session.get_client", return_value=bb), \
         patch("bugbuster_mcp.session.get_hal", return_value=hal):
        yield mcp.tools, bb, hal


# ---------------------------------------------------------------------------
# reads
# ---------------------------------------------------------------------------

def test_read_voltage_rounds_to_microvolts(env):
    tools, _, hal = env
    hal._io_mode = {6: PortMode.ANALOG_IN}
    hal.read_voltage.return_value = 1.23456789
    assert tools["read_voltage"](6) == {"io": 6, "voltage_v": 1.234568, "unit": "V"}
    hal.read_voltage.assert_called_once_with(6)


def test_read_current(env):
    tools, _, hal = env
    hal._io_mode = {9: PortMode.CURRENT_IN}
    hal.read_current.return_value = 12.0000004
    assert tools["read_current"](9) == {"io": 9, "current_ma": 12.0, "unit": "mA"}


@pytest.mark.parametrize("tool,mode", [("read_voltage", PortMode.CURRENT_IN),
                                       ("read_current", PortMode.ANALOG_IN),
                                       ("read_resistance", PortMode.ANALOG_IN)])
def test_reads_require_matching_mode(env, tool, mode):
    tools, _, hal = env
    hal._io_mode = {3: mode}
    with pytest.raises(ValueError, match=f"mode {mode.name}"):
        tools[tool](3)
    hal.read_voltage.assert_not_called()
    hal.read_current.assert_not_called()
    hal.read_resistance.assert_not_called()


@pytest.mark.parametrize("tool", ["read_voltage", "read_current", "read_resistance"])
@pytest.mark.parametrize("io,match", [(4, "does not support"), (0, "not valid")])
def test_reads_reject_non_analog_io(env, tool, io, match):
    tools, _, _ = env
    with pytest.raises(ValueError, match=match):
        tools[tool](io)


def test_read_resistance_raw_ohms(env):
    tools, _, hal = env
    hal._io_mode = {12: PortMode.RTD}
    hal.read_resistance.return_value = 109.735123
    assert tools["read_resistance"](12) == {"io": 12, "resistance_ohm": 109.7351, "unit": "Ω"}
    hal.read_temperature_pt100.assert_not_called()
    hal.read_temperature_pt1000.assert_not_called()


@pytest.mark.parametrize("as_temp", ["none", "thermistor"])
def test_read_resistance_unknown_conversion_returns_ohms(env, as_temp):
    tools, _, hal = env
    hal._io_mode = {3: PortMode.RTD}
    hal.read_resistance.return_value = 100.0
    res = tools["read_resistance"](3, as_temp=as_temp)
    assert res["unit"] == "Ω" and "temperature_c" not in res


@pytest.mark.parametrize("as_temp,method,unit", [
    ("pt100", "read_temperature_pt100", "°C (PT100)"),
    (" PT1000 ", "read_temperature_pt1000", "°C (PT1000)"),
])
def test_read_resistance_temperature(env, as_temp, method, unit):
    tools, _, hal = env
    hal._io_mode = {3: PortMode.RTD}
    hal.read_resistance.return_value = 1097.35
    getattr(hal, method).return_value = 25.12345
    res = tools["read_resistance"](3, as_temp=as_temp)
    assert res == {"io": 3, "resistance_ohm": 1097.35, "temperature_c": 25.123, "unit": unit}
    getattr(hal, method).assert_called_once_with(3)


def test_read_error_propagates(env):
    tools, _, hal = env
    hal._io_mode = {3: PortMode.RTD}
    hal.read_resistance.side_effect = TimeoutError("adc")
    with pytest.raises(TimeoutError):
        tools["read_resistance"](3)


# ---------------------------------------------------------------------------
# write_voltage
# ---------------------------------------------------------------------------

def test_write_voltage_happy(env):
    tools, _, hal = env
    hal._io_mode = {3: PortMode.ANALOG_OUT}
    assert tools["write_voltage"](3, 5.0) == {"io": 3, "voltage_v": 5.0, "success": True, "warnings": []}
    hal.write_voltage.assert_called_once_with(3, 5.0, bipolar=False)


def test_write_voltage_bipolar_negative(env):
    tools, _, hal = env
    hal._io_mode = {3: PortMode.ANALOG_OUT}
    tools["write_voltage"](3, -5.0, bipolar=True)
    hal.write_voltage.assert_called_once_with(3, -5.0, bipolar=True)


@pytest.mark.parametrize("v,bipolar", [(-0.1, False), (12.01, False), (-12.5, True)])
def test_write_voltage_range(env, v, bipolar):
    tools, _, hal = env
    hal._io_mode = {3: PortMode.ANALOG_OUT}
    with pytest.raises(ValueError):
        tools["write_voltage"](3, v, bipolar=bipolar)
    hal.write_voltage.assert_not_called()


def test_write_voltage_wrong_mode(env):
    tools, _, hal = env
    hal._io_mode = {3: PortMode.ANALOG_IN}
    with pytest.raises(ValueError, match="ANALOG_OUT"):
        tools["write_voltage"](3, 1.0)
    hal.write_voltage.assert_not_called()


def test_write_voltage_reports_faults(env):
    tools, bb, hal = env
    hal._io_mode = {6: PortMode.ANALOG_OUT}
    bb.power_get_status.return_value = {"efuse_faults": [False, True, False, False]}
    assert "E-fuse 2" in tools["write_voltage"](6, 1.0)["warnings"][0]


@pytest.mark.parametrize("io,slot", [(3, 12), (6, 13), (9, 14), (12, 15)])
def test_write_voltage_lease_maps_analog_slot(env, io, slot):
    tools, _, hal = env
    hal._io_mode = {io: PortMode.ANALOG_OUT}
    with patch.dict(io_owner._active_leases, {"h": [slot], "other": [0]}, clear=True):
        assert tools["write_voltage"](io, 1.0, lease_handle="h")["success"] is True
        with pytest.raises(ValueError, match=f"slot {slot}"):
            tools["write_voltage"](io, 1.0, lease_handle="other")
    hal.write_voltage.assert_called_once()


def test_write_voltage_unknown_lease(env):
    tools, _, hal = env
    hal._io_mode = {3: PortMode.ANALOG_OUT}
    with patch.dict(io_owner._active_leases, {}, clear=True):
        with pytest.raises(ValueError, match="Unknown lease"):
            tools["write_voltage"](3, 1.0, lease_handle="ghost")
    hal.write_voltage.assert_not_called()


# ---------------------------------------------------------------------------
# write_current
# ---------------------------------------------------------------------------

def test_write_current_happy(env):
    tools, _, hal = env
    hal._io_mode = {12: PortMode.CURRENT_OUT}
    assert tools["write_current"](12, 4.0) == {"io": 12, "current_ma": 4.0, "success": True, "warnings": []}
    hal.write_current.assert_called_once_with(12, 4.0)


def test_write_current_safe_limit_and_full_range(env):
    tools, _, hal = env
    hal._io_mode = {12: PortMode.CURRENT_OUT}
    with pytest.raises(ValueError, match="allow_full_range"):
        tools["write_current"](12, 20.0)
    hal.write_current.assert_not_called()
    tools["write_current"](12, 20.0, allow_full_range=True)
    hal.write_current.assert_called_once_with(12, 20.0)
    with pytest.raises(ValueError):
        tools["write_current"](12, 25.5, allow_full_range=True)
    with pytest.raises(ValueError):
        tools["write_current"](12, -1.0)


def test_write_current_bipolar_compliance_guard(env):
    tools, _, hal = env
    hal._io_mode = {3: PortMode.CURRENT_OUT}
    with pytest.raises(ValueError, match="10 V device limit"):
        tools["write_current"](3, 2.0, bipolar=True, voltage=10.5)
    hal.write_current.assert_not_called()
    tools["write_current"](3, 2.0, bipolar=True, voltage=10.0)
    tools["write_current"](3, 2.0, bipolar=False, voltage=15.0)
    assert hal.write_current.call_count == 2


def test_write_current_wrong_mode_and_io(env):
    tools, _, hal = env
    hal._io_mode = {3: PortMode.ANALOG_OUT}
    with pytest.raises(ValueError, match="CURRENT_OUT"):
        tools["write_current"](3, 1.0)
    with pytest.raises(ValueError, match="does not support"):
        tools["write_current"](5, 1.0)
    hal.write_current.assert_not_called()


def test_write_current_lease(env):
    tools, _, hal = env
    hal._io_mode = {9: PortMode.CURRENT_OUT}
    with patch.dict(io_owner._active_leases, {"ok": [14], "bad": [12]}, clear=True):
        tools["write_current"](9, 1.0, lease_handle="ok")
        with pytest.raises(ValueError, match="slot 14"):
            tools["write_current"](9, 1.0, lease_handle="bad")
    hal.write_current.assert_called_once_with(9, 1.0)


def test_observe_adc_current_and_bad_quantity(env):
    tools, _, hal = env
    hal._io_mode = {9: PortMode.CURRENT_IN}
    hal.read_current.side_effect = [4.0, 6.0]
    with patch.object(analog.time, "sleep") as sl:
        res = tools["observe_adc"](9, seconds=0.2, rate_hz=10, quantity="current")
    sl.assert_called_once_with(0.1)
    assert res == {"io": 9, "quantity": "current", "unit": "mA", "seconds": 0.2,
                   "count": 2, "min": 4.0, "max": 6.0, "mean": 5.0, "std": 1.0}
    with pytest.raises(ValueError, match="quantity must be"):
        tools["observe_adc"](9, quantity="power")


def test_summarize_and_check_window_helpers():
    assert analog.summarize([]) == {"count": 0}
    s = analog.summarize([1.0, 3.0])
    assert s == {"count": 2, "min": 1.0, "max": 3.0, "mean": 2.0, "std": 1.0}
    for secs, rate in [(0, 1), (61, 1), (1, 0), (1, 51)]:
        with pytest.raises(ValueError):
            analog.check_window(secs, rate)
    analog.check_window(60, 50)
