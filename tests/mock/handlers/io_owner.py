"""
IO Ownership command handlers for SimulatedDevice (BBP v5+).

Handles:
  IO_CLAIM         (0xA7) - acquire one or more slots
  IO_RELEASE       (0xA8) - release one or more slots (n=0 -> all of the caller's)
  IO_OWNER_STATUS  (0xA9) - read the full 16-slot ownership table (160 bytes)
  IO_FORCE_RELEASE (0xAA) - admin-gated unconditional release

Wire format - mirrors Firmware/ESP32/src/bbp/cmds/cmd_io_owner.cpp:
  IO_CLAIM   payload: n_slots(u8), slots(u8[n]), lease_ms(u32), purpose_tag(u32)
             reply:   n_slots(u8), status(u8[n])  0 = acquired, 11 = CMD_ERR_IO_OWNERSHIP
             n = 0, n > 16 or any slot >= 16 -> BBP error (INVALID_CHANNEL)
  IO_RELEASE payload: n_slots(u8), slots(u8[n]); n = 0 releases all of the caller's slots
             reply:   n_slots(u8), released(u8[n]) bools; for n = 0: 0, count_released
  IO_OWNER_STATUS reply: 16 x {kind(u8), session_id(u8), token_fp32(u32),
                                lease_until_ms(u32)} = 160 bytes
  IO_FORCE_RELEASE payload: slot(u8), token_len(u8), token(u8[token_len])
    The token check is simulator-only (the firmware leaves auth to the transport).
"""

import struct

from bugbuster.constants import CmdId, ErrorCode
from bugbuster.transport.usb import DeviceError

_ADMIN_TOKEN = b"SIMTOKEN"   # must match core.py

# Per-slot IO_CLAIM status (firmware CmdError values, mirror IoClaimStatus)
_IO_OK            = 0x00
_IO_HELD_BY_OTHER = 11      # CMD_ERR_IO_OWNERSHIP
# Simulator-only IO_FORCE_RELEASE reply for a wrong token.
_IO_ADMIN_REQUIRED = 0x05

# Owner kind for the USB client (matches IoOwnerKind.USB = 1)
_KIND_USB      = 1
_KIND_NONE     = 0
_KIND_INTERNAL = 5


def register(device) -> None:
    device.register_handler(CmdId.IO_CLAIM,         _io_claim(device))
    device.register_handler(CmdId.IO_RELEASE,       _io_release(device))
    device.register_handler(CmdId.IO_OWNER_STATUS,  _io_owner_status(device))
    device.register_handler(CmdId.IO_FORCE_RELEASE, _io_force_release(device))


def _free(entry) -> None:
    entry["kind"]           = _KIND_NONE
    entry["session_id"]     = 0
    entry["token_fp32"]     = 0
    entry["lease_until_ms"] = 0
    entry["purpose_tag"]    = 0


# ---------------------------------------------------------------------------
# IO_CLAIM (0xA7)
# ---------------------------------------------------------------------------

def _io_claim(device):
    def handler(payload: bytes) -> bytes:
        if len(payload) < 1 or not 1 <= payload[0] <= 16:
            raise DeviceError(ErrorCode.INVALID_CHANNEL, CmdId.IO_CLAIM)
        n = payload[0]
        if len(payload) < 1 + n + 8:
            raise DeviceError(ErrorCode.INVALID_PARAM, CmdId.IO_CLAIM)
        slots = list(payload[1:1 + n])
        if any(s >= 16 for s in slots):
            raise DeviceError(ErrorCode.INVALID_CHANNEL, CmdId.IO_CLAIM)
        lease_ms, = struct.unpack_from('<I', payload, 1 + n)
        purpose_tag, = struct.unpack_from('<I', payload, 1 + n + 4)

        lease_until = (device._now_ms + lease_ms) if lease_ms > 0 else 0

        per_slot = bytearray()
        for slot in slots:
            entry = device.io_owner_table[slot]
            current_kind = entry["kind"]
            # Expired lease that tick() has not reaped yet: treat as free and tell
            # the displaced owner (BBP_EVT_IO_PREEMPTED = 0x86: kind, slot).
            lease_was_expired = (
                current_kind != _KIND_NONE
                and entry["lease_until_ms"] > 0
                and device._now_ms > entry["lease_until_ms"]
            )
            if lease_was_expired:
                device.emit_event(0x86, bytes([current_kind, slot]))
                current_kind = _KIND_NONE
            if current_kind == _KIND_NONE or (
                current_kind == _KIND_USB and entry["session_id"] == device._usb_session_id
            ):
                entry["kind"]           = _KIND_USB
                entry["session_id"]     = device._usb_session_id
                entry["token_fp32"]     = 0
                entry["lease_until_ms"] = lease_until
                entry["purpose_tag"]    = purpose_tag
                per_slot.append(_IO_OK)
            else:
                per_slot.append(_IO_HELD_BY_OTHER)

        return bytes([n]) + bytes(per_slot)
    return handler


