"""DAQ-07 (MCP half) and MCP-34.

DAQ-07 A: daq_set_current_range accepted "high" for the 50 mohm range while
every report calls the 51 ohm range "hi", and "coarse"/"fine" collide with the
sample-source names. B: only ua|ma|a and the report names hi|mid|lo.

MCP-34 A: the store kept the last 8 captures of up to 4 M samples each
(~256 MB each at the module's own 64 B/sample budget, ~2 GB total).
B: eviction is by an estimated byte budget, always keeping the newest capture.
"""
from __future__ import annotations

from types import SimpleNamespace
from unittest.mock import patch

import pytest

from bugbuster.daq_config import DaqKey
from bugbuster_mcp import session
from bugbuster_mcp.tools import daq_power
from tests.unit._mock_client import make_client_mock


class _DummyMCP:
    def __init__(self):
        self.tools = {}

    def tool(self):
        def decorator(func):
            self.tools[func.__name__] = func
            return func
        return decorator


@pytest.fixture
def set_range():
    session.configure(transport="usb", port="/dev/null", vlogic=3.3)
    mcp = _DummyMCP()
    daq_power.register(mcp)
    bb = make_client_mock()
    with patch("bugbuster_mcp.session.get_client", return_value=bb), \
         patch.object(daq_power, "require_hat", lambda _bb: None):
        yield mcp.tools["daq_set_current_range"], bb


def _range_written(bb):
    return [c.args[1] for c in bb.daq.set.call_args_list if c.args[0] == DaqKey.RANGE_IDX]


@pytest.mark.parametrize("name, idx", [("ua", 2), ("ma", 1), ("a", 0), ("mid", 1)])
def test_unit_names_still_work(set_range, name, idx):
    tool, bb = set_range
    tool(range_name=name)
    assert _range_written(bb) == [idx]


@pytest.mark.parametrize("name", ["high", "low", "coarse", "fine"])
def test_ambiguous_aliases_are_rejected(set_range, name):
    tool, bb = set_range
    with pytest.raises(ValueError):
        tool(range_name=name)
    assert _range_written(bb) == []


@pytest.mark.parametrize("name, idx", [("hi", 2), ("lo", 0)])
def test_report_names_match_the_reports(set_range, name, idx):
    tool, bb = set_range
    tool(range_name=name)
    assert _range_written(bb) == [idx]


def _fake_capture(samples: int):
    return SimpleNamespace(current=range(samples), voltage=range(samples))


def test_store_stays_within_the_byte_budget():
    with patch.dict(daq_power._captures, clear=True):
        ids = [daq_power._store_capture(_fake_capture(daq_power.MAX_CAPTURE_SAMPLES)) for _ in range(8)]
        stored = sum(daq_power._capture_bytes(c) for c in daq_power._captures.values())
        assert stored <= daq_power.MAX_STORED_BYTES
        assert ids[-1] in daq_power._captures


def test_a_single_oversize_capture_is_still_kept():
    with patch.dict(daq_power._captures, clear=True):
        cid = daq_power._store_capture(_fake_capture(daq_power.MAX_STORED_BYTES))
        assert list(daq_power._captures) == [cid]
