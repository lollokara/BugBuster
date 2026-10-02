"""MCP-4: bounded observation tools that return a summary instead of one
sample: observe_adc (an analog IO over N seconds) and observe_daq (the DAQ
HAT meter over N seconds). A: neither existed."""

import itertools
import unittest
from unittest.mock import MagicMock, patch


from bugbuster_mcp import session
from bugbuster_mcp.tools import analog, daq


class DummyMCP:
    def __init__(self):
        self.tools = {}

    def tool(self):
        def decorator(func):
            self.tools[func.__name__] = func
            return func
        return decorator


class _Clock:
    def __init__(self):
        self.t = 0.0

    def monotonic(self):
        return self.t

    def sleep(self, s):
        self.t += s


class TestObserve(unittest.TestCase):
    def setUp(self):
        session.configure(transport="usb", port="/dev/null", vlogic=3.3)
        self.mcp = DummyMCP()
        analog.register(self.mcp)
        daq.register(self.mcp)
        self.clock = _Clock()
        for mod in (analog, daq):
            for name in ("monotonic", "sleep"):
                p = patch(f"{mod.__name__}.time.{name}", getattr(self.clock, name), create=True)
                p.start()
                self.addCleanup(p.stop)

    def test_observe_adc_summarises_voltage(self):
        hal = MagicMock()
        vals = itertools.cycle([1.0, 2.0, 3.0])
        hal.read_voltage.side_effect = lambda io: next(vals)
        with patch("bugbuster_mcp.session.get_hal", return_value=hal), \
             patch("bugbuster_mcp.tools.analog.require_io_mode"):
            r = self.mcp.tools["observe_adc"](io=3, seconds=1.0, rate_hz=3, quantity="voltage")
        self.assertEqual(r["unit"], "V")
        self.assertEqual((r["min"], r["max"]), (1.0, 3.0))
        self.assertAlmostEqual(r["mean"], 2.0)
        self.assertGreaterEqual(r["count"], 3)

    def test_observe_daq_summarises_each_field(self):
        bb = MagicMock()
        seq = itertools.cycle([{"current_a": 0.1, "voltage_v": 5.0}, {"current_a": 0.3, "voltage_v": 5.0}])
        bb.daq.measure.side_effect = lambda: next(seq)
        with patch("bugbuster_mcp.session.get_client", return_value=bb), \
             patch("bugbuster_mcp.tools.daq.require_hat"):
            r = self.mcp.tools["observe_daq"](seconds=1.0, rate_hz=4)
        self.assertAlmostEqual(r["fields"]["current_a"]["mean"], 0.2)
        self.assertEqual(r["fields"]["voltage_v"]["max"], 5.0)

    def test_bounds(self):
        with self.assertRaises(ValueError):
            self.mcp.tools["observe_daq"](seconds=120)
