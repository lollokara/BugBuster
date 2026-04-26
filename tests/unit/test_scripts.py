"""
Unit tests for BBP on-device scripting commands (Phase 5).

Runs without hardware against SimulatedDevice via SimulatedUSBTransport.
Covers: SCRIPT_EVAL, SCRIPT_STATUS, SCRIPT_LOGS, SCRIPT_STOP wire format
and client-level method behaviour.
"""

import struct
import pytest

import bugbuster as bb
from tests.mock import SimulatedDevice, SimulatedUSBTransport
from bugbuster.constants import CmdId
from bugbuster.client import ScriptStatusResult


# ---------------------------------------------------------------------------
# Fixture
# ---------------------------------------------------------------------------

def _usb_client():
    device = SimulatedDevice()
    transport = SimulatedUSBTransport(device)
    client = bb.BugBuster(transport)
    client.connect()
    return client, device


# ---------------------------------------------------------------------------
# CmdId constants
# ---------------------------------------------------------------------------

class TestScriptCmdIds:
    def test_opcode_values(self):
        assert CmdId.SCRIPT_EVAL   == 0xF5
        assert CmdId.SCRIPT_STATUS == 0xF6
        assert CmdId.SCRIPT_LOGS   == 0xF7
        assert CmdId.SCRIPT_STOP   == 0xF8


# ---------------------------------------------------------------------------
# SCRIPT_EVAL wire format
# ---------------------------------------------------------------------------

class TestScriptEvalWire:
    def test_eval_response_structure(self):
        """SCRIPT_EVAL returns u8 enqueued + u32 script_id (5 bytes)."""
        device = SimulatedDevice()
        src = b"print('hello')"
        payload = struct.pack('<H', len(src)) + src
        resp = device.dispatch(int(CmdId.SCRIPT_EVAL), payload)
        assert len(resp) == 5
        enqueued = resp[0]
        script_id, = struct.unpack_from('<I', resp, 1)
        assert enqueued == 1
        assert script_id == 1

    def test_eval_increments_script_id(self):
        device = SimulatedDevice()
        for expected_id in (1, 2, 3):
            src = b"x = 1"
            payload = struct.pack('<H', len(src)) + src
            resp = device.dispatch(int(CmdId.SCRIPT_EVAL), payload)
            script_id, = struct.unpack_from('<I', resp, 1)
            assert script_id == expected_id

    def test_eval_marks_running(self):
        device = SimulatedDevice()
        src = b"pass"
        payload = struct.pack('<H', len(src)) + src
        device.dispatch(int(CmdId.SCRIPT_EVAL), payload)
        assert device.script_running is True

    def test_eval_empty_payload_returns_not_enqueued(self):
        device = SimulatedDevice()
        resp = device.dispatch(int(CmdId.SCRIPT_EVAL), b'')
        assert resp[0] == 0


# ---------------------------------------------------------------------------
# SCRIPT_STATUS wire format
# ---------------------------------------------------------------------------

class TestScriptStatusWire:
    def test_status_initial_state(self):
        """Fresh device: not running, id=0, runs=0, errors=0, no error msg."""
        device = SimulatedDevice()
        resp = device.dispatch(int(CmdId.SCRIPT_STATUS), b'')
        is_running = bool(resp[0])
        script_id, = struct.unpack_from('<I', resp, 1)
        total_runs, = struct.unpack_from('<I', resp, 5)
        total_errors, = struct.unpack_from('<I', resp, 9)
        err_len = resp[13]
        assert is_running is False
        assert script_id == 0
        assert total_runs == 0
        assert total_errors == 0
        assert err_len == 0

    def test_status_after_eval(self):
        device = SimulatedDevice()
        src = b"x = 42"
        payload = struct.pack('<H', len(src)) + src
        device.dispatch(int(CmdId.SCRIPT_EVAL), payload)
        resp = device.dispatch(int(CmdId.SCRIPT_STATUS), b'')
        is_running = bool(resp[0])
        total_runs, = struct.unpack_from('<I', resp, 5)
        assert is_running is True
        assert total_runs == 1


# ---------------------------------------------------------------------------
# SCRIPT_LOGS wire format
# ---------------------------------------------------------------------------

