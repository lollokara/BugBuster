"""Coverage for bugbuster_mcp.tools.target: enter_bootloader,
release_bootloader and the target_power_up branches test_mcp_target_power.py
does not reach."""
from __future__ import annotations

import inspect
from unittest.mock import call, patch

import pytest

from bugbuster.client import IdacChannel
from bugbuster.hal import PortMode
from bugbuster_mcp import session
from bugbuster_mcp.tools import target
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


_OK = {"applied_v": 5.0, "efuse_faults": [False, False], "pg": True, "clamped": False}


@pytest.fixture
def env():
    mcp = _DummyMCP()
    target.register(mcp)
    bb = make_client_mock()
    bb.rail_power_up.return_value = dict(_OK)
    bb.mux_get.return_value = [1, 2, 3, 4]
    hal = make_hal_mock()
    with patch("bugbuster_mcp.session.get_client", return_value=bb), \
         patch("bugbuster_mcp.session.get_hal", return_value=hal), \
         patch("bugbuster_mcp.session.get_active_board_profile", return_value=None), \
         patch.object(target.time, "sleep"):
        yield mcp.tools, bb, hal


# ---------------------------------------------------------------------------
# target_power_up leftovers
# ---------------------------------------------------------------------------

def test_power_up_invalid_rail(env):
    tools, bb, _ = env
    with pytest.raises(ValueError, match="rail must be 1 or 2"):
        tools["target_power_up"](rail=3)
    bb.rail_power_up.assert_not_called()


def test_power_up_pg_and_clamp_warnings(env):
    tools, bb, _ = env
    bb.rail_power_up.return_value = {"applied_v": 14.85, "efuse_faults": [], "pg": False, "clamped": True}
    res = tools["target_power_up"](supply_voltage=15.0, rail=1, confirm=True)
    bb.rail_power_up.assert_called_once_with(1, 15.0, 500, confirm=True)
    assert res["success"] is False and res["applied_v"] == 14.85
    assert res["warnings"] == ["VADJ1 power-good signal not asserted - check load.",
                               "VADJ1 clamped to 14.85 V by the DAC limits."]
    assert res["efuses"] == ["EFUSE1", "EFUSE2"]


# ---------------------------------------------------------------------------
# enter_bootloader
# ---------------------------------------------------------------------------

def test_enter_bootloader_sequence(env):
    tools, bb, hal = env
    hal._mux_state = [0, 0, 0, 0]
    res = tools["enter_bootloader"](boot_io=1, tx_io=2, rx_io=3, baudrate=460800,
                                    supply_voltage=3.3, rail=1, settle_ms=200)
    assert bb.method_calls[:3] == [call.dio_configure(1, 2), call.dio_write(1, False),
                                   call.set_uart_config(bridge_id=0, uart_num=1, tx_pin=2, rx_pin=1,
                                                        baudrate=460800, data_bits=8, parity=0,
                                                        stop_bits=0, enabled=True)]
    bb.rail_power_up.assert_called_once_with(1, 3.3, 200, confirm=False, power_cycle=True)
    assert hal._mux_state == [1, 2, 3, 4]
    assert res == {
        "success": True,
        "uart_bridge": {"bridge_id": 0, "tx_io": 2, "tx_gpio": 2, "rx_io": 3, "rx_gpio": 1,
                        "baudrate": 460800,
                        "note": "Serial bridge active on USB CDC #1 (second virtual COM port)."},
        "boot_io": 1,
        "boot_pin": "LOW (held — call release_bootloader after flashing)",
        "warnings": [],
    }


def test_enter_bootloader_without_hal_mux_shadow(env):
    tools, bb, _ = env
    tools["enter_bootloader"](rail=2, tx_io=12, rx_io=7)
    bb.mux_get.assert_not_called()
    kw = bb.set_uart_config.call_args.kwargs
    assert (kw["tx_pin"], kw["rx_pin"]) == (13, 8)
    assert bb.rail_power_up.call_args.args[0] == 2


@pytest.mark.parametrize("kwargs,match", [
    (dict(tx_io=13), "Unsupported IO"),
    (dict(rx_io=0), "Unsupported IO"),
    (dict(rail=3), "rail must be 1 or 2"),
    (dict(supply_voltage=13.0), "confirm=True"),
    (dict(supply_voltage=2.0), "below the minimum"),
])
def test_enter_bootloader_validation_before_any_io(env, kwargs, match):
    tools, bb, _ = env
    with pytest.raises(ValueError, match=match):
        tools["enter_bootloader"](**kwargs)
    bb.dio_configure.assert_not_called()
    bb.rail_power_up.assert_not_called()


def test_enter_bootloader_high_voltage_with_confirm(env):
    tools, bb, _ = env
    tools["enter_bootloader"](supply_voltage=13.0, confirm=True)
    bb.rail_power_up.assert_called_once_with(1, 13.0, 500, confirm=True, power_cycle=True)


def test_enter_bootloader_reports_trip(env):
    tools, bb, _ = env
    bb.rail_power_up.return_value = {"applied_v": 5.0, "efuse_faults": [False, True], "pg": True}
    res = tools["enter_bootloader"](rail=2)
    assert res["success"] is False
    assert res["warnings"] == ["eFuse4 tripped - possible overcurrent. Check wiring."]


# ---------------------------------------------------------------------------
# release_bootloader
# ---------------------------------------------------------------------------

def _idac(v1=5.0, v2=13.0):
    return {"present": True, "channels": [IdacChannel(0, 3.3, 3.3, 1.8, 5.0, True),
                                          IdacChannel(0, v1, v1, 3.0, 15.0, True),
                                          IdacChannel(0, v2, v2, 3.0, 15.0, True)]}


def test_release_bootloader_sequence(env):
    tools, bb, hal = env
    bb.idac_get_status.return_value = _idac()
    res = tools["release_bootloader"](boot_io=1, rail=1)
    assert hal.method_calls == [call.configure(1, PortMode.DIGITAL_OUT), call.write_digital(1, True)]
    target.time.sleep.assert_called_once_with(0.1)
    bb.rail_power_up.assert_called_once_with(1, 5.0, 300, confirm=False)
    assert res == {"success": True, "boot_io": 1, "boot_pin": "HIGH",
                   "note": "Target power-cycled into normal boot mode.", "warnings": []}


def test_release_bootloader_high_rail_sets_confirm(env):
    tools, bb, _ = env
    bb.idac_get_status.return_value = _idac()
    tools["release_bootloader"](rail=2)
    bb.rail_power_up.assert_called_once_with(2, 13.0, 300, confirm=True)


def test_release_bootloader_invalid_rail(env):
    tools, bb, hal = env
    with pytest.raises(ValueError, match="rail must be 1 or 2"):
        tools["release_bootloader"](rail=0)
    hal.configure.assert_not_called()


def test_release_bootloader_warnings(env):
    tools, bb, _ = env
    bb.idac_get_status.return_value = _idac()
    bb.rail_power_up.return_value = {"applied_v": 5.0, "efuse_faults": [True, False], "pg": False}
    res = tools["release_bootloader"](rail=1)
    assert res["success"] is False and len(res["warnings"]) == 2


@pytest.mark.xfail(strict=True, reason="BUG: release_bootloader defaults boot_io=2 while enter_bootloader "
                                       "defaults (and holds low) boot_io=1, so default-arg calls never "
                                       "release the BOOT pin")
def test_bootloader_default_boot_io_match(env):
    tools, _, _ = env
    enter = inspect.signature(tools["enter_bootloader"]).parameters["boot_io"].default
    release = inspect.signature(tools["release_bootloader"]).parameters["boot_io"].default
    assert enter == release
