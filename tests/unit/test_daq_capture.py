"""Unit tests for the incremental frame parser and Capture accumulator.

Frames are synthesised byte-for-byte, so these tests pin the exact wire
behaviour the hardware tests later depend on -- including the two behaviours
that have caused real bugs: false-positive resync on magic bytes inside float
payloads, and seq-gap accounting.
"""
import struct

import pytest

from tests.lib import daq_proto as P
from tests.lib import daq_capture as C


def make_frame(typ, seq, payload):
    body = (bytes([P.PROTO_VERSION, typ, 0, 0])
            + struct.pack("<I", seq)
            + struct.pack("<H", len(payload))
            + payload)
    return bytes([P.MAGIC0, P.MAGIC1]) + body + b"\x00\x00"


def wave_i(seq, start_index, count, value=1.0):
    hdr = struct.pack("<QQIHBB", start_index, 0, 64000, count, 1, 0)
    return make_frame(P.REC_WAVE_I, seq,
                      hdr + struct.pack("<%df" % count, *([value] * count)) + bytes(count))


def test_parses_two_frames_from_one_chunk():
    cap = C.Capture()
    p = C.FrameParser(cap.on_record)
    p.feed(wave_i(0, 0, 2) + wave_i(1, 2, 2))
    assert cap.frames["WAVE_I"] == 2
    assert cap.wave_i_samples == 4


def test_parses_frame_split_across_chunks():
    cap = C.Capture()
    p = C.FrameParser(cap.on_record)
    data = wave_i(0, 0, 4)
    p.feed(data[:7])
    assert cap.frames.get("WAVE_I", 0) == 0
    p.feed(data[7:])
    assert cap.frames["WAVE_I"] == 1
    assert cap.wave_i_samples == 4


def test_magic_inside_payload_does_not_cause_a_false_frame():
    # 0xBB 0x50 appears inside the float payload; the parser must not treat it
    # as a header, because version/type/length would not validate.
    hdr = struct.pack("<QQIHBB", 0, 0, 64000, 2, 1, 0)
    payload = hdr + b"\xbb\x50\xbb\x50" + b"\x00\x00\x00\x00" + bytes(2)
    cap = C.Capture()
    p = C.FrameParser(cap.on_record)
    p.feed(make_frame(P.REC_WAVE_I, 0, payload))
    assert cap.frames["WAVE_I"] == 1
    assert p.resyncs == 0


def test_garbage_prefix_is_resynced_past():
    cap = C.Capture()
    p = C.FrameParser(cap.on_record)
    p.feed(b"\xbb\x99garbage" + wave_i(0, 0, 1))
    assert cap.frames["WAVE_I"] == 1
    assert p.resyncs >= 1


def test_seq_gap_is_counted_and_attributed():
    cap = C.Capture()
    p = C.FrameParser(cap.on_record)
    p.feed(wave_i(10, 0, 1) + wave_i(13, 1, 1))
    assert cap.seq_gaps == 1
    # Frames 11 and 12 never arrived: two frames lost, not the raw seq jump of 3.
    assert cap.seq_lost == 2
    assert cap.seq_first == 10
    assert cap.seq_last == 13


def test_no_gap_when_sequence_is_contiguous():
    cap = C.Capture()
    p = C.FrameParser(cap.on_record)
    for s in range(5):
        p.feed(wave_i(s, s, 1))
    assert cap.seq_gaps == 0
    assert cap.seq_lost == 0


def test_seq_wraps_at_32_bits_without_reporting_loss():
    cap = C.Capture()
    p = C.FrameParser(cap.on_record)
    p.feed(wave_i(0xFFFFFFFF, 0, 1) + wave_i(0, 1, 1))
    assert cap.seq_gaps == 0


def test_contiguous_wave_i_detects_a_sample_index_gap():
    cap = C.Capture()
    p = C.FrameParser(cap.on_record)
    p.feed(wave_i(0, 0, 10) + wave_i(1, 10, 10))
    assert cap.contiguous_wave_i is True

    cap2 = C.Capture()
    p2 = C.FrameParser(cap2.on_record)
    p2.feed(wave_i(0, 0, 10) + wave_i(1, 25, 10))   # 5-sample hole
    assert cap2.contiguous_wave_i is False


def test_rates_are_computed_from_the_capture_window():
    cap = C.Capture()
    p = C.FrameParser(cap.on_record)
    p.feed(wave_i(0, 0, 1000))
    cap.seconds = 2.0
    assert cap.wave_i_sps == pytest.approx(500.0)


def test_last_status_returns_the_most_recent_status_dict():
    cap = C.Capture()
    p = C.FrameParser(cap.on_record)
    s1 = struct.pack("<IIBBBB", 1000, 0, P.RANGE_HI, 1, 0, 0) + b"\x00" * 8
    s2 = struct.pack("<IIBBBB", 2000, 0, P.RANGE_LO, 1, 0, 1) + b"\x00" * 8
    p.feed(make_frame(P.REC_STATUS, 0, s1) + make_frame(P.REC_STATUS, 1, s2))
    assert cap.last_status["sample_rate"] == 2000
    assert cap.last_status["range"] == P.RANGE_LO


def test_oversized_length_is_rejected_as_a_header():
    cap = C.Capture()
    p = C.FrameParser(cap.on_record)
    bogus = (bytes([P.MAGIC0, P.MAGIC1, P.PROTO_VERSION, P.REC_WAVE_I, 0, 0])
             + struct.pack("<I", 0) + struct.pack("<H", P.MAX_PAYLOAD + 1))
    p.feed(bogus + wave_i(0, 0, 1))
    assert cap.frames["WAVE_I"] == 1
    assert p.resyncs >= 1
