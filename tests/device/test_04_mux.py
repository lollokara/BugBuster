"""
test_04_mux.py — ADGS2414D MUX switch matrix tests.

The MUX consists of 4 × ADGS2414D devices × 8 switches = 32 switches total.
The Python API uses 4-byte representations: one byte per device, one bit per switch.

Switch state representation:
  - mux_get()       → list of 4 bytes (one per device)
  - mux_set_all()   → list of 4 bytes
  - mux_set_switch(device, switch, closed) → individual switch control
"""

import time
import pytest
from conftest import assert_no_faults

pytestmark = [pytest.mark.timeout(60)]

# All switches open: 4 devices × byte 0x00
ALL_OPEN = [0x00, 0x00, 0x00, 0x00]

# All switches closed: 4 devices × byte 0xFF
ALL_CLOSED = [0xFF, 0xFF, 0xFF, 0xFF]


# ---------------------------------------------------------------------------
# Helper
# ---------------------------------------------------------------------------

def _restore_all_open(device):
    """Restore all MUX switches to open state."""
    try:
        device.mux_set_all(ALL_OPEN)
    except Exception:
        pass


# The supply monitor routes one rail through U23 every 5 s (IO-25). While it
# does, closing U17-S3 (device 2, bit 2) is refused by the interlock - correct
# behaviour, but it makes any pattern touching that bit time-dependent.
_U23_WINDOW_S = 7.0


def _is_route_rejected(exc) -> bool:
    from bugbuster.constants import ErrorCode
    if getattr(exc, "code", None) == ErrorCode.ADGS_ROUTE_REJECTED:
        return True
    response = getattr(exc, "response", None)
    return getattr(response, "status_code", None) == 409 or "HTTP 409" in str(exc)


def _set_all_retrying(device, pattern):
    """mux_set_all that waits out a U23 self-test window."""
    deadline = time.monotonic() + _U23_WINDOW_S
    while True:
        try:
            return device.mux_set_all(pattern)
        except Exception as exc:
            if not _is_route_rejected(exc) or time.monotonic() > deadline:
                raise
            time.sleep(0.2)


# ---------------------------------------------------------------------------
# Get all switches
# ---------------------------------------------------------------------------

def test_mux_get_all_returns_4_bytes(device):
    """
    mux_get() returns a list of 4 bytes (one per ADGS2414D device).
    Each byte encodes the state of 8 switches (bit 0 = switch 0, etc.).
    """
    states = device.mux_get()

    assert isinstance(states, list), f"mux_get() must return list, got {type(states)}"
    assert len(states) == 4, f"Expected 4 device bytes, got {len(states)}"
    for i, byte in enumerate(states):
        assert 0 <= byte <= 0xFF, f"Device {i} byte {byte} out of 0–255 range"
    assert_no_faults(device)


# ---------------------------------------------------------------------------
# Set all open
# ---------------------------------------------------------------------------

def test_mux_set_all_open(device):
    """
    mux_set_all([0,0,0,0]) opens all 32 switches.
    Read back should confirm all bytes are 0.
    """
    device.mux_set_all(ALL_OPEN)
    time.sleep(0.15)  # firmware enforces 100 ms dead time

    states = device.mux_get()
    assert states == ALL_OPEN, f"Expected all-open {ALL_OPEN}, got {states}"
    assert_no_faults(device)


# ---------------------------------------------------------------------------
# Set first switch closed
# ---------------------------------------------------------------------------

def test_mux_set_all_close_first(device):
    """
    mux_set_all([1, 0, 0, 0]) closes switch 0 of device 0 only.
    All other switches remain open.
    """
    target = [0x01, 0x00, 0x00, 0x00]
    device.mux_set_all(target)
    time.sleep(0.15)

    states = device.mux_get()
    assert states == target, (
        f"Expected {target} (only switch 0 closed), got {states}"
    )

    _restore_all_open(device)
    assert_no_faults(device)


# ---------------------------------------------------------------------------
# Single switch close
# ---------------------------------------------------------------------------

