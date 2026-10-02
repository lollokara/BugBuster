"""Coverage for bugbuster_mcp.tools.bus: deferred I2C/SPI jobs plus the
validation/lease branches test_mcp_bus.py and test_mcp_bus_rw.py skip."""
from __future__ import annotations

from unittest.mock import patch

import pytest

from bugbuster_mcp import session
from bugbuster_mcp.tools import bus, io_owner
from tests.unit._mock_client import make_client_mock


class _DummyMCP:
    def __init__(self):
        self.tools = {}

    def tool(self):
        def deco(fn):
            self.tools[fn.__name__] = fn
            return fn
        return deco


class _Plan:
    def as_dict(self):
        return {"route": "ok"}


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
    bus.register(mcp)
    bb = make_client_mock()
    bb.bus.setup_spi.return_value = _Plan()
    bb.bus.i2c_scan.return_value = {"addresses": [0x48]}
    with patch("bugbuster_mcp.session.get_client", return_value=bb), \
         patch("bugbuster_mcp.session.get_active_board_profile", return_value=None):
        yield mcp.tools, bb


# ---------------------------------------------------------------------------
# deferred jobs
# ---------------------------------------------------------------------------

def test_defer_i2c_read(env):
    tools, bb = env
    bb.bus.defer_i2c_read.return_value = 41
    assert tools["defer_i2c_read"](0x50, 8, timeout_ms=30) == {"success": True, "job_id": 41, "kind": "i2c_read"}
    bb.bus.defer_i2c_read.assert_called_once_with(0x50, 8, timeout_ms=30)


def test_defer_i2c_read_client_validation_propagates(env):
    tools, bb = env
    bb.bus.defer_i2c_read.side_effect = ValueError("I2C deferred read length must be 1-255 bytes")
    with pytest.raises(ValueError, match="1-255"):
        tools["defer_i2c_read"](0x50, 0)


def test_defer_i2c_write_read(env):
    tools, bb = env
    bb.bus.defer_i2c_write_read.return_value = 7
    res = tools["defer_i2c_write_read"](0x48, [0x00, 0xFF], 2)
    bb.bus.defer_i2c_write_read.assert_called_once_with(0x48, [0x00, 0xFF], 2, timeout_ms=100)
    assert res == {"success": True, "job_id": 7, "kind": "i2c_write_read"}


@pytest.mark.parametrize("data", [[], [256], [-1], [1.0], ["a"], [0] * 256])
def test_defer_i2c_write_read_rejects_bad_payload(env, data):
    tools, bb = env
    with pytest.raises(ValueError, match="write_data must be 1-255"):
        tools["defer_i2c_write_read"](0x48, data, 1)
    bb.bus.defer_i2c_write_read.assert_not_called()


def test_defer_i2c_write_read_accepts_255_bytes(env):
    tools, bb = env
    tools["defer_i2c_write_read"](0x48, [0] * 255, 1)
    bb.bus.defer_i2c_write_read.assert_called_once()


def test_defer_spi_transfer_kwargs(env):
    tools, bb = env
    bb.bus.defer_spi_transfer.return_value = 3
    assert tools["defer_spi_transfer"]([0] * 512) == {"success": True, "job_id": 3, "kind": "spi_transfer"}
    bb.bus.defer_spi_transfer.assert_called_once_with([0] * 512, timeout_ms=100)


@pytest.mark.parametrize("data", [[], [0] * 513, [300], [-5]])
def test_defer_spi_transfer_rejects_bad_payload(env, data):
    tools, bb = env
    with pytest.raises(ValueError, match="1-512"):
        tools["defer_spi_transfer"](data)
    bb.bus.defer_spi_transfer.assert_not_called()


def test_get_deferred_bus_result_adds_success(env):
    tools, bb = env
    bb.bus.deferred_result.return_value = {"state": "done", "data": [1, 2]}
    assert tools["get_deferred_bus_result"](9) == {"state": "done", "data": [1, 2], "success": True}
    bb.bus.deferred_result.assert_called_once_with(9)


def test_get_deferred_bus_result_error_propagates(env):
    tools, bb = env
    bb.bus.deferred_result.side_effect = NotImplementedError("USB only")
    with pytest.raises(NotImplementedError):
        tools["get_deferred_bus_result"](1)


# ---------------------------------------------------------------------------
# scan / spi lease + validation branches
# ---------------------------------------------------------------------------

def test_scan_lease_must_cover_both_pins(env):
    tools, bb = env
    with patch.dict(io_owner._active_leases, {"h": [0, 1], "p": [0]}, clear=True):
        res = tools["scan_i2c_bus"](sda_io=1, scl_io=2, supply_voltage=3.3, lease_handle="h")
        assert res["success"] is True
        with pytest.raises(ValueError, match="slot 1"):
            tools["scan_i2c_bus"](sda_io=1, scl_io=2, supply_voltage=3.3, lease_handle="p")
    bb.bus.i2c_scan.assert_called_once()


def test_spi_transfer_lease_covers_optional_pins(env):
    tools, bb = env
    bb.bus.spi_transfer.return_value = b"\x01\x02"
    with patch.dict(io_owner._active_leases, {"h": [0, 1, 2], "p": [0, 1]}, clear=True):
        res = tools["spi_transfer"](sck_io=1, supply_voltage=3.3, data=[1, 2], mosi_io=2, cs_io=3,
                                    lease_handle="h")
        assert res == {"success": True, "rx": [1, 2], "plan": {"route": "ok"}}
        with pytest.raises(ValueError, match="slot 2"):
            tools["spi_transfer"](sck_io=1, supply_voltage=3.3, data=[1], mosi_io=2, cs_io=3, lease_handle="p")


@pytest.mark.parametrize("kwargs,match", [
    (dict(mosi_io=13), "not valid"),
    (dict(data=[]), "1-512"),
    (dict(data=[256]), "1-512"),
    (dict(supply_voltage=14.0), "confirm"),
])
def test_spi_transfer_validation(env, kwargs, match):
    tools, bb = env
    args = dict(sck_io=1, supply_voltage=3.3, data=[0])
    args.update(kwargs)
    with pytest.raises(ValueError, match=match):
        tools["spi_transfer"](**args)
    bb.bus.setup_spi.assert_not_called()


def test_spi_jedec_validates_optional_mosi(env):
    tools, bb = env
    with pytest.raises(ValueError, match="not valid"):
        tools["spi_jedec_id"](sck_io=1, miso_io=2, cs_io=3, supply_voltage=3.3, mosi_io=0)
    bb.bus.setup_spi.assert_not_called()


def test_i2c_dump_registers_start_reg_range(env):
    tools, bb = env
    with pytest.raises(ValueError, match="start_reg"):
        tools["i2c_dump_registers"](0x50, start_reg=256)
    bb.bus.i2c_write_read.assert_not_called()


def test_spi_flash_read_address_range(env):
    tools, bb = env
    with pytest.raises(ValueError, match="24-bit"):
        tools["spi_flash_read"](0x1000000, 4)
    bb.bus.spi_transfer.assert_not_called()


@pytest.mark.xfail(strict=True, reason="BUG: _check_len accepts 256 but the client/BBP length field is "
                                       "one byte (ext_i2c_read requires 1-255), so length=256 passes "
                                       "MCP validation and fails later in struct.pack")
def test_i2c_read_rejects_256_bytes_up_front(env):
    tools, bb = env
    with pytest.raises(ValueError):
        tools["i2c_read"](0x50, 256)
    bb.bus.i2c_read.assert_not_called()
