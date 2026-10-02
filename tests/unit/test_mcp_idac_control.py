"""AN-15: MCP idac_control must apply the same limits as set_supply_voltage.

A: set_voltage skipped validate_vadj_voltage (board-profile locks, 15 V max,
12 V confirm gate) and could move VLOGIC (channel 0), which set_supply_voltage
refuses; set_code accepted only 0..127 although the DS4424 code is signed, and
wrote channel 0 and locked rails freely."""
from __future__ import annotations

from unittest.mock import patch

import pytest

from bugbuster_mcp import session
from bugbuster_mcp.tools import advanced
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
def env():
    session.configure(transport="usb", port="/dev/null", vlogic=3.3)
    mcp = _DummyMCP()
    advanced.register(mcp)
    bb = make_client_mock()
    with patch("bugbuster_mcp.session.get_client", return_value=bb), \
         patch("bugbuster_mcp.session.get_active_board_profile", return_value=None) as prof:
        yield mcp.tools["idac_control"], bb, prof


def test_status_still_reads(env):
    idac, bb, _ = env
    bb.idac_get_status.return_value = {"present": True}
    assert idac("status")["idac"] == {"present": True}


def test_in_range_vadj_voltage_is_written(env):
    idac, bb, _ = env
    idac("set_voltage", channel=1, voltage=5.0, i_understand_the_risk=True)
    bb.idac_set_voltage.assert_called_once_with(1, 5.0)


@pytest.mark.parametrize("channel", [0, 3])
def test_set_voltage_refuses_vlogic_and_unconnected(env, channel):
    idac, bb, _ = env
    with pytest.raises(ValueError):
        idac("set_voltage", channel=channel, voltage=3.3, i_understand_the_risk=True)
    bb.idac_set_voltage.assert_not_called()


@pytest.mark.parametrize("voltage", [16.0, 2.0, 13.0])
def test_set_voltage_applies_vadj_limits(env, voltage):
    idac, bb, _ = env
    with pytest.raises(ValueError):
        idac("set_voltage", channel=2, voltage=voltage, i_understand_the_risk=True)
    bb.idac_set_voltage.assert_not_called()


def test_set_voltage_respects_a_locked_profile(env):
    idac, bb, prof = env
    prof.return_value = {"name": "dut", "vadj1": {"value": 3.3, "locked": True}}
    with pytest.raises(ValueError, match="locked"):
        idac("set_voltage", channel=1, voltage=5.0, i_understand_the_risk=True)
    bb.idac_set_voltage.assert_not_called()


def test_set_code_accepts_signed_sink_codes(env):
    idac, bb, _ = env
    idac("set_code", channel=1, code=-20, i_understand_the_risk=True)
    bb.idac_set_code.assert_called_once_with(1, -20)


def test_set_code_refuses_vlogic_and_locked_rails(env):
    idac, bb, prof = env
    with pytest.raises(ValueError):
        idac("set_code", channel=0, code=5, i_understand_the_risk=True)
    prof.return_value = {"name": "dut", "vadj2": {"value": 3.3, "locked": True}}
    with pytest.raises(ValueError, match="locked"):
        idac("set_code", channel=2, code=5, i_understand_the_risk=True)
    bb.idac_set_code.assert_not_called()


@pytest.mark.parametrize("code", [-128, 128])
def test_set_code_range_is_bounded(env, code):
    idac, bb, _ = env
    with pytest.raises(ValueError):
        idac("set_code", channel=1, code=code, i_understand_the_risk=True)
    bb.idac_set_code.assert_not_called()
