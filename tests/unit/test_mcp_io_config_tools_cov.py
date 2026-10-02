"""Coverage for bugbuster_mcp.tools.io_config: configure_io,
set_supply_voltage, reset_device."""
from __future__ import annotations

from unittest.mock import patch

import pytest

from bugbuster.hal import PortMode
from bugbuster_mcp import session
from bugbuster_mcp.tools import io_config, io_owner
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
    session.configure(transport="usb", port="/dev/null", vlogic=1.8)
    yield
    (session._transport, session._port, session._host, session._vlogic,
     session._admin_token, session._active_board) = saved


@pytest.fixture
def env():
    mcp = _DummyMCP()
    io_config.register(mcp)
    bb = make_client_mock()
    bb.power_get_status.return_value = {}
    hal = make_hal_mock()
    with patch("bugbuster_mcp.session.get_client", return_value=bb), \
         patch("bugbuster_mcp.session.get_hal", return_value=hal), \
         patch("bugbuster_mcp.session.get_active_board_profile", return_value=None) as prof:
        yield mcp.tools, bb, hal, prof


def test_register_exposes_three_tools():
    mcp = _DummyMCP()
    io_config.register(mcp)
    assert set(mcp.tools) == {"configure_io", "set_supply_voltage", "reset_device"}


# ---------------------------------------------------------------------------
# configure_io
# ---------------------------------------------------------------------------

@pytest.mark.parametrize("io,mode", [
    (1, "DIGITAL_OUT"), (2, "digital_in"), (5, "DIGITAL_IN_LOW"), (7, "Digital_Out_Low"), (11, "DISABLED"),
    (3, "ANALOG_IN"), (6, "ANALOG_OUT"), (9, "CURRENT_IN"), (12, "CURRENT_OUT"),
    (3, "RTD"), (6, "HART"), (9, "HAT"),
])
def test_configure_io_valid_modes(env, io, mode):
    tools, _, hal, _ = env
    res = tools["configure_io"](io, mode)
    expected = PortMode(io_config._PORT_MODE_NAMES[mode.upper()])
    hal.configure.assert_called_once_with(io, expected, bipolar=False)
    assert res == {"io": io, "mode": mode.upper(), "bipolar": False, "success": True, "warnings": []}


def test_configure_io_bipolar_passthrough(env):
    tools, _, hal, _ = env
    assert tools["configure_io"](3, "analog_out", bipolar=True)["bipolar"] is True
    hal.configure.assert_called_once_with(3, PortMode.ANALOG_OUT, bipolar=True)


@pytest.mark.parametrize("mode", ["ANALOG_IN", "ANALOG_OUT", "CURRENT_IN", "CURRENT_OUT", "RTD", "HART", "HAT"])
def test_configure_io_analog_mode_on_digital_io(env, mode):
    tools, _, hal, _ = env
    with pytest.raises(ValueError, match=f"does not support {mode}"):
        tools["configure_io"](4, mode)
    hal.configure.assert_not_called()


@pytest.mark.parametrize("io,mode,match", [(0, "DIGITAL_IN", "not valid"), (13, "DIGITAL_IN", "not valid"),
                                           (1, "PWM", "Unknown mode 'PWM'")])
def test_configure_io_bad_args(env, io, mode, match):
    tools, _, hal, _ = env
    with pytest.raises(ValueError, match=match):
        tools["configure_io"](io, mode)
    hal.configure.assert_not_called()


def test_configure_io_reports_fault_warnings(env):
    tools, bb, _, _ = env
    bb.power_get_status.return_value = {"efuse_faults": [True, False, False, False]}
    assert "E-fuse 1" in tools["configure_io"](1, "DIGITAL_OUT")["warnings"][0]


def test_configure_io_fault_check_crash_becomes_warning(env):
    tools, _, hal, _ = env
    with patch("bugbuster_mcp.safety.check_faults_post", side_effect=RuntimeError("pca gone")):
        res = tools["configure_io"](1, "DIGITAL_OUT")
    assert res["success"] is True
    assert res["warnings"] == ["Could not check faults after configure: pca gone"]


def test_configure_io_hal_error_propagates(env):
    tools, _, hal, _ = env
    hal.configure.side_effect = RuntimeError("mux conflict")
    with pytest.raises(RuntimeError, match="mux conflict"):
        tools["configure_io"](1, "DIGITAL_OUT")


def test_configure_io_lease(env):
    tools, _, hal, _ = env
    with patch.dict(io_owner._active_leases, {"h": [4], "x": [0]}, clear=True):
        tools["configure_io"](5, "DIGITAL_IN", lease_handle="h")
        with pytest.raises(ValueError, match="slot 4"):
            tools["configure_io"](5, "DIGITAL_IN", lease_handle="x")
        with pytest.raises(ValueError, match="Unknown lease"):
            tools["configure_io"](5, "DIGITAL_IN", lease_handle="ghost")
    hal.configure.assert_called_once()


