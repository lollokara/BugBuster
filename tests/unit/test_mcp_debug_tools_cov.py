"""Coverage for bugbuster_mcp.tools.debug: setup_serial_bridge, setup_swd and
uart_config (parity mapping is in test_mcp_uart.py)."""
from __future__ import annotations

from unittest.mock import patch

import pytest

from bugbuster_mcp import session
from bugbuster_mcp.tools import debug
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
    debug.register(mcp)
    bb = make_client_mock()
    bb.power_get_status.return_value = {}
    bb.hat_status_cached.return_value = {"detected": True}
    bb.hat_setup_swd.return_value = True
    hal = make_hal_mock()
    with patch("bugbuster_mcp.session.get_client", return_value=bb), \
         patch("bugbuster_mcp.session.get_hal", return_value=hal):
        yield mcp.tools, bb, hal


# ---------------------------------------------------------------------------
# setup_serial_bridge
# ---------------------------------------------------------------------------

def test_serial_bridge_happy(env):
    tools, _, hal = env
    res = tools["setup_serial_bridge"](tx_io=2, rx_io=3, baudrate=921600, bridge=1)
    hal.set_serial.assert_called_once_with(tx=2, rx=3, baudrate=921600, bridge=1)
    assert {k: res[k] for k in ("tx_io", "rx_io", "baudrate", "bridge", "success")} == \
        {"tx_io": 2, "rx_io": 3, "baudrate": 921600, "bridge": 1, "success": True}
    assert "bridge 1" in res["note"] and "warnings" not in res


def test_serial_bridge_warnings(env):
    tools, bb, _ = env
    bb.power_get_status.return_value = {"vadj2_en": True, "vadj2_pg": False}
    res = tools["setup_serial_bridge"](tx_io=7, rx_io=8)
    assert any("VADJ2" in w for w in res["warnings"])


@pytest.mark.parametrize("kwargs,match", [
    (dict(tx_io=0, rx_io=2), "not valid"),
    (dict(tx_io=1, rx_io=13), "not valid"),
    (dict(tx_io=4, rx_io=4), "must be different"),
    (dict(tx_io=1, rx_io=2, baudrate=12345), "not a standard value"),
    (dict(tx_io=1, rx_io=2, bridge=2), "Bridge must be 0 or 1"),
])
def test_serial_bridge_validation(env, kwargs, match):
    tools, _, hal = env
    with pytest.raises(ValueError, match=match):
        tools["setup_serial_bridge"](**kwargs)
    hal.set_serial.assert_not_called()


def test_serial_bridge_hal_error_propagates(env):
    tools, _, hal = env
    hal.set_serial.side_effect = RuntimeError("mux busy")
    with pytest.raises(RuntimeError, match="mux busy"):
        tools["setup_serial_bridge"](tx_io=1, rx_io=2)


# ---------------------------------------------------------------------------
# setup_swd
# ---------------------------------------------------------------------------

@pytest.mark.parametrize("mv,connector,label", [(3300, 0, "A"), (1800, 1, "B"), (4200, 0, "A")])
def test_swd_happy(env, mv, connector, label):
    tools, bb, _ = env
    res = tools["setup_swd"](target_voltage_mv=mv, connector=connector)
    bb.hat_setup_swd.assert_called_once_with(target_voltage_mv=mv, connector=connector)
    assert res["success"] is True and res["target_voltage_mv"] == mv and res["connector"] == connector
    assert f"connector: {label}" in res["message"] and f"{mv / 1000:.2f} V" in res["message"]
    assert "warnings" not in res


@pytest.mark.parametrize("mv", [1199, 5501, 0])
def test_swd_voltage_range(env, mv):
    tools, bb, _ = env
    with pytest.raises(ValueError, match="1200-5500"):
        tools["setup_swd"](target_voltage_mv=mv)
    bb.hat_setup_swd.assert_not_called()


def test_swd_requires_hat(env):
    tools, bb, _ = env
    bb.hat_status_cached.return_value = {"detected": False}
    with pytest.raises(RuntimeError, match="No HAT"):
        tools["setup_swd"]()
    bb.hat_setup_swd.assert_not_called()


