"""Offline OTA for the DAQ HAT over the P4's own USB vendor link (C6-26).

Updates the ESP32-P4 itself or the ESP32-C6 display co-processor without WiFi
or the S3. Wire protocol: ``USB_CMD_OTA_*`` 0x8C-0x93 in
``Firmware/DAQ_HAT/ESP32P4/src/stream/usb_proto.h``; every command is answered
with a ``USB_REC_OTA_ACK`` record (DATA every ``OTA_ACK_WINDOW`` frames).

Flow
----
* C6: BEGIN (target 1) -> DATA... -> END (SHA-256 + merged-image check) ->
  APPLY -> poll STATUS until the push is DONE (~2 min for 1.3 MB).
* P4: BEGIN (target 3) -> DATA... -> END -> REBOOT -> wait for the device to
  re-enumerate -> CONFIRM. An image that is not confirmed rolls back on the
  next reset. The DUT supply (SMU) comes back OFF after a P4 reboot.

The ESP32-S3 mainboard is not a target here: it updates over BBP 0x77 on CDC0.
"""

from __future__ import annotations

import hashlib
import struct
import time
from typing import Callable, Optional

from .daq_stream import (
    CMD_OTA_ABORT,
    CMD_OTA_APPLY,
    CMD_OTA_BEGIN,
    CMD_OTA_CONFIRM,
    CMD_OTA_DATA,
    CMD_OTA_END,
    CMD_OTA_REBOOT,
    CMD_OTA_STATUS,
    OTA_ACK_WINDOW,
    OTA_ERR_OFFSET,
    OTA_ERRORS,
    OTA_TARGET_C6,
    OTA_TARGET_P4,
    DaqStream,
    DaqStreamError,
    OtaAckRecord,
    daq_stream_present,
)

DATA_CHUNK = 500            # device reassembly buffer holds 4 + 508 bytes
PRODUCT_IDS = {OTA_TARGET_C6: b"bb-daq-c6", OTA_TARGET_P4: b"bb-daq-p4"}
RELAY_DONE = 4
RELAY_FAILED = 5

Progress = Callable[[str, int, int], None]


class DaqUsbOtaError(RuntimeError):
    """The device refused or failed an OTA step."""


def _meta(image: bytes, target: int, version_u32: int) -> bytes:
    pid = PRODUCT_IDS[target].ljust(16, b"\0")
    return struct.pack("<II32s16s", len(image), version_u32,
                       hashlib.sha256(image).digest(), pid) + bytes((target,))


def _wait_ack(stream: DaqStream, cmd: int, timeout_s: float) -> OtaAckRecord:
    """Return the next ack for ``cmd``; raise on timeout. Acks for other
    commands (e.g. the device's own per-window DATA acks) are skipped unless
    they report an error."""
    deadline = time.monotonic() + timeout_s
    while time.monotonic() < deadline:
        for rec in stream.read_records(timeout_ms=200):
            if not isinstance(rec, OtaAckRecord):
                continue
            if rec.cmd == cmd:
                return rec
            if rec.status not in (0, OTA_ERR_OFFSET):
                raise DaqUsbOtaError(
                    f"device error on 0x{rec.cmd:02X}: {OTA_ERRORS.get(rec.status, rec.status)}")
    raise DaqUsbOtaError(f"no ack for command 0x{cmd:02X} within {timeout_s:.0f} s")


def _check(ack: OtaAckRecord, what: str) -> OtaAckRecord:
    if ack.status != 0:
        raise DaqUsbOtaError(f"{what}: {OTA_ERRORS.get(ack.status, ack.status)}")
    return ack


def _send_image(stream: DaqStream, image: bytes, progress: Optional[Progress]) -> None:
    size = len(image)
    offset = 0
    while offset < size:
        # One window of DATA frames, then a STATUS whose ack carries the
        # device's resume point (queue order guarantees it covers the window).
        sent = offset
        for _ in range(OTA_ACK_WINDOW):
            if sent >= size:
                break
            chunk = image[sent: sent + DATA_CHUNK]
            stream.send(CMD_OTA_DATA, struct.pack("<I", sent) + chunk)
            sent += len(chunk)
        stream.send(CMD_OTA_STATUS)
        ack = _wait_ack(stream, CMD_OTA_STATUS, 10.0)
        if ack.done_bytes > size or ack.done_bytes < offset:
            raise DaqUsbOtaError(f"device resume point {ack.done_bytes} out of range")
        offset = ack.done_bytes
        if progress:
            progress("upload", offset, size)


