"""
Unit tests for external I2C/SPI bus route planning.

Runs WITHOUT hardware — the planner must not touch the transport.
"""

import unittest
from unittest.mock import MagicMock

from bugbuster import BugBuster
from bugbuster.bus import BugBusterBusManager, BusPlanError
from bugbuster.constants import PowerControl


class DummyTransport:
    def __init__(self):
        self.posts = []
        self.gets = []

    def post(self, path, body=None, headers=None):
        self.posts.append((path, body, headers))
        if path == "/bus/i2c/setup":
            return {"sdaGpio": body["sdaGpio"], "sclGpio": body["sclGpio"], "frequencyHz": body["frequencyHz"]}
        if path == "/bus/i2c/scan":
            return {"addresses": [0x50], "count": 1}
        if path == "/bus/i2c/read":
            return {"data": [1, 2, 3]}
        if path == "/bus/i2c/write_read":
            return {"data": [4, 5]}
        if path == "/bus/i2c/write":
            return {"written": len(body["data"])}
        if path == "/bus/spi/setup":
            return {"sckGpio": body["sckGpio"], "frequencyHz": body["frequencyHz"], "mode": body["mode"]}
        if path == "/bus/spi/transfer":
            return {"data": list(reversed(body["data"]))}
        return {}

    def get(self, path, params=None):
        self.gets.append((path, params))
        if path == "/bus/status":
            return {"i2c": {"ready": True, "sdaGpio": 2}, "spi": {"ready": False}}
        return {}


def _bus() -> BugBusterBusManager:
    return BugBusterBusManager(DummyTransport())


class TestI2CBusPlanning(unittest.TestCase):
    def test_i2c_plan_resolves_mux_power_and_gpio(self):
        plan = _bus().plan_i2c(
            sda=2,
            scl=3,
            io_voltage=3.3,
            supply_voltage=3.3,
        )

        self.assertEqual(plan.kind, "i2c")
        self.assertEqual(plan.pins, {"sda": 2, "scl": 3})
        self.assertEqual(plan.esp_gpios, {"sda": 2, "scl": 4})
        self.assertEqual(plan.mux_states, (0x50, 0x00, 0x00, 0x00))
        self.assertEqual(plan.supplies, ("VADJ1",))
        self.assertEqual(plan.efuses, ("EFUSE1",))
        self.assertEqual(plan.reserved_addresses_skipped[0], 0x00)
        self.assertEqual(plan.reserved_addresses_skipped[-1], 0x7F)
        self.assertIn("set VLOGIC to 3.3 V", plan.side_effects)
        self.assertIn("enable VADJ1 and set to 3.3 V", plan.side_effects)
        self.assertIn("enable EFUSE1", plan.side_effects)

    def test_i2c_plan_serializes_reserved_scan_addresses(self):
        payload = _bus().plan_i2c(
            sda=2,
            scl=3,
            io_voltage=3.3,
            supply_voltage=3.3,
        ).as_dict()

        self.assertEqual(payload["mux_states"], [0x50, 0x00, 0x00, 0x00])
        self.assertIn("0x00", payload["reserved_addresses_skipped"])
        self.assertIn("0x7F", payload["reserved_addresses_skipped"])

    def test_internal_pullups_warn_above_100khz(self):
        plan = _bus().plan_i2c(
            sda=2,
            scl=3,
            io_voltage=3.3,
            supply_voltage=3.3,
            pullups="internal",
            frequency_hz=400_000,
        )

        self.assertTrue(any("internal I2C pull-ups are weak" in item for item in plan.warnings))
        self.assertTrue(any("above 100 kHz" in item for item in plan.warnings))

    def test_duplicate_i2c_pins_are_rejected(self):
        with self.assertRaises(BusPlanError):
            _bus().plan_i2c(sda=2, scl=2, io_voltage=3.3, supply_voltage=3.3)


