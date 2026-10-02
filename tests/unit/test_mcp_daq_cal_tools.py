"""Coverage for bugbuster_mcp.tools.daq_cal: SMU calibration start/ack/abort/status.

MCP-25: daq_cal_start overwrites the calibration table in NVM and needs
confirm=True.
"""
from __future__ import annotations

from types import SimpleNamespace
from unittest.mock import create_autospec, patch

import pytest

from bugbuster.daq_config import (
    DaqCalMode, DaqCalPersist, DaqCalPhase, DaqCalPrompt, DaqConfig,
)
from bugbuster_mcp.tools import daq_cal
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
    mcp = _DummyMCP()
    daq_cal.register(mcp)
    bb = make_client_mock()
    bb.daq = create_autospec(DaqConfig, instance=True)
    with patch("bugbuster_mcp.session.get_client", return_value=bb) as gc:
        yield SimpleNamespace(t=mcp.tools, bb=bb, gc=gc)


def test_registers_the_four_cal_tools(env):
    assert set(env.t) == {"daq_cal_start", "daq_cal_ack", "daq_cal_abort",
                          "daq_cal_status"}


# ---------------------------------------------------------------------------
# daq_cal_start
# ---------------------------------------------------------------------------
@pytest.mark.parametrize("mode, canonical, expected", [
    ("voltage", "voltage", DaqCalMode.VOLTAGE),
    ("v", "v", DaqCalMode.VOLTAGE),
    ("current", "current", DaqCalMode.CURRENT),
    ("i", "i", DaqCalMode.CURRENT),
    ("  VOLTAGE ", "voltage", DaqCalMode.VOLTAGE),
    ("Current", "current", DaqCalMode.CURRENT),
])
def test_start_sends_mode_to_the_hat(env, mode, canonical, expected):
    out = env.t["daq_cal_start"](mode=mode, confirm=True)
    env.bb.daq.cal_start.assert_called_once_with(expected)
    assert out["success"] is True
    assert out["mode"] == canonical
    assert "daq_cal_status" in out["message"] and "daq_cal_ack" in out["message"]
    assert set(out) == {"success", "mode", "message"}


@pytest.mark.parametrize("mode", ["voltage", "current"])
def test_start_without_confirm_refuses_and_sends_nothing(env, mode):
    with pytest.raises(ValueError, match=r"daq_cal_start requires confirm=True"):
        env.t["daq_cal_start"](mode=mode)
    env.gc.assert_not_called()
    env.bb.daq.cal_start.assert_not_called()


def test_start_with_confirm_false_explicit_refuses(env):
    with pytest.raises(ValueError, match="confirm=True"):
        env.t["daq_cal_start"](mode="voltage", confirm=False)
    env.bb.daq.cal_start.assert_not_called()


def test_start_checks_confirm_before_mode(env):
    with pytest.raises(ValueError, match="confirm=True"):
        env.t["daq_cal_start"](mode="nonsense")
    env.gc.assert_not_called()


@pytest.mark.parametrize("mode", ["", "volts", "amps", "0", "both"])
def test_start_rejects_unknown_mode_even_when_confirmed(env, mode):
    with pytest.raises(ValueError, match="mode must be 'voltage' or 'current'"):
        env.t["daq_cal_start"](mode=mode, confirm=True)
    env.gc.assert_not_called()
    env.bb.daq.cal_start.assert_not_called()


def test_start_propagates_device_errors(env):
    env.bb.daq.cal_start.side_effect = RuntimeError("P4 busy")
    with pytest.raises(RuntimeError, match="P4 busy"):
        env.t["daq_cal_start"](mode="voltage", confirm=True)


# ---------------------------------------------------------------------------
# daq_cal_ack / daq_cal_abort
# ---------------------------------------------------------------------------
def test_ack_sends_ack_only(env):
    out = env.t["daq_cal_ack"]()
    assert out == {"success": True, "message": "Prompt acknowledged."}
    env.bb.daq.cal_ack.assert_called_once_with()
    env.bb.daq.cal_start.assert_not_called()
    env.bb.daq.cal_abort.assert_not_called()


def test_abort_sends_abort_only(env):
    out = env.t["daq_cal_abort"]()
    assert out == {"success": True, "message": "Calibration aborted."}
    env.bb.daq.cal_abort.assert_called_once_with()
    env.bb.daq.cal_ack.assert_not_called()


def test_ack_and_abort_propagate_device_errors(env):
    env.bb.daq.cal_ack.side_effect = RuntimeError("no prompt pending")
    env.bb.daq.cal_abort.side_effect = RuntimeError("link down")
    with pytest.raises(RuntimeError, match="no prompt pending"):
        env.t["daq_cal_ack"]()
    with pytest.raises(RuntimeError, match="link down"):
        env.t["daq_cal_abort"]()


# ---------------------------------------------------------------------------
# daq_cal_status
# ---------------------------------------------------------------------------
def _status(**over):
    st = {
        "phase": DaqCalPhase.PROMPT, "prompt": DaqCalPrompt.DISCONNECT_LOAD,
        "mode": DaqCalMode.VOLTAGE, "progress": 0, "point": 0, "code": 0,
        "persist": DaqCalPersist.RAM, "measured": 0.0, "min": 0.0, "max": 0.0,
        "flags": 0, "vcount": 0, "icount": 0,
    }
    st.update(over)
    return st


def test_status_renders_enums_as_lowercase_names(env):
    env.bb.daq.cal_status.return_value = _status()
    out = env.t["daq_cal_status"]()
    env.bb.daq.cal_status.assert_called_once_with()
    assert out["phase"] == "prompt"
    assert out["prompt"] == "disconnect_load"
    assert out["mode"] == "voltage"
    assert out["persist"] == "ram"


def test_status_passes_numeric_fields_through(env):
    env.bb.daq.cal_status.return_value = _status(
        phase=DaqCalPhase.SUCCESS, prompt=DaqCalPrompt.NONE,
        mode=DaqCalMode.CURRENT, persist=DaqCalPersist.SAVED, progress=100,
        point=17, code=-3, measured=2.4, min=0.1, max=2.5, flags=0x0008,
        vcount=12, icount=9)
    out = env.t["daq_cal_status"]()
    assert out["phase"] == "success" and out["prompt"] == "none"
    assert out["mode"] == "current" and out["persist"] == "saved"
    assert (out["progress"], out["point"], out["code"]) == (100, 17, -3)
    assert (out["measured"], out["min"], out["max"]) == (2.4, 0.1, 2.5)
    assert (out["flags"], out["vcount"], out["icount"]) == (8, 12, 9)
    assert set(out) == set(_status())


def test_status_leaves_unknown_raw_values_untouched(env):
    env.bb.daq.cal_status.return_value = _status(phase=99, prompt=7, mode=5,
                                                 persist=9)
    out = env.t["daq_cal_status"]()
    assert (out["phase"], out["prompt"], out["mode"], out["persist"]) == (99, 7, 5, 9)


def test_status_does_not_mutate_the_clients_dict(env):
    raw = _status()
    env.bb.daq.cal_status.return_value = raw
    env.t["daq_cal_status"]()
    assert raw["phase"] is DaqCalPhase.PROMPT


@pytest.mark.parametrize("phase, name", [
    (DaqCalPhase.IDLE, "idle"), (DaqCalPhase.RUNNING, "running"),
    (DaqCalPhase.FAILED, "failed"),
])
def test_status_phase_names(env, phase, name):
    env.bb.daq.cal_status.return_value = _status(phase=phase)
    assert env.t["daq_cal_status"]()["phase"] == name
