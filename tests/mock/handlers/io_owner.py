"""
IO Ownership command handlers for SimulatedDevice (BBP v5+).

Handles:
  IO_CLAIM         (0xA7) — acquire one or more slots
  IO_RELEASE       (0xA8) — release one or more slots (n=0 → release-all-by-caller)
  IO_OWNER_STATUS  (0xA9) — read the full 16-slot ownership table (160 bytes)
  IO_FORCE_RELEASE (0xAA) — admin-gated unconditional release

Wire format — §2 of IoOwnership plan:
  IO_CLAIM payload:   n_slots(u8), slots(u8[n]), lease_ms(u32), purpose_tag(u32)
  IO_OWNER_STATUS response: 16 × {kind(u8), session_id(u8), token_fp32(u32),
                                   lease_until_ms(u32)} = 160 bytes
  IO_FORCE_RELEASE payload: slot(u8), token_len(u8), token(u8[token_len])

Status bytes returned per-slot (first byte of IO_CLAIM response):
  0x00  IO_OK
  0x01  IO_HELD_BY_OTHER
  0x02  IO_NOT_OWNED
  0x03  IO_INVALID_SLOT
  0x05  IO_ADMIN_REQUIRED
"""

import struct
from bugbuster.constants import CmdId

_ADMIN_TOKEN = b"SIMTOKEN"   # must match core.py

# Per-slot status codes (mirror IoClaimStatus in constants.py)
_IO_OK            = 0x00
_IO_HELD_BY_OTHER = 0x01
_IO_NOT_OWNED     = 0x02
_IO_INVALID_SLOT  = 0x03
# LEASE_EXPIRED (0x04): diagnostic only — never returned by IO_CLAIM itself.
# Emitted as an event via device.emit_event() when a slot's lease expires and a
# new claim arrives for that same slot, so the prior owner's next
# io_owner_status() query observes the transition via EVT_IO_PREEMPTED.
_IO_LEASE_EXPIRED  = 0x04
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


# ---------------------------------------------------------------------------
# IO_CLAIM (0xA7)
# Payload: n_slots(u8), slots(u8[n]), lease_ms(u32 LE), purpose_tag(u32 LE)
# Response: global_status(u8), per_slot_status(u8[n])
# ---------------------------------------------------------------------------

def _io_claim(device):
    def handler(payload: bytes) -> bytes:
        if len(payload) < 1:
            return bytes([_IO_INVALID_SLOT])
        n = payload[0]
        if len(payload) < 1 + n + 8:
            return bytes([_IO_INVALID_SLOT])
        slots      = list(payload[1:1 + n])
        lease_ms,  = struct.unpack_from('<I', payload, 1 + n)
        purpose_tag, = struct.unpack_from('<I', payload, 1 + n + 4)

        lease_until = (device._now_ms + lease_ms) if lease_ms > 0 else 0

        per_slot = bytearray()
        global_ok = True
        for slot in slots:
            if slot >= 16:
                per_slot.append(_IO_INVALID_SLOT)
                global_ok = False
                continue
            entry = device.io_owner_table[slot]
            current_kind = entry["kind"]
            # Detect expired lease: slot was owned but tick() hasn't run yet.
            # Emit LEASE_EXPIRED event so displaced owner can observe the expiry.
            lease_was_expired = (
                current_kind != _KIND_NONE
                and entry["lease_until_ms"] > 0
                and device._now_ms > entry["lease_until_ms"]
            )
            if lease_was_expired:
                # Treat as free; emit event to notify prior owner.
                # BBP_EVT_IO_PREEMPTED = 0x86, payload: displaced_kind(u8), slot(u8)
                device.emit_event(
                    0x86,  # BBP_EVT_IO_PREEMPTED
                    bytes([current_kind, slot]),
                )
                current_kind = _KIND_NONE
            if current_kind == _KIND_NONE or (
                current_kind == _KIND_USB and entry["session_id"] == device._usb_session_id
            ):
                # Free or already owned by this session — grant
                entry["kind"]           = _KIND_USB
                entry["session_id"]     = device._usb_session_id
                entry["token_fp32"]     = 0
                entry["lease_until_ms"] = lease_until
                entry["purpose_tag"]    = purpose_tag
                per_slot.append(_IO_OK)
            else:
                per_slot.append(_IO_HELD_BY_OTHER)
                global_ok = False

        overall = _IO_OK if global_ok else _IO_HELD_BY_OTHER
        return bytes([overall]) + bytes(per_slot)
    return handler


# ---------------------------------------------------------------------------
# IO_RELEASE (0xA8)
# Payload: n_slots(u8), slots(u8[n])  — n=0 → release all slots owned by caller
# Response: status(u8)
# ---------------------------------------------------------------------------

def _io_release(device):
    def handler(payload: bytes) -> bytes:
        if len(payload) < 1:
            return bytes([_IO_INVALID_SLOT])
        n = payload[0]
        if n == 0:
            # Release all slots owned by this USB session
            for entry in device.io_owner_table:
                if entry["kind"] == _KIND_USB and entry["session_id"] == device._usb_session_id:
                    entry["kind"]           = _KIND_NONE
                    entry["session_id"]     = 0
                    entry["token_fp32"]     = 0
                    entry["lease_until_ms"] = 0
                    entry["purpose_tag"]    = 0
            return bytes([_IO_OK])
        if len(payload) < 1 + n:
            return bytes([_IO_INVALID_SLOT])
        slots = list(payload[1:1 + n])
        global_ok = True
        for slot in slots:
            if slot >= 16:
                global_ok = False
                continue
            entry = device.io_owner_table[slot]
            if entry["kind"] == _KIND_USB and entry["session_id"] == device._usb_session_id:
                entry["kind"]           = _KIND_NONE
                entry["session_id"]     = 0
                entry["token_fp32"]     = 0
                entry["lease_until_ms"] = 0
                entry["purpose_tag"]    = 0
            elif entry["kind"] != _KIND_NONE:
                global_ok = False
        return bytes([_IO_OK if global_ok else _IO_NOT_OWNED])
    return handler


# ---------------------------------------------------------------------------
# IO_OWNER_STATUS (0xA9)
# Payload: empty
# Response: 16 × {kind(u8), session_id(u8), token_fp32(u32 LE),
#                 lease_until_ms(u32 LE)} = 160 bytes
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
# Payload: slot(u8), token_len(u8), token(u8[token_len])
# Response: status(u8)
# ---------------------------------------------------------------------------

def _io_force_release(device):
    def handler(payload: bytes) -> bytes:
        if len(payload) < 2:
            return bytes([_IO_ADMIN_REQUIRED])
        slot      = payload[0]
        tok_len   = payload[1]
        token     = payload[2:2 + tok_len]
        if token != _ADMIN_TOKEN:
            return bytes([_IO_ADMIN_REQUIRED])
        if slot == 0xFF:
            # Release all slots
            for entry in device.io_owner_table:
                entry["kind"]           = _KIND_NONE
                entry["session_id"]     = 0
                entry["token_fp32"]     = 0
                entry["lease_until_ms"] = 0
                entry["purpose_tag"]    = 0
            return bytes([_IO_OK])
        if slot >= 16:
            return bytes([_IO_INVALID_SLOT])
        entry = device.io_owner_table[slot]
        entry["kind"]           = _KIND_NONE
        entry["session_id"]     = 0
        entry["token_fp32"]     = 0
        entry["lease_until_ms"] = 0
        entry["purpose_tag"]    = 0
        return bytes([_IO_OK])
    return handler
