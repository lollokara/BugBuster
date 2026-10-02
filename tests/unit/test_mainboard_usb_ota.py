"""C6-26 follow-up: Python sender for mainboard OTA over USB CDC0 (BBP 0x77),
so the S3 / SPIFFS / RP2040 update without WiFi from Python and the MCP."""

import hashlib
import struct

import pytest

from bugbuster import usb_ota
from bugbuster.constants import CmdId


class FakeBB:
    def __init__(self, fail_at=None):
        self.ops = []
        self.written = bytearray()
        self.fail_at = fail_at
        self._t = self

    def _require_usb(self, name):
        pass

    def send_command(self, cmd, payload=b"", timeout=None):
        return self._usb_cmd(cmd, payload)

    def _usb_cmd(self, cmd, payload=b""):
        assert cmd == CmdId.OTA
        op = payload[0]
        self.ops.append(op)
        if op == usb_ota.OP_BEGIN:
            tgt, size, has_sha = struct.unpack_from("<BIB", payload, 1)
            self.size, self.sha = size, payload[7:39]
            assert has_sha == 1
        elif op == usb_ota.OP_CHUNK:
            tgt, off, n, final = struct.unpack_from("<BIHB", payload, 1)
            if self.fail_at is not None and off >= self.fail_at:
                raise TimeoutError("device stopped answering")
            assert off == len(self.written), "offsets must be in order"
            self.written += payload[9:9 + n]
            if final:
                assert len(self.written) == self.size
                assert hashlib.sha256(self.written).digest() == self.sha
        return b"\x01"


def test_esp32_upload_frames_and_sha():
    bb = FakeBB()
    img = bytes(range(256)) * 20
    res = usb_ota.mainboard_usb_ota(bb, img, "esp32")
    assert bytes(bb.written) == img
    assert bb.ops[0] == usb_ota.OP_ABORT and bb.ops[1] == usb_ota.OP_BEGIN
    assert res["ok"] and "reboots" in res["note"]


def test_failure_aborts_the_session():
    bb = FakeBB(fail_at=1920)
    with pytest.raises(TimeoutError):
        usb_ota.mainboard_usb_ota(bb, b"\xAA" * 5000, "spiffs")
    assert bb.ops[-1] == usb_ota.OP_ABORT


def test_rejects_unknown_target():
    with pytest.raises(ValueError):
        usb_ota.mainboard_usb_ota(FakeBB(), b"x", "p4")
