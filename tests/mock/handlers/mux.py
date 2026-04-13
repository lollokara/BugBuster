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

    device.register_handler(CmdId.MUX_SET_ALL,    _mux_set_all(device))
    device.register_handler(CmdId.MUX_GET_ALL,    _mux_get_all(device))
    device.register_handler(CmdId.MUX_SET_SWITCH, _mux_set_switch(device))


# ---------------------------------------------------------------------------
# MUX_SET_ALL (0x90)
# client.py sends: bytes(states)  — 4 bytes, one per ADGS2414D device
# ---------------------------------------------------------------------------

def _mux_set_all(device):
    def handler(payload: bytes) -> bytes:
        for i in range(4):
            if i < len(payload):
                device.mux_states[i] = payload[i] & 0xFF
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
                device.mux_states[dev_idx] |= (1 << switch_idx)
            else:
                device.mux_states[dev_idx] &= ~(1 << switch_idx)
                device.mux_states[dev_idx] &= 0xFF
        return b''
    return handler
