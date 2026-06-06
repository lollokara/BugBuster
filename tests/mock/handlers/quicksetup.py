"""
Quick-setup slot handlers for SimulatedDevice.

Commands handled:
  QS_LIST   (0xF0) — list occupied slots (bitmap + hashes)
  QS_GET    (0xF1) — read slot JSON
  QS_SAVE   (0xF2) — snapshot device state to slot
  QS_APPLY  (0xF3) — apply slot to device state
  QS_DELETE (0xF4) — delete slot
"""

import json
import struct
import time
from bugbuster.constants import CmdId

SLOT_COUNT = 4


def register(device) -> None:
    if not hasattr(device, "qs_slots"):
        device.qs_slots = [None] * SLOT_COUNT  # None or {"name": str, "ts": int, "hash": int, "payload": dict}

    device.register_handler(CmdId.QS_LIST,   _qs_list(device))
    device.register_handler(CmdId.QS_GET,    _qs_get(device))
    device.register_handler(CmdId.QS_SAVE,   _qs_save(device))
    device.register_handler(CmdId.QS_APPLY,  _qs_apply(device))
    device.register_handler(CmdId.QS_DELETE, _qs_delete(device))


def _snapshot(device) -> dict:
    """Build a minimal JSON-serialisable snapshot of live device state."""
    channels = []
    for ch in device.channels:
        channels.append({
            "id": ch["id"],
            "function": ch["function"],
            "dac_code": ch["dac_code"],
            "dac_value": ch["dac_value"],
        })
    return {"channels": channels}


def _slot_hash(payload: dict) -> int:
    raw = json.dumps(payload, sort_keys=True).encode()
    h = 0
    for b in raw:
        h = ((h << 5) + h + b) & 0xFF
    return h


def _qs_list(device):
    def handler(payload: bytes) -> bytes:
        bitmap = 0
        hashes = []
        for i, slot in enumerate(device.qs_slots):
            if slot is not None:
                bitmap |= (1 << i)
                hashes.append(slot["hash"])
            else:
                hashes.append(0)
        return struct.pack('<B' + 'B' * SLOT_COUNT, bitmap, *hashes)
    return handler


def _qs_get(device):
    def handler(payload: bytes) -> bytes:
        if not payload:
            return b''
        idx = payload[0]
        if idx >= SLOT_COUNT or device.qs_slots[idx] is None:
            return b''
        return json.dumps(device.qs_slots[idx]["payload"]).encode()
    return handler


def _qs_save(device):
    def handler(payload: bytes) -> bytes:
        if not payload:
            return b''
        idx = payload[0]
        if idx >= SLOT_COUNT:
            return b''
        snap = _snapshot(device)
        device.qs_slots[idx] = {
            "name": f"slot{idx}",
            "ts": int(time.time()),
            "hash": _slot_hash(snap),
            "payload": snap,
        }
        return json.dumps(snap).encode()
    return handler


def _qs_apply(device):
    def handler(payload: bytes) -> bytes:
        if not payload:
            return struct.pack('<B', 2)
        idx = payload[0]
        if idx >= SLOT_COUNT or device.qs_slots[idx] is None:
            return struct.pack('<B', 1)  # not found
        snap = device.qs_slots[idx]["payload"]
        for ch_snap in snap.get("channels", []):
            cid = ch_snap.get("id")
            for ch in device.channels:
                if ch["id"] == cid:
                    ch["function"] = ch_snap.get("function", ch["function"])
                    ch["dac_code"] = ch_snap.get("dac_code", ch["dac_code"])
                    ch["dac_value"] = ch_snap.get("dac_value", ch["dac_value"])
        return struct.pack('<B', 0)  # ok
    return handler


def _qs_delete(device):
    def handler(payload: bytes) -> bytes:
        if not payload:
            return struct.pack('<B', 1)
        idx = payload[0]
        if idx >= SLOT_COUNT or device.qs_slots[idx] is None:
            return struct.pack('<B', 1)  # not found
        device.qs_slots[idx] = None
        return struct.pack('<B', 0)  # existed
    return handler
