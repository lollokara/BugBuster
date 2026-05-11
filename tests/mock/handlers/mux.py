"""
MUX switch matrix handlers for SimulatedDevice.

Commands handled:
  MUX_SET_ALL    (0x90) — set all 32 switches via 4-byte bitmask
  MUX_GET_ALL    (0x91) — return current 4-byte switch state
  MUX_SET_SWITCH (0x92) — open/close a single switch
"""

import struct
from bugbuster.constants import CmdId


def register(device) -> None:
    # Ensure mux_states exists as a list of 4 bytes
    if not hasattr(device, "mux_states"):
        device.mux_states = [0, 0, 0, 0]
    # Ensure ADGS mutual-exclusion tracking exists
    if not hasattr(device, "adgs_active"):
        device.adgs_active = [None, None, None, None]

    device.register_handler(CmdId.MUX_SET_ALL,    _mux_set_all(device))
    device.register_handler(CmdId.MUX_GET_ALL,    _mux_get_all(device))
    device.register_handler(CmdId.MUX_SET_SWITCH, _mux_set_switch(device))


# ---------------------------------------------------------------------------
# ADGS mutual-exclusion helper
# Device index 3 (U23 selftest) is exempt from the one-switch-at-a-time rule.
# ---------------------------------------------------------------------------

_ADGS_SELFTEST_IDX = 3  # U23 — exempt from mutual-exclusion enforcement


def _adgs_apply_mutual_exclusion(device, dev_idx: int, new_state: int) -> int:
    """
    Enforce the one-switch-closed-at-a-time rule for non-selftest ADGS devices.

    *new_state* is the desired bitmask for device *dev_idx*.
    Returns the allowed new_state (possibly modified to auto-open prior switch).
    Updates device.adgs_active[dev_idx] to reflect the first closed switch.
    """
    if dev_idx == _ADGS_SELFTEST_IDX:
        # Selftest device is exempt — no mutual exclusion
        return new_state

    # Determine the first (lowest-bit) switch that is closed in new_state
    first_closed = None
    for bit in range(8):
        if new_state & (1 << bit):
            first_closed = bit
            break

    device.adgs_active[dev_idx] = first_closed
    # Enforce: only one switch may be closed — keep lowest bit only
    if new_state and (new_state & (new_state - 1)):
        # More than one bit set — open all except the lowest
        lowest = new_state & (-new_state)
        new_state = lowest
        device.adgs_active[dev_idx] = lowest.bit_length() - 1

    return new_state


# ---------------------------------------------------------------------------
# MUX_SET_ALL (0x90)
# client.py sends: bytes(states)  — 4 bytes, one per ADGS2414D device
# Raw bitmap write — bypasses mutual-exclusion (used for bulk test patterns).
# adgs_active is updated to reflect the first closed switch per device.
# ---------------------------------------------------------------------------

def _mux_set_all(device):
    def handler(payload: bytes) -> bytes:
        for i in range(4):
            if i < len(payload):
                state = payload[i] & 0xFF
                device.mux_states[i] = state
                # Update adgs_active tracking (no enforcement — raw write)
                if i != _ADGS_SELFTEST_IDX:
                    first = None
                    for bit in range(8):
                        if state & (1 << bit):
                            first = bit
                            break
                    device.adgs_active[i] = first
        return b''
    return handler


# ---------------------------------------------------------------------------
# MUX_GET_ALL (0x91)
# client.py: return list(resp[:4])
# ---------------------------------------------------------------------------

def _mux_get_all(device):
    def handler(payload: bytes) -> bytes:
        return bytes(device.mux_states[:4])
    return handler


# ---------------------------------------------------------------------------
# MUX_SET_SWITCH (0x92)
# client.py sends: struct.pack('<BBB', device_idx, switch, int(closed))
# device_idx 0–3, switch 0–7, closed bool
# ---------------------------------------------------------------------------

def _mux_set_switch(device):
    def handler(payload: bytes) -> bytes:
        dev_idx, switch_idx, closed = struct.unpack_from('<BBB', payload)
        if 0 <= dev_idx < 4 and 0 <= switch_idx < 8:
            if closed:
                if dev_idx != _ADGS_SELFTEST_IDX:
                    # Auto-open any previously closed switch on this device
                    device.mux_states[dev_idx] = 0
                device.mux_states[dev_idx] |= (1 << switch_idx)
                device.adgs_active[dev_idx] = switch_idx
            else:
                device.mux_states[dev_idx] &= ~(1 << switch_idx)
                device.mux_states[dev_idx] &= 0xFF
                # Update adgs_active: find next closed switch, or None
                if dev_idx != _ADGS_SELFTEST_IDX:
                    remaining = device.mux_states[dev_idx]
                    device.adgs_active[dev_idx] = (
                        (remaining & -remaining).bit_length() - 1 if remaining else None
                    )
        return b''
    return handler