class TestScriptLogsWire:
    def test_logs_empty_on_fresh_device(self):
        device = SimulatedDevice()
        resp = device.dispatch(int(CmdId.SCRIPT_LOGS), b'')
        count, = struct.unpack_from('<H', resp, 0)
        assert count == 0
        assert len(resp) == 2

    def test_logs_populated_after_eval(self):
        device = SimulatedDevice()
        src = b"print('test')"
        payload = struct.pack('<H', len(src)) + src
        device.dispatch(int(CmdId.SCRIPT_EVAL), payload)
        resp = device.dispatch(int(CmdId.SCRIPT_LOGS), b'')
        count, = struct.unpack_from('<H', resp, 0)
        assert count > 0
        log_text = resp[2:2 + count].decode("utf-8")
        assert "eval:1" in log_text

    def test_logs_drained_after_read(self):
        device = SimulatedDevice()
        src = b"pass"
        payload = struct.pack('<H', len(src)) + src
        device.dispatch(int(CmdId.SCRIPT_EVAL), payload)
        # First drain
        device.dispatch(int(CmdId.SCRIPT_LOGS), b'')
        # Second drain should be empty
        resp = device.dispatch(int(CmdId.SCRIPT_LOGS), b'')
        count, = struct.unpack_from('<H', resp, 0)
        assert count == 0


# ---------------------------------------------------------------------------
# SCRIPT_STOP wire format
# ---------------------------------------------------------------------------

class TestScriptStopWire:
    def test_stop_returns_empty(self):
        device = SimulatedDevice()
        resp = device.dispatch(int(CmdId.SCRIPT_STOP), b'')
        assert resp == b''

    def test_stop_clears_running_flag(self):
        device = SimulatedDevice()
        # Simulate a running script
        device.script_running = True
        device.dispatch(int(CmdId.SCRIPT_STOP), b'')
        assert device.script_running is False


# ---------------------------------------------------------------------------
# Client method round-trips (through SimulatedUSBTransport)
# ---------------------------------------------------------------------------

class TestScriptClientMethods:
    def test_script_eval_returns_status_result(self):
        client, device = _usb_client()
        result = client.script_eval("print('hello')")
        assert isinstance(result, ScriptStatusResult)
        assert result.is_running is True
        assert result.script_id == 1
        client.disconnect()

    def test_script_status_reflects_eval(self):
        client, device = _usb_client()
        client.script_eval("x = 1")
        status = client.script_status()
        assert isinstance(status, ScriptStatusResult)
        assert status.total_runs == 1
        assert status.script_id == 1
        client.disconnect()

    def test_script_logs_returns_string(self):
        client, device = _usb_client()
        client.script_eval("pass")
        logs = client.script_logs()
        assert isinstance(logs, str)
        assert len(logs) > 0
        client.disconnect()

    def test_script_logs_drain(self):
        client, device = _usb_client()
        client.script_eval("pass")
        client.script_logs()         # drain
        logs2 = client.script_logs() # should be empty
        assert logs2 == ""
        client.disconnect()

    def test_script_stop_no_error(self):
        client, device = _usb_client()
        client.script_eval("pass")
        client.script_stop()
        assert device.script_running is False
        client.disconnect()

    def test_script_eval_too_large_raises(self):
        client, _ = _usb_client()
        with pytest.raises(ValueError, match="too large"):
            client.script_eval("x" * (32 * 1024 + 1))
        client.disconnect()

    def test_script_eval_multiple_runs_increment_total(self):
        client, device = _usb_client()
        client.script_eval("a = 1")
        client.script_eval("b = 2")
        status = client.script_status()
        assert status.total_runs == 2
        assert status.script_id == 2
        client.disconnect()

    def test_script_status_initial(self):
        client, _ = _usb_client()
        status = client.script_status()
        assert status.is_running is False
        assert status.script_id == 0
        assert status.total_runs == 0
        assert status.total_errors == 0
        assert status.last_error == ""
        client.disconnect()


# ---------------------------------------------------------------------------
# Phase 6a CmdId constants
# ---------------------------------------------------------------------------

class TestScriptStorageCmdIds:
    def test_opcode_values(self):
        assert CmdId.SCRIPT_UPLOAD   == 0xF9
        assert CmdId.SCRIPT_LIST     == 0xFA
        assert CmdId.SCRIPT_RUN_FILE == 0xFB
        assert CmdId.SCRIPT_DELETE   == 0xFC


# ---------------------------------------------------------------------------
# SCRIPT_UPLOAD wire format
# ---------------------------------------------------------------------------