def test_mux_single_switch_close(device):
    """
    mux_set_switch(0, 0, True) closes switch 0 of device 0.
    Other switches should remain unchanged.
    """
    device.mux_set_all(ALL_OPEN)  # start clean
    time.sleep(0.15)

    device.mux_set_switch(0, 0, True)
    time.sleep(0.15)

    states = device.mux_get()
    assert states[0] & 0x01, "Switch 0 of device 0 should be closed (bit 0 set)"

    _restore_all_open(device)
    assert_no_faults(device)


# ---------------------------------------------------------------------------
# Single switch open
# ---------------------------------------------------------------------------

def test_mux_single_switch_open(device):
    """
    Close switch 0 of device 0, then open it with mux_set_switch(0, 0, False).
    Verify the switch bit is cleared.
    """
    device.mux_set_switch(0, 0, True)
    time.sleep(0.15)
    device.mux_set_switch(0, 0, False)
    time.sleep(0.15)

    states = device.mux_get()
    assert not (states[0] & 0x01), "Switch 0 of device 0 should be open (bit 0 clear)"
    assert_no_faults(device)


# ---------------------------------------------------------------------------
# Round trip
# ---------------------------------------------------------------------------

def test_mux_round_trip(device):
    """
    Write a specific pattern to the MUX, read it back, and verify it matches.
    Uses a checkerboard pattern: device 0 = 0xAA, device 1 = 0x55, etc.
    """
    pattern = [0xAA, 0x55, 0xAA, 0x55]
    _set_all_retrying(device, pattern)
    time.sleep(0.15)

    readback = device.mux_get()
    assert readback == pattern, f"MUX round-trip failed: wrote {pattern}, read {readback}"

    _restore_all_open(device)
    assert_no_faults(device)


# ---------------------------------------------------------------------------
# Each device independently
# ---------------------------------------------------------------------------

def test_mux_per_device_control(device):
    """
    Verify each of the 4 ADGS2414D devices can be controlled independently.
    Set one device at a time to 0xFF and confirm others remain 0x00.

    MUX-1 fix: Device 2 (U17) has a hardware interlock with the self-test MUX
    (U23). Disable the selftest worker before testing device 2 to avoid
    spurious BUSY errors. Retry with backoff for transient BUSY because the
    supply monitor owns U23 intermittently even when worker is off.
    """
    import time as _time
    from bugbuster.constants import CmdId

    # Disable selftest worker before device 2 to avoid U17-S3 interlock
    selftest_was_on = False
    if hasattr(device, "_usb") and device._usb:  # noqa: SLF001
        # USB: direct BBP command
        try:
            device._usb_cmd(CmdId.SELFTEST_WORKER, b"\x00")  # noqa: SLF001
            selftest_was_on = True
        except Exception:
            pass
    elif hasattr(device, "_http_post"):  # noqa: SLF001
        # HTTP: API route
        try:
            device._http_post("/selftest/worker", {"enabled": False})  # noqa: SLF001
            selftest_was_on = True
        except Exception:
            pass

    try:
        for dev_idx in range(4):
            target = [0xFF if i == dev_idx else 0x00 for i in range(4)]

            # Device 2 may be refused by the U17-S3 interlock even with the
            # worker off (the supply monitor owns U23 for a window every 5 s).
            _set_all_retrying(device, target)

            _time.sleep(0.15)

            states = device.mux_get()
            assert states == target, (
                f"Device {dev_idx} only: wrote {target}, got {states}"
            )
    finally:
        # Restore selftest worker if it was on
        if selftest_was_on:
            if hasattr(device, "_usb") and device._usb:  # noqa: SLF001
                try:
                    device._usb_cmd(CmdId.SELFTEST_WORKER, b"\x01")  # noqa: SLF001
                except Exception:
                    pass
            elif hasattr(device, "_http_post"):  # noqa: SLF001
                try:
                    device._http_post("/selftest/worker", {"enabled": True})  # noqa: SLF001
                except Exception:
                    pass

    _restore_all_open(device)
    assert_no_faults(device)


