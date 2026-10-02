"""BUS-011 / BUS-016: MCP tools for read/write on an already-configured
external I2C/SPI bus, plus register-dump and SPI-flash-read helpers.

A: only scan / one-shot SPI / deferred jobs existed; a plain I2C write, read or
register read had to go through Python."""

import unittest
from unittest.mock import patch

import pytest

from bugbuster_mcp import session
from bugbuster_mcp.tools.bus import register
from tests.unit._mock_client import make_client_mock


class DummyMCP:
    def __init__(self):
        self.tools = {}

    def tool(self):
        def decorator(func):
            self.tools[func.__name__] = func
            return func
        return decorator


class TestBusReadWrite(unittest.TestCase):
    def setUp(self):
        session.configure(transport="usb", port="/dev/null", vlogic=3.3)
        self.mcp = DummyMCP()
        register(self.mcp)
        self.bb = make_client_mock()
        p = patch("bugbuster_mcp.session.get_client", return_value=self.bb)
        p.start()
        self.addCleanup(p.stop)

    @pytest.mark.xfail(strict=True, reason="BUS-011")
    def test_i2c_write_read_and_write_read(self):
        self.bb.bus.i2c_write.return_value = 2
        self.bb.bus.i2c_read.return_value = b"\x12\x34"
        self.bb.bus.i2c_write_read.return_value = b"\xAB"
        t = self.mcp.tools
        self.assertEqual(t["i2c_write"](address=0x50, data=[0x00, 0x10])["written"], 2)
        self.assertEqual(t["i2c_read"](address=0x50, length=2)["data"], [0x12, 0x34])
        self.assertEqual(t["i2c_write_read"](address=0x50, write_data=[0x0F], read_length=1)["data"], [0xAB])
        self.bb.bus.i2c_write.assert_called_once_with(0x50, [0x00, 0x10], timeout_ms=100)

    @pytest.mark.xfail(strict=True, reason="BUS-011")
    def test_i2c_rejects_bad_address_and_lengths(self):
        t = self.mcp.tools
        with self.assertRaises(ValueError):
            t["i2c_read"](address=0x80, length=1)
        with self.assertRaises(ValueError):
            t["i2c_read"](address=0x50, length=0)
        with self.assertRaises(ValueError):
            t["i2c_write"](address=0x50, data=[256])

    @pytest.mark.xfail(strict=True, reason="BUS-011")
    def test_setup_and_status(self):
        plan = self.bb.bus.setup_i2c.return_value
        plan.as_dict.return_value = {"kind": "i2c"}
        self.bb.bus.status.return_value = {"sessions": [{"kind": "i2c"}]}
        t = self.mcp.tools
        self.assertEqual(t["setup_i2c_bus"](sda_io=1, scl_io=2, supply_voltage=3.3)["plan"], {"kind": "i2c"})
        self.assertEqual(t["bus_status"]()["sessions"], [{"kind": "i2c"}])

    @pytest.mark.xfail(strict=True, reason="BUS-016")
    def test_i2c_dump_registers(self):
        self.bb.bus.i2c_write_read.return_value = bytes(range(4))
        r = self.mcp.tools["i2c_dump_registers"](address=0x50, start_reg=0x10, count=4)
        self.assertEqual(r["registers"], {"0x10": 0, "0x11": 1, "0x12": 2, "0x13": 3})
        self.bb.bus.i2c_write_read.assert_called_once_with(0x50, [0x10], 4, timeout_ms=100)

    @pytest.mark.xfail(strict=True, reason="BUS-016")
    def test_spi_flash_read(self):
        self.bb.bus.spi_transfer.return_value = bytes([0, 0, 0, 0]) + b"\xDE\xAD"
        r = self.mcp.tools["spi_flash_read"](address=0x012345, length=2)
        self.assertEqual(r["data"], [0xDE, 0xAD])
        self.bb.bus.spi_transfer.assert_called_once_with([0x03, 0x01, 0x23, 0x45, 0, 0], timeout_ms=100)
