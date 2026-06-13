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
# Each main device has three independent physical IO groups:
# S1-S4, S5-S6, and S7-S8. Only switches inside one group are exclusive.
# ---------------------------------------------------------------------------

_GROUP_MASKS = (0x0F, 0x30, 0xC0)


def _group_mask_for_switch(sw_idx: int) -> int:
    if sw_idx < 4:
        return _GROUP_MASKS[0]
    if sw_idx < 6:
        return _GROUP_MASKS[1]
    return _GROUP_MASKS[2]


def _adgs_apply_mutual_exclusion(device, dev_idx: int, new_state: int) -> int:
    """
    Enforce the one-switch-closed-at-a-time rule per physical IO group.

    *new_state* is the desired bitmask for device *dev_idx*.
    Returns the allowed new_state (possibly modified to auto-open same-group switches).
    Updates device.adgs_active[dev_idx] to reflect the first closed switch.
    """
    allowed = 0
    first_closed = None
    for mask in _GROUP_MASKS:
        group_state = new_state & mask
        if not group_state:
            continue
        lowest = group_state & -group_state
        allowed |= lowest
        if first_closed is None:
            first_closed = lowest.bit_length() - 1

    device.adgs_active[dev_idx] = first_closed
    return allowed


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
                group_mask = _group_mask_for_switch(switch_idx)
                device.mux_states[dev_idx] &= ~group_mask & 0xFF
                device.mux_states[dev_idx] |= (1 << switch_idx)
                device.mux_states[dev_idx] = _adgs_apply_mutual_exclusion(
                    device, dev_idx, device.mux_states[dev_idx]
                )
            else:
                device.mux_states[dev_idx] &= ~(1 << switch_idx)
                device.mux_states[dev_idx] &= 0xFF
                remaining = device.mux_states[dev_idx]
                device.adgs_active[dev_idx] = (
                    (remaining & -remaining).bit_length() - 1 if remaining else None
                )
        return b''
    return handler