def usb_ota_upload(
    image: bytes,
    target: str,
    *,
    version_u32: int = 0,
    progress: Optional[Progress] = None,
    stream_factory: Callable[[], DaqStream] = DaqStream,
    reenum_timeout_s: float = 60.0,
    push_timeout_s: float = 600.0,
) -> dict:
    """Upload ``image`` to the DAQ HAT P4 ("p4") or C6 ("c6") over USB.

    ``image`` for the C6 must be a merged image (bootloader at 0x0, partition
    table at 0x8000); for the P4 the app image. Returns a summary dict.
    """
    tgt = {"p4": OTA_TARGET_P4, "c6": OTA_TARGET_C6}.get(target.lower())
    if tgt is None:
        raise ValueError("target must be 'p4' or 'c6' (the S3 updates over BBP 0x77 on CDC0)")
    if not image:
        raise ValueError("empty image")

    stream = stream_factory()
    stream.open()
    try:
        # Clear a session a crashed host may have left open (no-op otherwise;
        # never touches an S3-driven push).
        stream.send(CMD_OTA_ABORT)
        _wait_ack(stream, CMD_OTA_ABORT, 10.0)
        stream.send(CMD_OTA_BEGIN, _meta(image, tgt, version_u32))
        _check(_wait_ack(stream, CMD_OTA_BEGIN, 60.0), "begin")
        try:
            _send_image(stream, image, progress)
        except Exception:
            stream.send(CMD_OTA_ABORT)  # leave the device clean
            raise
        stream.send(CMD_OTA_END)
        end = _check(_wait_ack(stream, CMD_OTA_END, 120.0), "end/verify")
        result = {"target": target.lower(), "bytes": len(image), "verified": True,
                  "fw_version_before": end.fw_version_str}

        if tgt == OTA_TARGET_C6:
            stream.send(CMD_OTA_APPLY)
            _check(_wait_ack(stream, CMD_OTA_APPLY, 10.0), "apply")
            deadline = time.monotonic() + push_timeout_s
            while True:
                stream.send(CMD_OTA_STATUS)
                st = _wait_ack(stream, CMD_OTA_STATUS, 10.0)
                if progress:
                    progress("push", st.pushed_bytes, st.image_size or len(image))
                if st.state == RELAY_DONE:
                    break
                if st.state == RELAY_FAILED:
                    raise DaqUsbOtaError("C6 push failed (see P4 log)")
                if time.monotonic() > deadline:
                    raise DaqUsbOtaError("C6 push did not finish in time")
                time.sleep(1.0)
            result["applied"] = True
            return result

        # P4 self-update: reboot, wait for re-enumeration, confirm.
        stream.send(CMD_OTA_REBOOT)
        _check(_wait_ack(stream, CMD_OTA_REBOOT, 10.0), "reboot")
    finally:
        stream.close()

    time.sleep(2.0)   # the device is gone; do not race its re-enumeration
    deadline = time.monotonic() + reenum_timeout_s
    while not daq_stream_present():
        if time.monotonic() > deadline:
            raise DaqUsbOtaError("P4 did not re-enumerate after reboot; it will roll back on the next reset if unconfirmed")
        time.sleep(0.5)
    time.sleep(1.0)

    stream = stream_factory()
    for _ in range(10):
        try:
            stream.open()
            break
        except DaqStreamError:
            time.sleep(1.0)
    else:
        raise DaqUsbOtaError("could not reopen the DAQ data plane after reboot")
    try:
        stream.send(CMD_OTA_STATUS)
        st = _wait_ack(stream, CMD_OTA_STATUS, 10.0)
        result["fw_version_after"] = st.fw_version_str
        result["pending_verify_after_reboot"] = st.pending_verify
        if st.pending_verify:
            stream.send(CMD_OTA_CONFIRM)
            _check(_wait_ack(stream, CMD_OTA_CONFIRM, 10.0), "confirm")
            stream.send(CMD_OTA_STATUS)
            st = _wait_ack(stream, CMD_OTA_STATUS, 10.0)
        result["confirmed"] = not st.pending_verify
        result["warnings"] = ["P4 rebooted: the DUT supply (SMU) is OFF until re-enabled."]
        return result
    finally:
        stream.close()
