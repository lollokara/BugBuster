"""
Unit tests for V2-A persistent interpreter state (BBP script engine).

Covers:
  - persist=False (default) keeps EPHEMERAL mode
  - persist=True switches to PERSISTENT mode (sticky)
  - persist=False after PERSISTENT mode is a no-op (sticky)
  - BBP sub=4 RESET_VM wire round-trip
  - BBP sub=5 STATUS_PERSISTED wire round-trip
  - Client script_reset() USB path
  - USB flags byte symmetry (flags=0x00 / flags=0x01)
  - ScriptStatusResult has new fields with correct defaults
"""

import struct
import pytest

import bugbuster as bb
from tests.mock import SimulatedDevice, SimulatedUSBTransport
from bugbuster.constants import CmdId
from bugbuster.client import ScriptStatusResult


# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

def _usb_client():
    device = SimulatedDevice()
    transport = SimulatedUSBTransport(device)
    client = bb.BugBuster(transport)
    client.connect()
    return client, device


def _eval_payload(src: bytes, persist: bool = False) -> bytes:
    """Build SCRIPT_EVAL payload: u8 flags, u16 src_len, src."""
    flags = 0x01 if persist else 0x00
    return struct.pack('<BH', flags, len(src)) + src


# ---------------------------------------------------------------------------
# Gate 1: default (persist=False) keeps EPHEMERAL mode
# ---------------------------------------------------------------------------

class TestEphemeralDefault:
    def test_no_persist_flag_keeps_ephemeral(self):
        """Eval without persist flag leaves device in EPHEMERAL mode (0)."""
        device = SimulatedDevice()
        src = b"x = 1"
        device.dispatch(int(CmdId.SCRIPT_EVAL), _eval_payload(src, persist=False))
        assert device.script_mode == 0

    def test_flags_byte_zero_ephemeral(self):
        """Explicit flags=0x00 keeps EPHEMERAL mode."""
        device = SimulatedDevice()
        payload = struct.pack('<BH', 0x00, 4) + b"pass"
        device.dispatch(int(CmdId.SCRIPT_EVAL), payload)
        assert device.script_mode == 0


# ---------------------------------------------------------------------------
# Gate 2: persist=True switches to PERSISTENT mode
# ---------------------------------------------------------------------------

class TestPersistentMode:
    def test_persist_flag_switches_to_persistent(self):
        """Eval with persist=True switches device to PERSISTENT mode (1)."""
        device = SimulatedDevice()
        src = b"x = 1"
        device.dispatch(int(CmdId.SCRIPT_EVAL), _eval_payload(src, persist=True))
        assert device.script_mode == 1

    def test_flags_byte_01_persistent(self):
        """Explicit flags=0x01 switches to PERSISTENT mode."""
        device = SimulatedDevice()
        payload = struct.pack('<BH', 0x01, 4) + b"pass"
        device.dispatch(int(CmdId.SCRIPT_EVAL), payload)
        assert device.script_mode == 1


# ---------------------------------------------------------------------------
# Gate 3: persist=False is a no-op after PERSISTENT mode (sticky)
# ---------------------------------------------------------------------------

class TestPersistentSticky:
    def test_persist_false_noop_in_persistent_mode(self):
        """Once in PERSISTENT mode, persist=False does not reset to EPHEMERAL."""
        device = SimulatedDevice()
        # Switch to persistent
        device.dispatch(int(CmdId.SCRIPT_EVAL), _eval_payload(b"x=1", persist=True))
        assert device.script_mode == 1
        # Subsequent eval without persist flag — mode must remain PERSISTENT
        device.dispatch(int(CmdId.SCRIPT_EVAL), _eval_payload(b"y=2", persist=False))
        assert device.script_mode == 1

    def test_multiple_evals_persistent_stays(self):
        """Multiple evals in persistent mode keep mode=1."""
        device = SimulatedDevice()
        device.dispatch(int(CmdId.SCRIPT_EVAL), _eval_payload(b"a=1", persist=True))
        for _ in range(3):
            device.dispatch(int(CmdId.SCRIPT_EVAL), _eval_payload(b"b=2", persist=False))
        assert device.script_mode == 1


# ---------------------------------------------------------------------------
# Gate 4: BBP sub=4 RESET_VM wire round-trip
# ---------------------------------------------------------------------------

class TestResetVMWire:
    def test_reset_vm_response_is_ok(self):
        """sub=4 RESET_VM returns u8=1 (ok)."""
        device = SimulatedDevice()
        # First switch to persistent
        device.dispatch(int(CmdId.SCRIPT_EVAL), _eval_payload(b"x=1", persist=True))
        assert device.script_mode == 1
        # Reset VM
        resp = device.dispatch(int(CmdId.SCRIPT_AUTORUN), struct.pack('<B', 4))
        assert len(resp) == 1
        assert resp[0] == 1  # ok

    def test_reset_vm_returns_to_ephemeral(self):
        """sub=4 RESET_VM resets script_mode back to 0 (EPHEMERAL)."""
        device = SimulatedDevice()
        device.dispatch(int(CmdId.SCRIPT_EVAL), _eval_payload(b"x=1", persist=True))
        device.dispatch(int(CmdId.SCRIPT_AUTORUN), struct.pack('<B', 4))
        assert device.script_mode == 0

    def test_reset_vm_increments_auto_reset_count(self):
        """sub=4 RESET_VM increments script_auto_reset_count."""
        device = SimulatedDevice()
        device.dispatch(int(CmdId.SCRIPT_AUTORUN), struct.pack('<B', 4))
        device.dispatch(int(CmdId.SCRIPT_AUTORUN), struct.pack('<B', 4))
        assert device.script_auto_reset_count == 2