class TestScriptUploadWire:
    def test_upload_returns_ok(self):
        device = SimulatedDevice()
        name = b"test.py"
        body = b"print('hello')"
        payload = bytes([len(name)]) + name + struct.pack('<H', len(body)) + body
        resp = device.dispatch(int(CmdId.SCRIPT_UPLOAD), payload)
        assert resp[0] == 1  # ok
        assert resp[1] == 0  # no error

    def test_upload_stores_file(self):
        device = SimulatedDevice()
        name = b"stored.py"
        body = b"x = 42"
        payload = bytes([len(name)]) + name + struct.pack('<H', len(body)) + body
        device.dispatch(int(CmdId.SCRIPT_UPLOAD), payload)
        assert "stored.py" in device.script_files
        assert device.script_files["stored.py"] == body

    def test_upload_empty_payload_returns_error(self):
        device = SimulatedDevice()
        resp = device.dispatch(int(CmdId.SCRIPT_UPLOAD), b'')
        assert resp[0] == 0  # not ok

    def test_upload_overwrites_existing(self):
        device = SimulatedDevice()
        name = b"over.py"
        for content in (b"v1 = 1", b"v2 = 2"):
            payload = bytes([len(name)]) + name + struct.pack('<H', len(content)) + content
            device.dispatch(int(CmdId.SCRIPT_UPLOAD), payload)
        assert device.script_files["over.py"] == b"v2 = 2"


# ---------------------------------------------------------------------------
# SCRIPT_LIST wire format
# ---------------------------------------------------------------------------

class TestScriptListWire:
    def test_list_empty_on_fresh_device(self):
        device = SimulatedDevice()
        resp = device.dispatch(int(CmdId.SCRIPT_LIST), b'')
        assert resp[0] == 0  # count = 0

    def test_list_shows_uploaded_file(self):
        device = SimulatedDevice()
        name = b"listed.py"
        body = b"pass"
        payload = bytes([len(name)]) + name + struct.pack('<H', len(body)) + body
        device.dispatch(int(CmdId.SCRIPT_UPLOAD), payload)
        resp = device.dispatch(int(CmdId.SCRIPT_LIST), b'')
        count = resp[0]
        assert count == 1
        name_len = resp[1]
        listed_name = resp[2:2 + name_len].decode("utf-8")
        assert listed_name == "listed.py"

    def test_list_multiple_files(self):
        device = SimulatedDevice()
        for n in (b"a.py", b"b.py", b"c.py"):
            payload = bytes([len(n)]) + n + struct.pack('<H', 4) + b"pass"
            device.dispatch(int(CmdId.SCRIPT_UPLOAD), payload)
        resp = device.dispatch(int(CmdId.SCRIPT_LIST), b'')
        assert resp[0] == 3


# ---------------------------------------------------------------------------
# SCRIPT_RUN_FILE wire format
# ---------------------------------------------------------------------------

class TestScriptRunFileWire:
    def test_run_file_returns_enqueued(self):
        device = SimulatedDevice()
        name = b"runner.py"
        payload = bytes([len(name)]) + name + struct.pack('<H', 4) + b"pass"
        device.dispatch(int(CmdId.SCRIPT_UPLOAD), payload)
        resp = device.dispatch(int(CmdId.SCRIPT_RUN_FILE), bytes([len(name)]) + name)
        assert resp[0] == 1  # enqueued
        script_id, = struct.unpack_from('<I', resp, 1)
        assert script_id == 1

    def test_run_file_not_found_returns_not_enqueued(self):
        device = SimulatedDevice()
        name = b"missing.py"
        resp = device.dispatch(int(CmdId.SCRIPT_RUN_FILE), bytes([len(name)]) + name)
        assert resp[0] == 0  # not enqueued

    def test_run_file_marks_running(self):
        device = SimulatedDevice()
        name = b"run2.py"
        payload = bytes([len(name)]) + name + struct.pack('<H', 4) + b"pass"
        device.dispatch(int(CmdId.SCRIPT_UPLOAD), payload)
        device.dispatch(int(CmdId.SCRIPT_RUN_FILE), bytes([len(name)]) + name)
        assert device.script_running is True


# ---------------------------------------------------------------------------
# SCRIPT_DELETE wire format
# ---------------------------------------------------------------------------

class TestScriptDeleteWire:
    def test_delete_existing_file(self):
        device = SimulatedDevice()
        name = b"del.py"
        payload = bytes([len(name)]) + name + struct.pack('<H', 4) + b"pass"
        device.dispatch(int(CmdId.SCRIPT_UPLOAD), payload)
        resp = device.dispatch(int(CmdId.SCRIPT_DELETE), bytes([len(name)]) + name)
        assert resp[0] == 1  # ok
        assert "del.py" not in device.script_files

    def test_delete_nonexistent_returns_error(self):
        device = SimulatedDevice()
        name = b"ghost.py"
        resp = device.dispatch(int(CmdId.SCRIPT_DELETE), bytes([len(name)]) + name)
        assert resp[0] == 0  # not ok
        err_len = resp[1]
        assert err_len > 0

    def test_delete_empty_payload_returns_error(self):
        device = SimulatedDevice()
        resp = device.dispatch(int(CmdId.SCRIPT_DELETE), b'')
        assert resp[0] == 0


# ---------------------------------------------------------------------------
# Client method round-trips for Phase 6a
# ---------------------------------------------------------------------------

