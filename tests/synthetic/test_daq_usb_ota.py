"""C6-26: OTA over the P4's own USB vendor link (0x8C-0x93) - kept and fixed
instead of retired, so the DAQ HAT can be updated without WiFi.

A (before): no ack channel (handlers only logged), the C6 apply step needed the
P4 debug console, no P4 self-target, the S3 target staged images nobody
consumed, and all flash work ran on the TinyUSB task.
"""

import re
import struct
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[2]
P4 = ROOT / "Firmware" / "DAQ_HAT" / "ESP32P4" / "src"
PROTO = P4 / "stream" / "usb_proto.h"
BOARD = P4 / "board" / "daq_board.c"


def _usb_cmd_handler(src: str) -> str:
    start = src.index("static void usb_cmd_handler(")
    return src[start: src.index("\n}\n", start)]


@pytest.mark.xfail(strict=True, reason="C6-26")
def test_proto_defines_ack_and_new_commands():
    h = PROTO.read_text(encoding="utf-8")
    for name, val in [("USB_REC_OTA_ACK", "0x08"), ("USB_CMD_OTA_APPLY", "0x90"),
                      ("USB_CMD_OTA_CONFIRM", "0x91"), ("USB_CMD_OTA_REBOOT", "0x92"),
                      ("USB_CMD_OTA_STATUS", "0x93")]:
        assert re.search(rf"{name}\s*=\s*{val}", h), name
    assert "usb_ota_ack_t" in h and "USB_OTA_TARGET_P4" in h


@pytest.mark.xfail(strict=True, reason="C6-26")
def test_tinyusb_handler_only_enqueues_ota_work():
    body = _usb_cmd_handler(BOARD.read_text(encoding="utf-8"))
    assert "usb_ota_enqueue" in body
    for forbidden in ("relay_stage_begin", "relay_stage_write", "relay_stage_end",
                      "ota_begin(", "ota_write(", "ota_end("):
        assert forbidden not in body, forbidden


@pytest.mark.xfail(strict=True, reason="C6-26")
def test_worker_has_internal_stack_and_answers_every_command():
    src = BOARD.read_text(encoding="utf-8")
    m = re.search(r"xTaskCreatePinnedToCoreWithCaps\(usb_ota_task[^;]*MALLOC_CAP_INTERNAL", src, re.S)
    assert m, "usb_ota worker must use an internal-RAM stack"
    handle = src[src.index("static void usb_ota_handle("):]
    handle = handle[: handle.index("\nstatic void usb_ota_task(")]
    for cmd in ("BEGIN", "DATA", "END", "ABORT", "APPLY", "CONFIRM", "REBOOT", "STATUS"):
        assert f"USB_CMD_OTA_{cmd}" in handle, cmd
    assert "USB_OTA_TARGET_P4" in handle and "ota_begin(" in handle
    assert "relay_apply_start(b)" in handle
    # The C6 image must be a merged image before it can be pushed to offset 0.
    assert "usb_ota_c6_staged_is_merged" in handle


@pytest.mark.xfail(strict=True, reason="C6-26")
def test_python_decoder_parses_ota_ack():
    from bugbuster import daq_stream as ds

    payload = struct.pack("<BbBBIIIIB3x", 0x8E, -6, 1, 5, 1000, 2000, 0, 0x00020403, 1)
    frame = ds.build_frame(0x08, payload)
    rec, consumed = ds.parse_frame(frame, 0)
    assert consumed == len(frame)
    assert isinstance(rec, ds.OtaAckRecord)
    assert (rec.cmd, rec.status, rec.target, rec.done_bytes, rec.image_size) == (0x8E, -6, 1, 1000, 2000)
    assert rec.pending_verify is True