# ---------------------------------------------------------------------------
# IO_RELEASE (0xA8)
# ---------------------------------------------------------------------------

def _io_release(device):
    def mine(entry) -> bool:
        return entry["kind"] == _KIND_USB and entry["session_id"] == device._usb_session_id

    def handler(payload: bytes) -> bytes:
        if len(payload) < 1:
            raise DeviceError(ErrorCode.INVALID_PARAM, CmdId.IO_RELEASE)
        n = payload[0]
        if n == 0:
            count = 0
            for entry in device.io_owner_table:
                if mine(entry):
                    _free(entry)
                    count += 1
            return bytes([0, count])
        if n > 16:
            raise DeviceError(ErrorCode.INVALID_CHANNEL, CmdId.IO_RELEASE)
        if len(payload) < 1 + n:
            raise DeviceError(ErrorCode.INVALID_PARAM, CmdId.IO_RELEASE)
        slots = list(payload[1:1 + n])
        if any(s >= 16 for s in slots):
            raise DeviceError(ErrorCode.INVALID_CHANNEL, CmdId.IO_RELEASE)
        released = bytearray()
        for slot in slots:
            entry = device.io_owner_table[slot]
            ok = mine(entry)
            if ok:
                _free(entry)
            released.append(1 if ok else 0)
        return bytes([n]) + bytes(released)
    return handler


# ---------------------------------------------------------------------------
# IO_OWNER_STATUS (0xA9)
# ---------------------------------------------------------------------------

def _io_owner_status(device):
    def handler(payload: bytes) -> bytes:
        buf = bytearray()
        for entry in device.io_owner_table:
            buf += struct.pack('<BB', entry["kind"], entry["session_id"])
            buf += struct.pack('<I', entry["token_fp32"])
            buf += struct.pack('<I', entry["lease_until_ms"] & 0xFFFFFFFF)
        return bytes(buf)  # 16 * 10 = 160 bytes
    return handler


# ---------------------------------------------------------------------------
# IO_FORCE_RELEASE (0xAA)
# Payload: slot(u8), token_len(u8), token(u8[token_len]); slot 0xFF = all
# Response: slot(u8) as the firmware, or _IO_ADMIN_REQUIRED for a wrong token
# ---------------------------------------------------------------------------

def _io_force_release(device):
    def handler(payload: bytes) -> bytes:
        if len(payload) < 2:
            return bytes([_IO_ADMIN_REQUIRED])
        slot = payload[0]
        tok_len = payload[1]
        token = payload[2:2 + tok_len]
        if token != _ADMIN_TOKEN:
            return bytes([_IO_ADMIN_REQUIRED])
        if slot == 0xFF:
            for entry in device.io_owner_table:
                _free(entry)
            return bytes([slot])
        if slot >= 16:
            raise DeviceError(ErrorCode.INVALID_CHANNEL, CmdId.IO_FORCE_RELEASE)
        _free(device.io_owner_table[slot])
        return bytes([slot])
    return handler