# ---------------------------------------------------------------------------
# Gate 5: BBP sub=5 STATUS_PERSISTED wire round-trip
# ---------------------------------------------------------------------------

class TestStatusPersistedWire:
    def test_status_persisted_response_length(self):
        """sub=5 STATUS_PERSISTED returns exactly 17 bytes (B + 4×I)."""
        device = SimulatedDevice()
        resp = device.dispatch(int(CmdId.SCRIPT_AUTORUN), struct.pack('<B', 5))
        assert len(resp) == 17

    def test_status_persisted_initial_mode_ephemeral(self):
        """sub=5 initial state: mode=0, globals_bytes=0, auto_reset_count=0."""
        device = SimulatedDevice()
        resp = device.dispatch(int(CmdId.SCRIPT_AUTORUN), struct.pack('<B', 5))
        mode, globals_bytes, auto_reset_count, last_eval_ms, idle_ms = struct.unpack('<BIIII', resp)
        assert mode == 0
        assert globals_bytes == 0
        assert auto_reset_count == 0

    def test_status_persisted_mode_after_persist_eval(self):
        """sub=5 after persist eval: mode=1."""
        device = SimulatedDevice()
        device.dispatch(int(CmdId.SCRIPT_EVAL), _eval_payload(b"x=1", persist=True))
        resp = device.dispatch(int(CmdId.SCRIPT_AUTORUN), struct.pack('<B', 5))
        mode, = struct.unpack_from('<B', resp, 0)
        assert mode == 1

    def test_status_persisted_after_reset_mode_zero(self):
        """sub=5 after RESET_VM: mode=0."""
        device = SimulatedDevice()
        device.dispatch(int(CmdId.SCRIPT_EVAL), _eval_payload(b"x=1", persist=True))
        device.dispatch(int(CmdId.SCRIPT_AUTORUN), struct.pack('<B', 4))
        resp = device.dispatch(int(CmdId.SCRIPT_AUTORUN), struct.pack('<B', 5))
        mode, = struct.unpack_from('<B', resp, 0)
        assert mode == 0


# ---------------------------------------------------------------------------
# Client: script_reset() USB path
# ---------------------------------------------------------------------------

class TestScriptResetClient:
    def test_script_reset_usb(self):
        """script_reset() over USB sends sub=4 and resets device mode."""
        client, device = _usb_client()
        # Switch to persistent
        client.script_eval("x = 1", persist=True)
        assert device.script_mode == 1
        # Reset
        client.script_reset()
        assert device.script_mode == 0
        client.disconnect()

    def test_script_reset_increments_auto_reset_count(self):
        """script_reset() increments auto_reset_count on device."""
        client, device = _usb_client()
        client.script_reset()
        client.script_reset()
        assert device.script_auto_reset_count == 2
        client.disconnect()


# ---------------------------------------------------------------------------
# Client: script_eval persist kwarg
# ---------------------------------------------------------------------------

class TestScriptEvalPersistClient:
    def test_eval_default_no_persist(self):
        """script_eval() default: persist=False, device stays EPHEMERAL."""
        client, device = _usb_client()
        client.script_eval("x = 1")
        assert device.script_mode == 0
        client.disconnect()

    def test_eval_persist_true_switches_mode(self):
        """script_eval(persist=True) switches device to PERSISTENT."""
        client, device = _usb_client()
        client.script_eval("x = 1", persist=True)
        assert device.script_mode == 1
        client.disconnect()

    def test_eval_persist_false_after_persistent_noop(self):
        """script_eval(persist=False) after PERSISTENT mode keeps mode=1."""
        client, device = _usb_client()
        client.script_eval("x = 1", persist=True)
        client.script_eval("y = 2", persist=False)
        assert device.script_mode == 1
        client.disconnect()

    def test_eval_returns_script_status_result(self):
        """script_eval(persist=True) still returns ScriptStatusResult."""
        client, device = _usb_client()
        result = client.script_eval("x = 1", persist=True)
        assert isinstance(result, ScriptStatusResult)
        assert result.is_running is True
        assert result.script_id >= 1
        client.disconnect()


# ---------------------------------------------------------------------------
# ScriptStatusResult backward compat: new fields have defaults
# ---------------------------------------------------------------------------

class TestScriptStatusResultDefaults:
    def test_old_construction_still_works(self):
        """ScriptStatusResult with only 5 positional args uses defaults for new fields."""
        r = ScriptStatusResult(
            is_running=False,
            script_id=0,
            total_runs=0,
            total_errors=0,
            last_error="",
        )
        assert r.mode == 0
        assert r.globals_bytes_est == 0
        assert r.globals_count == 0
        assert r.auto_reset_count == 0
        assert r.last_eval_at_ms == 0
        assert r.idle_for_ms == 0
        assert r.watermark_soft_hit is False

    def test_new_fields_accessible(self):
        """All new V2-A fields are accessible on ScriptStatusResult."""
        r = ScriptStatusResult(
            is_running=True,
            script_id=5,
            total_runs=10,
            total_errors=1,
            last_error="ValueError",
            mode=1,
            globals_bytes_est=4096,
            globals_count=12,
            auto_reset_count=2,
            last_eval_at_ms=1000,
            idle_for_ms=500,
            watermark_soft_hit=True,
        )
        assert r.mode == 1
        assert r.globals_bytes_est == 4096
        assert r.globals_count == 12
        assert r.auto_reset_count == 2
        assert r.last_eval_at_ms == 1000
        assert r.idle_for_ms == 500
        assert r.watermark_soft_hit is True