# ---------------------------------------------------------------------------
# Restore all open (cleanup)
# ---------------------------------------------------------------------------

def test_mux_set_all_open_restore(device):
    """
    Final cleanup test: verify all switches can be opened successfully.
    This test should always pass to leave the device in a safe state.
    """
    device.mux_set_all(ALL_OPEN)
    time.sleep(0.15)

    states = device.mux_get()
    assert states == ALL_OPEN, f"Could not restore all switches to open: {states}"
    assert_no_faults(device)


# ---------------------------------------------------------------------------
# U17-S3 interlock regression test
# ---------------------------------------------------------------------------

def test_mux_device2_interlock_reports_correctly(device, request):
    """
    Regression test for MUX-1: verify the U17-S3 / U23 interlock reports BUSY
    when the selftest worker is running, instead of silently ignoring the write.

    Device 2 (U17) and the self-test MUX (U23) share a signal path. When U23 is
    active (selftest worker running), writing to device 2 must return BUSY, not
    silently succeed with stale state.

    This test is a regression guard for the firmware fix that made the interlock
    honest. If the test fails, the silent-success bug has returned.

    NOTE: Skipped in simulator mode - the simulator does not implement the U17-S3
    interlock.
    """
    # Skip in simulator mode - no interlock implementation
    _sim_mode = request.config.getoption("--sim", default=False) or request.config.getoption("--sim-full", default=False)
    if _sim_mode:
        pytest.skip("Simulator does not implement U17-S3 interlock")
    import time as _time
    from bugbuster.constants import CmdId
    from bugbuster.transport.usb import DeviceError
    from requests.exceptions import HTTPError

    # Enable selftest worker explicitly
    if hasattr(device, "_usb") and device._usb:  # noqa: SLF001
        device._usb_cmd(CmdId.SELFTEST_WORKER, b"\x01")  # noqa: SLF001
    elif hasattr(device, "_http_post"):  # noqa: SLF001
        device._http_post("/selftest/worker", {"enabled": True})  # noqa: SLF001

    _time.sleep(0.1)  # Let worker settle

    # Try to close U17-S3 (device 2, bit 2) while selftest is active. The
    # supply monitor only holds U23 for a short window every 5 s (IO-25), so
    # keep trying for one full period; a close outside the window is correct.
    target = [0x00, 0x00, 0x04, 0x00]  # U17_S3_MASK = 0x04

    got_busy = False
    deadline = _time.monotonic() + _U23_WINDOW_S
    while not got_busy and _time.monotonic() < deadline:
        try:
            device.mux_set_all(target)
            device.mux_set_all(ALL_OPEN)
        except (DeviceError, HTTPError) as exc:
            got_busy = _is_route_rejected(exc)
            if not got_busy:
                raise
        _time.sleep(0.05)

    # Disable selftest worker for cleanup
    if hasattr(device, "_usb") and device._usb:  # noqa: SLF001
        device._usb_cmd(CmdId.SELFTEST_WORKER, b"\x00")  # noqa: SLF001
    elif hasattr(device, "_http_post"):  # noqa: SLF001
        device._http_post("/selftest/worker", {"enabled": False})  # noqa: SLF001

    assert got_busy, (
        "MUX device 2 U17-S3 write with selftest active should return "
        "ROUTE_REJECTED (0x13) / HTTP 409. "
        "If this fails, the interlock is not reporting correctly - "
        "the silent-success bug (MUX-1) has returned."
    )

    _restore_all_open(device)
    assert_no_faults(device)


# ---------------------------------------------------------------------------
# Invalid payload
# ---------------------------------------------------------------------------

def test_mux_set_all_invalid_length_raises(device):
    """
    mux_set_all() with wrong number of bytes should raise ValueError.
    The API requires exactly 4 bytes.
    """
    with pytest.raises(ValueError):
        device.mux_set_all([0x00, 0x00])  # only 2 bytes — invalid
    assert_no_faults(device)