# ---------------------------------------------------------------------------
# set_supply_voltage
# ---------------------------------------------------------------------------

@pytest.mark.parametrize("rail", [1, 2])
def test_set_supply_voltage_happy(env, rail):
    tools, _, hal, _ = env
    assert tools["set_supply_voltage"](rail, 5.0) == {"rail": f"VADJ{rail}", "voltage": 5.0,
                                                     "success": True, "warnings": []}
    hal.set_voltage.assert_called_once_with(rail=rail, voltage=5.0)


def test_set_supply_voltage_refuses_vlogic(env):
    tools, _, hal, _ = env
    with pytest.raises(ValueError, match="VLOGIC cannot be changed.*1.8 V"):
        tools["set_supply_voltage"](0, 3.3)
    hal.set_voltage.assert_not_called()


@pytest.mark.parametrize("rail", [-1, 3])
def test_set_supply_voltage_invalid_rail(env, rail):
    tools, _, hal, _ = env
    with pytest.raises(ValueError, match=f"Invalid rail {rail}"):
        tools["set_supply_voltage"](rail, 5.0)
    hal.set_voltage.assert_not_called()


@pytest.mark.parametrize("v,confirm,match", [(2.9, False, "below the minimum"), (15.1, True, "hardware maximum"),
                                             (12.5, False, "confirm=True")])
def test_set_supply_voltage_limits(env, v, confirm, match):
    tools, _, hal, _ = env
    with pytest.raises(ValueError, match=match):
        tools["set_supply_voltage"](1, v, confirm=confirm)
    hal.set_voltage.assert_not_called()


def test_set_supply_voltage_above_12v_with_confirm(env):
    tools, _, hal, _ = env
    tools["set_supply_voltage"](2, 14.0, confirm=True)
    hal.set_voltage.assert_called_once_with(rail=2, voltage=14.0)


def test_set_supply_voltage_locked_profile(env):
    tools, _, hal, prof = env
    prof.return_value = {"name": "dut", "vadj1": {"value": 3.3, "locked": True}}
    with pytest.raises(ValueError, match="locked"):
        tools["set_supply_voltage"](1, 5.0)
    tools["set_supply_voltage"](1, 3.3)
    hal.set_voltage.assert_called_once_with(rail=1, voltage=3.3)


def test_set_supply_voltage_lease_covers_rail_block(env):
    tools, _, hal, _ = env
    leases = {"r1": list(range(6)), "r2": list(range(6, 12)), "part": [0, 1, 2]}
    with patch.dict(io_owner._active_leases, leases, clear=True):
        tools["set_supply_voltage"](1, 5.0, lease_handle="r1")
        tools["set_supply_voltage"](2, 5.0, lease_handle="r2")
        with pytest.raises(ValueError, match="slot 6"):
            tools["set_supply_voltage"](2, 5.0, lease_handle="r1")
        with pytest.raises(ValueError, match="slot 3"):
            tools["set_supply_voltage"](1, 5.0, lease_handle="part")
        # no slots to check for a bad rail: the rail error wins
        with pytest.raises(ValueError, match="VLOGIC"):
            tools["set_supply_voltage"](0, 3.3, lease_handle="ghost")
    assert hal.set_voltage.call_count == 2


def test_set_supply_voltage_fault_warnings(env):
    tools, bb, _, _ = env
    bb.power_get_status.return_value = {"vadj2_en": True, "vadj2_pg": False}
    assert any("VADJ2" in w for w in tools["set_supply_voltage"](2, 5.0)["warnings"])
    with patch("bugbuster_mcp.safety.check_faults_post", side_effect=RuntimeError("x")):
        res = tools["set_supply_voltage"](2, 5.0)
    assert res["warnings"] == ["Could not check faults after voltage change: x"]


# ---------------------------------------------------------------------------
# reset_device
# ---------------------------------------------------------------------------

def test_reset_device_resets_then_reinitializes(env):
    tools, _, hal, _ = env
    calls = []
    with patch("bugbuster_mcp.session.reset_session", side_effect=lambda: calls.append("reset")), \
         patch("bugbuster_mcp.session.get_hal", side_effect=lambda: calls.append("hal") or hal):
        res = tools["reset_device"]()
    assert calls == ["reset", "hal"]
    assert res["success"] is True and "DISABLED" in res["message"]


def test_reset_device_reinit_failure_propagates(env):
    tools, _, _, _ = env
    with patch("bugbuster_mcp.session.reset_session"), \
         patch("bugbuster_mcp.session.get_hal", side_effect=OSError("port busy")):
        with pytest.raises(OSError):
            tools["reset_device"]()