def test_swd_over_http_is_explained(env):
    tools, bb, _ = env
    bb.hat_status_cached.side_effect = NotImplementedError
    with pytest.raises(RuntimeError, match="USB transport"):
        tools["setup_swd"]()


def test_swd_nack(env):
    tools, bb, _ = env
    bb.hat_setup_swd.return_value = False
    with pytest.raises(RuntimeError, match="did not acknowledge"):
        tools["setup_swd"]()


def test_swd_warnings(env):
    tools, bb, _ = env
    bb.power_get_status.return_value = {"efuse_faults": [True]}
    assert "E-fuse 1" in tools["setup_swd"]()["warnings"][0]


# ---------------------------------------------------------------------------
# uart_config
# ---------------------------------------------------------------------------

_CFG = [
    {"bridge_id": 0, "uart_num": 1, "tx_pin": 4, "rx_pin": 2, "baudrate": 9600,
     "data_bits": 7, "parity": 2, "stop_bits": 2, "enabled": True},
    {"bridge_id": 1, "uart_num": 2, "tx_pin": 8, "rx_pin": 9, "baudrate": 57600,
     "data_bits": 8, "parity": 0, "stop_bits": 1, "enabled": False},
]


def test_uart_config_read_only(env):
    tools, bb, _ = env
    bb.get_uart_config.return_value = _CFG
    assert tools["uart_config"](bridge_id=1) == {"bridge_id": 1, "config": _CFG, "updated": False}
    bb.set_uart_config.assert_not_called()


def test_uart_config_partial_update_keeps_current_values(env):
    tools, bb, _ = env
    bb.get_uart_config.return_value = _CFG
    res = tools["uart_config"](bridge_id=0, baudrate=115200)
    bb.set_uart_config.assert_called_once_with(
        bridge_id=0, uart_num=1, tx_pin=4, rx_pin=2, baudrate=115200,
        data_bits=7, parity=2, stop_bits=2, enabled=True,
    )
    assert res == {"bridge_id": 0, "updated": True, "config": {
        "baudrate": 115200, "data_bits": 7, "parity": "unchanged", "stop_bits": 2}}


def test_uart_config_full_update_on_bridge_1(env):
    tools, bb, _ = env
    bb.get_uart_config.return_value = _CFG
    tools["uart_config"](bridge_id=1, baudrate=9600, tx_pin=0, rx_pin=0, data_bits=5,
                         parity="ODD", stop_bits=2, enabled=True)
    bb.set_uart_config.assert_called_once_with(
        bridge_id=1, uart_num=2, tx_pin=0, rx_pin=0, baudrate=9600,
        data_bits=5, parity=1, stop_bits=2, enabled=True,
    )


def test_uart_config_disable_with_false(env):
    tools, bb, _ = env
    bb.get_uart_config.return_value = _CFG
    tools["uart_config"](bridge_id=0, enabled=False)
    assert bb.set_uart_config.call_args.kwargs["enabled"] is False


def test_uart_config_unknown_bridge_falls_back_to_defaults(env):
    tools, bb, _ = env
    bb.get_uart_config.return_value = _CFG[:1]
    tools["uart_config"](bridge_id=1, enabled=True)
    bb.set_uart_config.assert_called_once_with(
        bridge_id=1, uart_num=2, tx_pin=1, rx_pin=2, baudrate=115200,
        data_bits=8, parity=0, stop_bits=1, enabled=True,
    )


def test_uart_config_bad_parity(env):
    tools, bb, _ = env
    bb.get_uart_config.return_value = _CFG
    with pytest.raises(ValueError, match="parity must be"):
        tools["uart_config"](parity="mark")
    bb.set_uart_config.assert_not_called()


def test_uart_config_read_error_propagates(env):
    tools, bb, _ = env
    bb.get_uart_config.side_effect = TimeoutError("cdc")
    with pytest.raises(TimeoutError):
        tools["uart_config"]()