class TestSPIBusPlanning(unittest.TestCase):
    def test_spi_plan_resolves_multi_io_block_muxes(self):
        plan = _bus().plan_spi(
            sck=3,
            mosi=2,
            miso=5,
            cs=6,
            io_voltage=3.3,
            supply_voltage=3.3,
        )

        self.assertEqual(plan.kind, "spi")
        self.assertEqual(plan.esp_gpios, {"sck": 4, "mosi": 2, "miso": 6, "cs": 7})
        self.assertEqual(plan.mux_states, (0x50, 0x50, 0x00, 0x00))
        self.assertEqual(plan.supplies, ("VADJ1",))
        self.assertEqual(plan.efuses, ("EFUSE1", "EFUSE2"))
        self.assertEqual(plan.pullups, "off")

    def test_setup_spi_applies_route_before_firmware_setup(self):
        client = MagicMock()
        bus = BugBusterBusManager(client)

        plan = bus.setup_spi(
            sck=3,
            mosi=2,
            miso=5,
            cs=6,
            io_voltage=3.3,
            supply_voltage=3.3,
            frequency_hz=1_000_000,
            mode=0,
        )

        client.mux_set_all.assert_called_with([0x50, 0x50, 0x00, 0x00])
        client.ext_spi_setup.assert_called_with(
            sck_gpio=4,
            mosi_gpio=2,
            miso_gpio=6,
            cs_gpio=7,
            frequency_hz=1_000_000,
            mode=0,
        )
        self.assertEqual(plan.esp_gpios, {"sck": 4, "mosi": 2, "miso": 6, "cs": 7})

    def test_spi_jedec_id_parses_transfer_response(self):
        client = MagicMock()
        client.ext_spi_transfer.return_value = bytes([0x00, 0xEF, 0x40, 0x18])

        result = BugBusterBusManager(client).spi_jedec_id()

        self.assertEqual(result["jedec_id"], "EF4018")
        self.assertEqual(result["manufacturer_id"], 0xEF)

    def test_deferred_spi_transfer_queues_job(self):
        client = MagicMock()
        client.ext_job_submit_spi_transfer.return_value = 42

        job_id = BugBusterBusManager(client).defer_spi_transfer([0x9F, 0, 0, 0], timeout_ms=25)

        self.assertEqual(job_id, 42)
        client.ext_job_submit_spi_transfer.assert_called_once_with([0x9F, 0, 0, 0], timeout_ms=25)

    def test_deferred_result_normalizes_bytes(self):
        client = MagicMock()
        client.ext_job_get.return_value = {
            "job_id": 42,
            "status": 3,
            "status_name": "done",
            "kind": 3,
            "kind_name": "spi_transfer",
            "data": bytes([0x00, 0xEF, 0x40, 0x18]),
        }

        result = BugBusterBusManager(client).deferred_result(42)

        self.assertEqual(result["data"], [0x00, 0xEF, 0x40, 0x18])
        self.assertEqual(result["data_hex"], "00EF4018")

    def test_spi_requires_data_pin(self):
        with self.assertRaises(BusPlanError):
            _bus().plan_spi(sck=3, io_voltage=3.3, supply_voltage=3.3)

    def test_split_supply_routes_are_rejected_by_default(self):
        with self.assertRaises(BusPlanError):
            _bus().plan_spi(
                sck=2,
                mosi=8,
                io_voltage=3.3,
                supply_voltage=3.3,
            )

    def test_split_supply_routes_can_be_explicitly_allowed(self):
        plan = _bus().plan_spi(
            sck=2,
            mosi=8,
            io_voltage=3.3,
            supply_voltage=3.3,
            allow_split_supplies=True,
        )

        self.assertEqual(plan.supplies, ("VADJ1", "VADJ2"))
        self.assertEqual(plan.efuses, ("EFUSE1", "EFUSE4"))


class TestBugBusterBusFacade(unittest.TestCase):
    def test_bug_buster_exposes_lazy_bus_planner(self):
        bb = BugBuster(DummyTransport())

        plan = bb.bus_plan(
            "i2c",
            sda=2,
            scl=3,
            io_voltage=3.3,
            supply_voltage=3.3,
        )

        self.assertEqual(plan["kind"], "i2c")
        self.assertEqual(plan["esp_gpios"], {"sda": 2, "scl": 4})

    def test_unknown_bus_kind_is_rejected(self):
        bb = BugBuster(DummyTransport())

        with self.assertRaises(ValueError):
            bb.bus_plan("uart")

    def test_http_ext_bus_methods_use_bus_endpoints(self):
        transport = DummyTransport()
        bb = BugBuster(transport)

        self.assertEqual(bb.ext_i2c_setup(sda_gpio=2, scl_gpio=4)["sdaGpio"], 2)
        self.assertEqual(bb.ext_i2c_scan(), [0x50])
        self.assertEqual(bb.ext_i2c_write(0x50, [1, 2, 3]), 3)
        self.assertEqual(bb.ext_i2c_read(0x50, 3), bytes([1, 2, 3]))
        self.assertEqual(bb.ext_i2c_write_read(0x50, [0], 2), bytes([4, 5]))
        self.assertEqual(bb.ext_spi_setup(sck_gpio=4)["sckGpio"], 4)
        self.assertEqual(bb.ext_spi_transfer([1, 2, 3]), bytes([3, 2, 1]))
        self.assertEqual(bb.bus.status()["sessions"][0]["kind"], "i2c")


class TestI2CBusExecution(unittest.TestCase):
    def test_setup_i2c_applies_route_before_firmware_setup(self):
        client = MagicMock()
        bus = BugBusterBusManager(client)

        plan = bus.setup_i2c(
            sda=2,
            scl=3,
            io_voltage=3.3,
            supply_voltage=3.3,
            frequency_hz=400_000,
        )

        client.power_set.assert_any_call(PowerControl.MUX, on=True)
        client.set_level_shifter_oe.assert_called_with(on=True)
        client.idac_set_voltage.assert_any_call(0, 3.3)
        client.power_set.assert_any_call(PowerControl.VADJ1, on=True)
        client.power_set.assert_any_call(PowerControl.EFUSE1, on=True)
        client.mux_set_all.assert_called_with([0x50, 0x00, 0x00, 0x00])
        client.ext_i2c_setup.assert_called_with(
            sda_gpio=2,
            scl_gpio=4,
            frequency_hz=400_000,
            pullups="external",
        )
        self.assertEqual(plan.esp_gpios, {"sda": 2, "scl": 4})

    def test_i2c_scan_can_setup_and_scan(self):
        client = MagicMock()
        client.ext_i2c_scan.return_value = [0x50, 0x68]
        bus = BugBusterBusManager(client)

        result = bus.i2c_scan(sda=2, scl=3, io_voltage=3.3, supply_voltage=3.3)

        self.assertEqual(result["addresses"], ["0x50", "0x68"])
        self.assertEqual(result["address_values"], [0x50, 0x68])
        self.assertIsNotNone(result["plan"])

    def test_i2c_scan_requires_complete_route_when_setting_up(self):
        with self.assertRaises(BusPlanError):
            BugBusterBusManager(MagicMock()).i2c_scan(sda=2)


if __name__ == "__main__":
    unittest.main()