class TestScriptStorageClientMethods:
    def test_script_upload_and_list(self):
        client, device = _usb_client()
        client.script_upload("hello.py", "print('hi')")
        names = client.script_list()
        assert "hello.py" in names
        client.disconnect()

    def test_script_list_empty(self):
        client, device = _usb_client()
        names = client.script_list()
        assert names == []
        client.disconnect()

    def test_script_upload_multiple_and_list(self):
        client, device = _usb_client()
        client.script_upload("a.py", "a = 1")
        client.script_upload("b.py", "b = 2")
        names = client.script_list()
        assert "a.py" in names
        assert "b.py" in names
        client.disconnect()

    def test_script_delete_removes_file(self):
        client, device = _usb_client()
        client.script_upload("gone.py", "pass")
        client.script_delete("gone.py")
        names = client.script_list()
        assert "gone.py" not in names
        client.disconnect()

    def test_script_delete_nonexistent_raises(self):
        client, device = _usb_client()
        with pytest.raises(RuntimeError):
            client.script_delete("nope.py")
        client.disconnect()

    def test_script_run_file_returns_status(self):
        client, device = _usb_client()
        client.script_upload("run.py", "x = 1")
        result = client.script_run_file("run.py")
        assert isinstance(result, ScriptStatusResult)
        assert result.is_running is True
        assert result.script_id == 1
        client.disconnect()

    def test_script_run_file_not_found_raises(self):
        client, device = _usb_client()
        with pytest.raises(RuntimeError):
            client.script_run_file("missing.py")
        client.disconnect()

    def test_script_upload_invalid_name_raises(self):
        client, _ = _usb_client()
        with pytest.raises(ValueError):
            client.script_upload("bad name.txt", "pass")
        client.disconnect()

    def test_script_upload_too_large_raises(self):
        client, _ = _usb_client()
        with pytest.raises(ValueError, match="too large"):
            client.script_upload("big.py", "x" * (32 * 1024 + 1))
        client.disconnect()


# ---------------------------------------------------------------------------
# Phase 6b — Autorun tests
# ---------------------------------------------------------------------------

class TestAutorun:
    """Tests for SCRIPT_AUTORUN (0xFD) sub-byte multiplex over BBP."""

    def test_status_defaults(self):
        """Fresh device: autorun disabled, no script, IO12 low."""
        client, device = _usb_client()
        st = client.script_autorun_status()
        assert st.enabled is False
        assert st.has_script is False
        assert st.io12_high is False
        assert st.last_run_ok is False
        assert st.last_run_id == 0
        client.disconnect()

    def test_enable_and_status(self):
        """Enable autorun with an uploaded script; status reflects enabled=True."""
        client, device = _usb_client()
        client.script_upload("hello.py", "print('hello')")
        client.script_autorun_enable("hello.py")
        st = client.script_autorun_status()
        assert st.enabled is True
        assert st.has_script is True
        client.disconnect()

    def test_disable_clears_enabled(self):
        """Disable autorun; status shows enabled=False."""
        client, device = _usb_client()
        client.script_upload("hello.py", "print('hello')")
        client.script_autorun_enable("hello.py")
        client.script_autorun_disable()
        st = client.script_autorun_status()
        assert st.enabled is False
        client.disconnect()

    def test_enable_missing_script_raises(self):
        """Enable with non-existent script name raises RuntimeError."""
        client, device = _usb_client()
        with pytest.raises(RuntimeError):
            client.script_autorun_enable("nosuchfile.py")
        client.disconnect()

    def test_io12_high_flag(self):
        """Simulator io12_high flag is reflected in status response."""
        client, device = _usb_client()
        device.autorun_io12_high = True
        st = client.script_autorun_status()
        assert st.io12_high is True
        client.disconnect()

    def test_run_now_success(self):
        """run_now with autorun script present returns a valid script_id."""
        client, device = _usb_client()
        client.script_upload("boot.py", "pass")
        client.script_autorun_enable("boot.py")
        script_id = client.script_autorun_run_now()
        assert script_id > 0
        client.disconnect()

    def test_run_now_no_script_raises(self):
        """run_now with no autorun script raises RuntimeError."""
        client, device = _usb_client()
        with pytest.raises(RuntimeError):
            client.script_autorun_run_now()
        client.disconnect()

    def test_disable_idempotent(self):
        """Calling disable when already disabled does not raise."""
        client, device = _usb_client()
        client.script_autorun_disable()  # was never enabled — must not raise
        client.disconnect()

    def test_cmd_id_value(self):
        """SCRIPT_AUTORUN opcode is 0xFD."""
        from bugbuster.constants import CmdId
        assert CmdId.SCRIPT_AUTORUN == 0xFD
