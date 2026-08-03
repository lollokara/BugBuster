"""Unit tests for DaqLink command construction and the safety contract.

Driven through FakeIO, so no hardware: we assert on the exact bytes DaqLink
would put on the wire. The payload layouts are pinned against usb_proto.h.
"""
import struct

import pytest

from tests.lib import daq_proto as P
from tests.lib.daq_link import DaqLink, FakeIO


def sent_frames(io):
    """Split FakeIO's write log into (cmd, payload) pairs."""
    out = []
    for w in io.writes:
        assert w[0] == P.MAGIC0 and w[1] == P.MAGIC1
        plen = struct.unpack_from("<H", w, 10)[0]
        out.append((w[3], w[P.HDR_LEN:P.HDR_LEN + plen]))
    return out


def test_start_and_stop_emit_empty_control_frames():
    io = FakeIO()
    link = DaqLink(io)
    link.start()
    link.stop()
    assert sent_frames(io) == [(P.CMD_START, b""), (P.CMD_STOP, b"")]


def test_set_rate_payload_matches_usb_cmd_rate_t():
    io = FakeIO()
    DaqLink(io).set_rate(64000, 64000, 8)
    cmd, payload = sent_frames(io)[0]
    assert cmd == P.CMD_SET_RATE
    cur, volt, decim = struct.unpack_from("<IIB", payload, 0)
    assert (cur, volt, decim) == (64000, 64000, 8)
    assert len(payload) == 12   # 4 + 4 + 1 + 3 pad


def test_set_source_payload_matches_usb_cmd_source_t():
    io = FakeIO()
    DaqLink(io).set_source(5.0, 0.5, True)
    cmd, payload = sent_frames(io)[0]
    assert cmd == P.CMD_SET_SOURCE
    vdut, ilimit, enable = struct.unpack_from("<ffB", payload, 0)
    assert vdut == pytest.approx(5.0)
    assert ilimit == pytest.approx(0.5)
    assert enable == 1
    assert len(payload) == 12   # 4 + 4 + 1 + 3 pad


def test_range_lock_and_auto():
    io = FakeIO()
    link = DaqLink(io)
    link.set_range_lock(P.RANGE_MID)
    link.set_range_lock(P.RANGE_AUTO)
    frames = sent_frames(io)
    assert frames[0] == (P.CMD_RANGE_LOCK, bytes([P.RANGE_MID]))
    assert frames[1] == (P.CMD_RANGE_LOCK, bytes([P.RANGE_AUTO]))


def test_set_fft_payload_matches_usb_cmd_fft_t():
    io = FakeIO()
    DaqLink(io).set_fft(nbins=512, source=0, window=1, enabled=True)
    cmd, payload = sent_frames(io)[0]
    assert cmd == P.CMD_FFT_CONFIG
    nbins, source, window, enabled = struct.unpack_from("<HBBB", payload, 0)
    assert (nbins, source, window, enabled) == (512, 0, 1, 1)
    assert len(payload) == 8


def test_arm_payload_matches_usb_cmd_arm_t():
    io = FakeIO()
    DaqLink(io).arm(True, trig_logic=1, pre_samples=4096)
    cmd, payload = sent_frames(io)[0]
    assert cmd == P.CMD_ARM
    armed, logic, _pad, pre = struct.unpack_from("<BBHI", payload, 0)
    assert (armed, logic, pre) == (1, 1, 4096)
    assert len(payload) == 8


def test_reset_commands_are_empty():
    io = FakeIO()
    link = DaqLink(io)
    link.reset_energy()
    link.reset_stats()
    assert sent_frames(io) == [(P.CMD_RESET_ENERGY, b""), (P.CMD_RESET_STATS, b"")]


def test_collect_returns_a_capture_with_records_and_bytes():
    from tests.unit.test_daq_capture import wave_i
    io = FakeIO(reads=[wave_i(0, 0, 4), wave_i(1, 4, 4), b""])
    cap = DaqLink(io).collect(seconds=0.0)
    assert cap.frames["WAVE_I"] == 2
    assert cap.wave_i_samples == 8
    assert cap.bytes_rx > 0


def test_safe_state_stops_disables_smu_and_unlocks_range():
    io = FakeIO()
    DaqLink(io).safe_state()
    cmds = [c for c, _ in sent_frames(io)]
    assert cmds == [P.CMD_STOP, P.CMD_SET_SOURCE, P.CMD_RANGE_LOCK]
    _, src = sent_frames(io)[1]
    assert struct.unpack_from("<ffB", src, 0)[2] == 0     # enable = 0
    assert sent_frames(io)[2][1] == bytes([P.RANGE_AUTO])


def test_safe_state_never_raises_on_a_broken_link():
    class Broken(FakeIO):
        def write(self, data):
            raise OSError("pipe is gone")

    DaqLink(Broken()).safe_state()   # must not raise


def test_context_manager_calls_safe_state_and_close():
    io = FakeIO()
    with DaqLink(io) as link:
        link.start()
    assert io.closed is True
    cmds = [c for c, _ in sent_frames(io)]
    assert P.CMD_STOP in cmds
