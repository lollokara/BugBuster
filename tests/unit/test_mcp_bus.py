"""
Unit tests for side-effect-free MCP bus planning tools.
"""

import unittest
from unittest.mock import MagicMock, patch

from bugbuster_mcp import session
from bugbuster_mcp.tools.bus import register


class DummyMCP:
    def __init__(self):
        self.tools = {}

    def tool(self):
        def decorator(func):
            self.tools[func.__name__] = func
            return func
        return decorator


class TestMCPBusTools(unittest.TestCase):
    def setUp(self):
        session.configure(transport="usb", port="/dev/null", vlogic=3.3)
        self.mcp = DummyMCP()
        register(self.mcp)

    def test_plan_i2c_bus_is_dry_run(self):
        payload = self.mcp.tools["plan_i2c_bus"](
            sda_io=1,
            scl_io=2,
            supply_voltage=3.3,
        )

        self.assertTrue(payload["success"])
        self.assertTrue(payload["dry_run"])
        self.assertEqual(payload["esp_gpios"], {"sda": 4, "scl": 2})
        self.assertEqual(payload["mux_states"], [0x50, 0x00, 0x00, 0x00])
        self.assertIn("0x7F", payload["reserved_addresses_skipped"])

    def test_plan_spi_bus_is_dry_run(self):
        payload = self.mcp.tools["plan_spi_bus"](
            sck_io=1,
            mosi_io=2,
            miso_io=4,
            cs_io=5,
            supply_voltage=3.3,
        )

        self.assertTrue(payload["success"])
        self.assertTrue(payload["dry_run"])
        self.assertEqual(payload["esp_gpios"], {"sck": 4, "mosi": 2, "miso": 7, "cs": 6})
        self.assertEqual(payload["mux_states"], [0x50, 0x50, 0x00, 0x00])

    def test_plan_i2c_bus_validates_io(self):
        with self.assertRaises(ValueError):
            self.mcp.tools["plan_i2c_bus"](sda_io=0, scl_io=2, supply_voltage=3.3)

    def test_scan_i2c_bus_uses_session_client(self):
        fake_bb = MagicMock()
        fake_bb.bus.i2c_scan.return_value = {
            "kind": "i2c_scan",
            "addresses": ["0x50"],
            "address_values": [0x50],
            "count": 1,
            "plan": {"kind": "i2c"},
        }

        with patch("bugbuster_mcp.session.get_client", return_value=fake_bb):
            payload = self.mcp.tools["scan_i2c_bus"](
                sda_io=1,
                scl_io=2,
                supply_voltage=3.3,
            )

        self.assertTrue(payload["success"])
        self.assertEqual(payload["addresses"], ["0x50"])
        fake_bb.bus.i2c_scan.assert_called_once()

    def test_spi_transfer_uses_session_client(self):
        fake_bb = MagicMock()
        fake_plan = MagicMock()
        fake_plan.as_dict.return_value = {"kind": "spi"}
        fake_bb.bus.setup_spi.return_value = fake_plan
        fake_bb.bus.spi_transfer.return_value = bytes([0x12, 0x34])

        with patch("bugbuster_mcp.session.get_client", return_value=fake_bb):
            payload = self.mcp.tools["spi_transfer"](
                sck_io=1,
                mosi_io=2,
                miso_io=4,
                cs_io=5,
                supply_voltage=3.3,
                data=[0x9F, 0x00],
            )

        self.assertTrue(payload["success"])
        self.assertEqual(payload["rx"], [0x12, 0x34])
        fake_bb.bus.setup_spi.assert_called_once()
        fake_bb.bus.spi_transfer.assert_called_once_with([0x9F, 0x00])

    def test_spi_jedec_id_uses_session_client(self):
        fake_bb = MagicMock()
        fake_plan = MagicMock()
        fake_plan.as_dict.return_value = {"kind": "spi"}
        fake_bb.bus.setup_spi.return_value = fake_plan
        fake_bb.bus.spi_jedec_id.return_value = {"jedec_id": "EF4018"}

        with patch("bugbuster_mcp.session.get_client", return_value=fake_bb):
            payload = self.mcp.tools["spi_jedec_id"](
                sck_io=1,
                miso_io=4,
                cs_io=5,
                supply_voltage=3.3,
            )

        self.assertTrue(payload["success"])
        self.assertEqual(payload["jedec_id"], "EF4018")

    def test_defer_spi_transfer_uses_session_client(self):
        fake_bb = MagicMock()
        fake_bb.bus.defer_spi_transfer.return_value = 7

        with patch("bugbuster_mcp.session.get_client", return_value=fake_bb):
            payload = self.mcp.tools["defer_spi_transfer"]([0x9F, 0x00], timeout_ms=25)

        self.assertTrue(payload["success"])
        self.assertEqual(payload["job_id"], 7)
        fake_bb.bus.defer_spi_transfer.assert_called_once_with([0x9F, 0x00], timeout_ms=25)

    def test_deferred_result_uses_session_client(self):
        fake_bb = MagicMock()
        fake_bb.bus.deferred_result.return_value = {
            "job_id": 7,
            "status_name": "done",
            "kind_name": "spi_transfer",
            "data": [0x00, 0xEF],
        }

        with patch("bugbuster_mcp.session.get_client", return_value=fake_bb):
            payload = self.mcp.tools["get_deferred_bus_result"](7)

        self.assertTrue(payload["success"])
        self.assertEqual(payload["data"], [0x00, 0xEF])
        fake_bb.bus.deferred_result.assert_called_once_with(7)


if __name__ == "__main__":
    unittest.main()
