"""Offline OTA for the mainboard over USB CDC0 (BBP 0x77), C6-26 follow-up.

Targets: the ESP32-S3 application, the on-device web UI (SPIFFS image) and
the RP2040 HAT (staged on the S3, then flashed by it). No WiFi needed. The DAQ
HAT P4 and C6 update over their own USB link, see ``bugbuster.daq_usb_ota``.

Wire format (``Firmware/ESP32/src/bbp/cmds/cmd_ota.cpp``), little endian:
  BEGIN 0x10: u8 target, u32 size, bool has_sha, [32 B sha256]
  CHUNK 0x11: u8 target, u32 offset, u16 len, bool final, bytes
  ABORT 0x12
Every op is acknowledged by the BBP response itself; offsets must be in order.
An ESP32 image reboots the S3 one second after the final chunk.
"""

from __future__ import annotations

import hashlib
import struct
from typing import Any, Callable, Optional

from .constants import CmdId

TARGETS = {"esp32": 0x00, "rp2040": 0x01, "spiffs": 0x02}
OP_BEGIN, OP_CHUNK, OP_ABORT = 0x10, 0x11, 0x12
CHUNK = 960          # same as the desktop app (commands.rs USB_OTA_CHUNK_SIZE)
BEGIN_TIMEOUT_S = 90.0
FINAL_TIMEOUT_S = 120.0   # RP2040: the S3 flashes the HAT before answering

Progress = Callable[[int, int], None]


def mainboard_usb_ota(bb: Any, image: bytes, target: str = "esp32",
                      progress: Optional[Progress] = None) -> dict:
    """Upload ``image`` to the mainboard over the USB BBP link of ``bb``."""
    tgt = TARGETS.get(target.lower())
    if tgt is None:
        raise ValueError(f"target must be one of {sorted(TARGETS)}")
    if not image:
        raise ValueError("empty image")
    bb._require_usb("mainboard_usb_ota")   # this is the no-WiFi path

    def send(payload: bytes, timeout: Optional[float] = None) -> bytes:
        return bb._t.send_command(CmdId.OTA, payload, timeout=timeout)

    sha = hashlib.sha256(image).digest()
    send(bytes((OP_ABORT,)))                                          # clear a stale session
    # BEGIN erases synchronously: the whole 4 MB SPIFFS partition takes ~30 s.
    send(struct.pack("<BBIB", OP_BEGIN, tgt, len(image), 1) + sha, timeout=BEGIN_TIMEOUT_S)
    off = 0
    try:
        while off < len(image):
            chunk = image[off: off + CHUNK]
            final = off + len(chunk) >= len(image)
            send(struct.pack("<BBIHB", OP_CHUNK, tgt, off, len(chunk), int(final)) + chunk,
                 timeout=FINAL_TIMEOUT_S if final else None)
            off += len(chunk)
            if progress:
                progress(off, len(image))
    except Exception:
        try:
            send(bytes((OP_ABORT,)))
        except Exception:
            pass
        raise
    result = {"target": target.lower(), "bytes": len(image), "sha256": sha.hex(), "ok": True}
    if tgt == TARGETS["esp32"]:
        result["note"] = ("The S3 reboots in ~1 s; the USB link re-enumerates. "
                          "Use reset_link / reconnect, then check ota_get_info.")
    return result
