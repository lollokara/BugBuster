"""
Unit tests for ScriptSession and bb.script_session() (V2-H).

Runs without hardware via SimulatedDevice + SimulatedUSBTransport.

Note: the simulated device does not execute MicroPython; it logs the submitted
source text.  Tests therefore assert on the log *format* produced by the
simulator (e.g. "[eval:N] <src>") rather than on Python execution output.
"""

import pytest
import bugbuster as bb
from bugbuster.script import ScriptSession
from bugbuster.client import ScriptStatusResult
from tests.mock import SimulatedDevice, SimulatedUSBTransport


# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

def _usb_client():
    device = SimulatedDevice()
    transport = SimulatedUSBTransport(device)
    client = bb.BugBuster(transport)
    client.connect()
    return client, device


# ---------------------------------------------------------------------------
# ScriptSession basic interface
# ---------------------------------------------------------------------------

class TestScriptSessionContextManager:
    def test_enter_warms_vm(self):
        """__enter__ sends an empty eval with persist=True, switching mode."""
        client, device = _usb_client()
        with ScriptSession(client=client) as s:
            assert device.script_mode == 1  # persistent after warm-up
        client.disconnect()

    def test_exit_resets_vm(self):
        """__exit__ calls script_reset(), returning mode to EPHEMERAL."""
        client, device = _usb_client()
        with ScriptSession(client=client):
            pass
        # After context exit the mode is reset to 0
        assert device.script_mode == 0
        client.disconnect()

    def test_factory_returns_script_session(self):
        """bb.script_session() returns a ScriptSession instance."""
        client, device = _usb_client()
        session = client.script_session()
        assert isinstance(session, ScriptSession)
        client.disconnect()


# ---------------------------------------------------------------------------
# eval() returns logs
# ---------------------------------------------------------------------------

class TestScriptSessionEval:
    def test_eval_returns_string(self):
        """eval() returns a str."""
        client, device = _usb_client()
        with client.script_session() as s:
            result = s.eval("x = 5")
        assert isinstance(result, str)
        client.disconnect()

    def test_eval_logs_contain_source(self):
        """Simulator logs contain the submitted source snippet."""
        client, device = _usb_client()
        with client.script_session() as s:
            logs = s.eval("x = 5")
        # Simulator appends "[eval:N] <src[:40]>" to the ring
        assert "x = 5" in logs
        client.disconnect()

    def test_eval_no_drain(self):
        """eval(log_drain=False) returns empty string."""
        client, device = _usb_client()
        with client.script_session() as s:
            result = s.eval("x = 5", log_drain=False)
        assert result == ""
        client.disconnect()

    def test_eval_sequential_state(self):
        """Two evals in the same session accumulate script_id."""
        client, device = _usb_client()
        with client.script_session() as s:
            s.eval("x = 5")
            s.eval("y = x + 1")
        # script_id was incremented: 1 (warm), 2 (x=5), 3 (y=x+1)
        assert device.script_id >= 3
        client.disconnect()


# ---------------------------------------------------------------------------
# reset() clears state
# ---------------------------------------------------------------------------

class TestScriptSessionReset:
    def test_reset_clears_mode(self):
        """s.reset() sends script_reset() then re-warms (mode=1 after)."""
        client, device = _usb_client()
        with client.script_session() as s:
            assert device.script_mode == 1
            s.reset()
            # After reset+rewarm, mode is persistent again
            assert device.script_mode == 1
        client.disconnect()

    def test_reset_increments_auto_reset_count(self):
        """s.reset() increments device auto_reset_count."""
        client, device = _usb_client()
        with client.script_session() as s:
            s.reset()
        assert device.script_auto_reset_count >= 1
        client.disconnect()


# ---------------------------------------------------------------------------
# status() passthrough
# ---------------------------------------------------------------------------

class TestScriptSessionStatus:
    def test_status_returns_script_status_result(self):
        """s.status() returns a ScriptStatusResult."""
        client, device = _usb_client()
        with client.script_session() as s:
            st = s.status()
        assert isinstance(st, ScriptStatusResult)
        client.disconnect()

    def test_status_fields_present(self):
        """All V2-A fields are accessible on the returned status."""
        client, device = _usb_client()
        with client.script_session() as s:
            st = s.status()
        # These should be accessible without AttributeError
        _ = st.mode
        _ = st.globals_bytes_est
        _ = st.globals_count
        _ = st.auto_reset_count
        _ = st.last_eval_at_ms
        _ = st.idle_for_ms
        _ = st.watermark_soft_hit
        client.disconnect()
