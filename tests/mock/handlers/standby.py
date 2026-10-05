"""
System standby handlers for SimulatedDevice.

Handles: STANDBY (0x78) BBP command with suboperations:
  0 - status
  1 - presence
  2 - policy
  3 - wake
  4 - sleep

Simulates the standby state machine with fake clock for testing.
"""

import struct
import time
from dataclasses import dataclass, field
from enum import IntEnum

from bugbuster.constants import CmdId, ErrorCode
from bugbuster.standby import (
    SUBOP_STATUS, SUBOP_PRESENCE, SUBOP_POLICY, SUBOP_WAKE, SUBOP_SLEEP,
    STATUS_SCHEMA, STATUS_LEN, _STATUS_FMT,
    StandbyState, STATE_NAMES,
    PRESENCE_TTL_S, PRESENCE_MAX_CLIENTS,
    TIMEOUT_CHOICES, DEFAULT_TIMEOUT_S,
)


@dataclass
class SimulatedStandbyState:
    """Internal sim state for standby."""
    state: int = int(StandbyState.ACTIVE)
    generation: int = 0
    ready: bool = True
    last_activity_s: float = field(default_factory=time.time)
    timeout_s: int = DEFAULT_TIMEOUT_S
    # Presence sessions: {client_id: (ttl_remaining_s, time_added_s)}
    presence_slots: dict = field(default_factory=dict)
    # Inhibitor flags (bitmask)
    inhibitors: int = 0

    def presence_count(self) -> int:
        return len(self.presence_slots)

    def presence_full(self) -> bool:
        return len(self.presence_slots) >= PRESENCE_MAX_CLIENTS

    def is_active(self) -> bool:
        return self.state == int(StandbyState.ACTIVE)


def register(device) -> None:
    """Register STANDBY handler on the device."""
    device.standby_state = SimulatedStandbyState()

    def handle_standby(payload: bytes) -> bytes:
        """Handle STANDBY (0x78) BBP command.

        Suboperations:
          0 (status)    - return current status
          1 (presence)  - register/refresh client presence
          2 (policy)    - set timeout policy
          3 (wake)      - wake from sleep
          4 (sleep)     - request sleep
        """
        if len(payload) < 1:
            return struct.pack("<BB", 0x78, ErrorCode.INVALID_PARAMS)

        subop = payload[0]
        st = device.standby_state

        # Clean up expired presence slots
        now = time.time()
        expired = [cid for cid, (ttl_s, added_s) in st.presence_slots.items()
                   if (now - added_s) > ttl_s]
        for cid in expired:
            del st.presence_slots[cid]

        if subop == SUBOP_STATUS:
            return _encode_standby_status(st)

        elif subop == SUBOP_PRESENCE:
            if len(payload) < 6:
                return struct.pack("<BB", 0x78, ErrorCode.INVALID_PARAMS)
            client_id = struct.unpack_from("<I", payload, 1)[0]
            present = payload[5]
            return _handle_presence(st, client_id, bool(present))

        elif subop == SUBOP_POLICY:
            if len(payload) < 3:
                return struct.pack("<BB", 0x78, ErrorCode.INVALID_PARAMS)
            timeout_s = struct.unpack_from("<H", payload, 1)[0]
            if timeout_s not in TIMEOUT_CHOICES:
                return struct.pack("<BB", 0x78, ErrorCode.INVALID_PARAMS)
            st.timeout_s = timeout_s
            st.generation += 1
            return _encode_standby_status(st)

        elif subop == SUBOP_WAKE:
            if st.state != int(StandbyState.ASLEEP):
                return _encode_standby_status(st)
            st.state = int(StandbyState.WAKING)
            st.ready = True
            st.generation += 1
            st.last_activity_s = now
            return _encode_standby_status(st)

        elif subop == SUBOP_SLEEP:
            if st.state != int(StandbyState.ACTIVE):
                return struct.pack("<BB", 0x78, ErrorCode.INVALID_STATE)
            if st.presence_count() > 0:
                # Clients still present: refuse
                return _encode_standby_status(st)
            st.state = int(StandbyState.PREPARING)
            st.ready = False
            st.generation += 1
            st.last_activity_s = now
            # Advance to ASLEEP after a tick
            st.state = int(StandbyState.ASLEEP)
            st.generation += 1
            return _encode_standby_status(st)

        else:
            return struct.pack("<BB", 0x78, ErrorCode.INVALID_PARAMS)

    device.register_handler(CmdId.STANDBY, handle_standby)


def _encode_standby_status(st: SimulatedStandbyState) -> bytes:
    """Encode a 28-byte standby status response."""
    now = time.time()
    idle_ms = int((now - st.last_activity_s) * 1000)

    payload = struct.pack(
        "<BBBBIHBBIHHHHI",
        STATUS_SCHEMA,              # schema
        st.state,                   # state
        1 if st.ready else 0,       # ready
        st.state,                   # stage (sim uses state as stage)
        st.generation,              # generation (u32)
        st.timeout_s,               # timeout_seconds (u16)
        st.presence_count(),        # clients (u8)
        0,                          # failed_stage (u8)
        st.inhibitors,              # inhibitors (u32)
        0,                          # completed (u16)
        0,                          # failed (u16)
        0,                          # skipped (u16)
        0,                          # reserved (u16)
        idle_ms & 0xFFFFFFFF,       # idle_remaining_ms (u32)
    )
    return payload


def _handle_presence(st: SimulatedStandbyState, client_id: int, present: bool) -> bytes:
    """Handle presence register/deregister."""
    if not present:
        # Release
        st.presence_slots.pop(client_id, None)
        st.generation += 1
        return _encode_standby_status(st)

    # Register/refresh
    if client_id == 0:
        # Invalid client id
        return struct.pack("<BB", 0x78, ErrorCode.INVALID_PARAMS)

    if client_id not in st.presence_slots and st.presence_full():
        # Table full, refuse
        return struct.pack("<BB", 0x78, ErrorCode.DEVICE_BUSY)

    st.presence_slots[client_id] = (PRESENCE_TTL_S, time.time())
    st.generation += 1
    return _encode_standby_status(st)
