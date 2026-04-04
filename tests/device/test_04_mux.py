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
import bugbuster as bb
from conftest import assert_no_faults

pytestmark = [pytest.mark.timeout(10)]

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
    device.mux_set_all(pattern)
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
    """
    for dev_idx in range(4):
        target = [0xFF if i == dev_idx else 0x00 for i in range(4)]
        device.mux_set_all(target)
        time.sleep(0.15)

        states = device.mux_get()
        assert states == target, (
            f"Device {dev_idx} only: wrote {target}, got {states}"
        )

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
