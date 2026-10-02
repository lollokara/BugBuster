"""C6-26: host sender for OTA over the DAQ HAT vendor link, against a fake P4
that implements the firmware's ack/resume rules (usb_ota_handle in daq_board.c)."""

import hashlib
import struct

import pytest

from bugbuster import daq_stream as ds
from bugbuster import daq_usb_ota as uota


class FakeP4:
    """Minimal model of the P4 OTA worker. ``drop`` = DATA frame numbers to lose."""

    def __init__(self, drop=(), pending_after_reboot=True, fail_end=False):
        self.drop = set(drop)
        self.frames = 0
        self.target = 0
        self.image = bytearray()
        self.size = 0
        self.sha = b""
        self.relay_state = 0
        self.pushed = 0
        self.pending = False
        self.pending_after_reboot = pending_after_reboot
        self.fail_end = fail_end
        self.rebooted = False
        self.confirmed = False
        self.q = []

    def ack(self, cmd, status=0):
        done = len(self.image) if self.target or self.relay_state else 0
        tgt = self.target or (1 if self.relay_state else 0)
        self.q.append(ds.OtaAckRecord(cmd, status, tgt, self.relay_state, done, self.size,
                                      self.pushed, 0x020403, 1 if self.pending else 0))

    def handle(self, cmd, p):
        if cmd == ds.CMD_OTA_ABORT:
            self.target = 0
            return self.ack(cmd)
        if cmd == ds.CMD_OTA_BEGIN:
            size, _ver, sha, pid = struct.unpack_from("<II32s16s", p, 0)
            tgt = p[56]
            if pid.rstrip(b"\0") != uota.PRODUCT_IDS[tgt]:
                return self.ack(cmd, -3)
            self.target, self.size, self.sha, self.image = tgt, size, sha, bytearray()
            self.relay_state = 1 if tgt == 1 else 0
            return self.ack(cmd)
        if cmd == ds.CMD_OTA_DATA:
            self.frames += 1
            if self.frames in self.drop:
                return None
            off = struct.unpack_from("<I", p, 0)[0]
            if off != len(self.image):
                return self.ack(cmd, ds.OTA_ERR_OFFSET)
            self.image += p[4:]
            if self.frames % ds.OTA_ACK_WINDOW == 0:
                self.ack(cmd)
            return None
        if cmd == ds.CMD_OTA_STATUS:
            if self.relay_state == 3:            # pushing: finish on the next poll
                self.pushed, self.relay_state = self.size, 4
            return self.ack(cmd)
        if cmd == ds.CMD_OTA_END:
            ok = (len(self.image) == self.size and hashlib.sha256(self.image).digest() == self.sha
                  and not self.fail_end)
            if self.target == 1:
                self.relay_state = 2 if ok else 5
            st = 0 if ok else -6
            self.ack(cmd, st)
            self.target = 0
            return None
        if cmd == ds.CMD_OTA_APPLY:
            if self.relay_state != 2:
                return self.ack(cmd, -7)
            self.relay_state = 3
            return self.ack(cmd)
        if cmd == ds.CMD_OTA_REBOOT:
            self.ack(cmd)
            self.rebooted = True
            self.pending = self.pending_after_reboot
            return None
        if cmd == ds.CMD_OTA_CONFIRM:
            self.pending = False
            self.confirmed = True
            return self.ack(cmd)
        return self.ack(cmd, -1)


class FakeStream:
    def __init__(self, dev):
        self.dev = dev

    def open(self):
        return self

    def close(self):
        pass

    def send(self, cmd, payload=b""):
        self.dev.handle(cmd, bytes(payload))

    def read_records(self, timeout_ms=200):
        out, self.dev.q = self.dev.q, []
        return out


@pytest.fixture(autouse=True)
def _fast(monkeypatch):
    monkeypatch.setattr(uota.time, "sleep", lambda s: None)
    monkeypatch.setattr(uota, "daq_stream_present", lambda: True)


def _image(n=20_000):
    return bytes((i * 7) & 0xFF for i in range(n))


def test_c6_upload_stage_apply_and_push():
    dev = FakeP4()
    img = _image()
    seen = []
    res = uota.usb_ota_upload(img, "c6", stream_factory=lambda: FakeStream(dev),
                              progress=lambda s, d, t: seen.append(s))
    assert bytes(dev.image) == img
    assert res["applied"] is True and dev.relay_state == 4
    assert "push" in seen


def test_resumes_after_a_lost_data_frame():
    dev = FakeP4(drop={3, 40})
    img = _image(30_000)
    uota.usb_ota_upload(img, "c6", stream_factory=lambda: FakeStream(dev))
    assert bytes(dev.image) == img


def test_p4_reboots_and_confirms():
    dev = FakeP4()
    res = uota.usb_ota_upload(_image(), "p4", stream_factory=lambda: FakeStream(dev))
    assert dev.rebooted and dev.confirmed and res["confirmed"] is True
    assert "SMU" in res["warnings"][0]


def test_verify_failure_is_reported():
    dev = FakeP4(fail_end=True)
    with pytest.raises(uota.DaqUsbOtaError, match="verify"):
        uota.usb_ota_upload(_image(), "c6", stream_factory=lambda: FakeStream(dev))


def test_s3_is_not_a_target():
    with pytest.raises(ValueError, match="BBP 0x77"):
        uota.usb_ota_upload(b"x", "s3")
