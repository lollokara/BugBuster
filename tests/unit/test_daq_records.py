# tests/unit/test_daq_records.py
"""Unit tests for DAQ record decoding, driven by synthetic payloads built to
the exact struct layouts in usb_proto.h. No hardware.
"""
import struct

import pytest

from tests.lib import daq_proto as P
from tests.lib import daq_records as R


def _wave_payload(count, values, metas=None, start_index=0, ts=0, rate=64000, decim=1):
    hdr = struct.pack("<QQIHBB", start_index, ts, rate, count, decim, 0)
    body = struct.pack("<%df" % count, *values)
    if metas is not None:
        body += bytes(metas)
    return hdr + body


def test_wave_i_decodes_header_samples_and_meta():
    rec = R.decode(P.REC_WAVE_I, _wave_payload(3, [1.0, 2.0, 3.0], [0x01, 0x02, 0x11],
                                               start_index=99, ts=12345, rate=64000))
    assert isinstance(rec, R.WaveI)
    assert rec.start_index == 99
    assert rec.timestamp_us == 12345
    assert rec.sample_rate == 64000
    assert rec.count == 3
    assert rec.decimation == 1
    assert rec.samples == (1.0, 2.0, 3.0)
    assert rec.meta == bytes([0x01, 0x02, 0x11])


def test_wave_v_has_no_meta_array():
    rec = R.decode(P.REC_WAVE_V, _wave_payload(2, [5.0, 5.5]))
    assert isinstance(rec, R.WaveV)
    assert rec.samples == (5.0, 5.5)
    assert rec.decimation == 1


def test_wave_i_rejects_truncated_payload():
    # count says 4 samples but only 2 floats + 0 meta bytes are present
    bad = struct.pack("<QQIHBB", 0, 0, 64000, 4, 1, 0) + struct.pack("<2f", 1.0, 2.0)
    assert R.decode(P.REC_WAVE_I, bad) is None


def test_meta_bit_helpers():
    m = 0b00_1_0_10_01  # settling=0, saturated=1, source=2, range=1
    m = (1 << 4) | (2 << 2) | 1
    assert R.meta_range(m) == 1
    assert R.meta_source(m) == 2
    assert R.meta_saturated(m) is True
    assert R.meta_settling(m) is False
    assert R.meta_settling(1 << 5) is True


def test_stats_decodes_three_blocks():
    blocks = b""
    for base in (1.0, 2.0, 3.0):
        blocks += struct.pack("<fffffI", base, base + 9, base + 1, base + 2, base + 3, 100)
    rec = R.decode(P.REC_STATS, blocks)
    assert isinstance(rec, R.Stats)
    assert rec.i.min == pytest.approx(1.0)
    assert rec.i.max == pytest.approx(10.0)
    assert rec.v.mean == pytest.approx(3.0)
    assert rec.p.count == 100


def test_energy_decodes_doubles_then_floats():
    payload = struct.pack("<ddddd", 1.5, 2.5, 3.5, 4.5, 5.5) + struct.pack("<fff", 0.1, 5.0, 0.5)
    rec = R.decode(P.REC_ENERGY, payload)
    assert isinstance(rec, R.Energy)
    assert rec.energy_mwh == pytest.approx(1.5)
    assert rec.energy_j == pytest.approx(2.5)
    assert rec.charge_mah == pytest.approx(3.5)
    assert rec.charge_c == pytest.approx(4.5)
    assert rec.elapsed_s == pytest.approx(5.5)
    assert rec.last_v == pytest.approx(5.0)


def test_marker_decodes_20_bytes():
    payload = struct.pack("<QQBBBB", 777, 888, 3, 1, 1, 0)
    rec = R.decode(P.REC_MARKER, payload)
    assert isinstance(rec, R.Marker)
    assert rec.sample_index == 777
    assert rec.channel == 3
    assert rec.edge == 1
    assert rec.kind == 1


def test_fft_decodes_header_and_bins():
    payload = struct.pack("<IHBB", 64000, 4, 0, 1) + struct.pack("<4f", 1.0, 2.0, 3.0, 4.0)
    rec = R.decode(P.REC_FFT, payload)
    assert isinstance(rec, R.Fft)
    assert rec.nbins == 4
    assert rec.window == 1
    assert rec.bins == (1.0, 2.0, 3.0, 4.0)


def test_status_degrades_gracefully_on_short_payload():
    short = struct.pack("<IIBBBB", 64000, 0, P.RANGE_MID, 1, 0, 1) + b"\x00" * 8
    d = R.decode(P.REC_STATUS, short)
    assert d["sample_rate"] == 64000
    assert d["range"] == P.RANGE_MID
    assert d["streaming"] == 1
    assert "frames_tx" not in d


def test_status_full_96_bytes_decodes_perf_counters():
    p = bytearray(96)
    struct.pack_into("<II", p, 0, 64000, 0)
    struct.pack_into("<BBBB", p, 8, P.RANGE_LO, 1, 0, 1)
    struct.pack_into("<ff", p, 12, 5.0, 0.5)
    struct.pack_into("<IIIII", p, 36, 111, 222, 0, 4096, 9999)
    struct.pack_into("<BBHI", p, 88, 2, 3, 8, 64000000)
    d = R.decode(P.REC_STATUS, bytes(p))
    assert d["vdut_set"] == pytest.approx(5.0)
    assert d["ilimit_set"] == pytest.approx(0.5)
    assert d["frames_tx"] == 111
    assert d["bytes_per_sec"] == 222
    assert d["fifo_drop_frames"] == 0
    assert d["ring_high_water"] == 4096
    assert d["stream_decim"] == 8
    assert d["odr_mhz"] == 64000000
