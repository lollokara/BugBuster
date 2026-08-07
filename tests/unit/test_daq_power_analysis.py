"""Unit tests for the host-side DAQ power data plane: frame decoding,
capture assembly, and the post-processing in bugbuster.power_analysis."""
import math
import struct

import pytest

from bugbuster.daq_stream import (
    CaptureAccumulator, EnergyRecord, MarkerRecord, PowerCapture, StatusRecord,
    WaveIRecord, WaveVRecord, build_frame, crc16_ccitt, parse_frame,
    FRAME_HEADER_LEN, MARK_KIND_FLAG, MARK_KIND_TRIGGER, PROTO_MAGIC0,
    PROTO_MAGIC1, PROTO_VERSION, REC_ENERGY, REC_MARKER, REC_STATUS,
    REC_WAVE_I, REC_WAVE_V,
)
from bugbuster.power_analysis import (
    analyze, battery_life, detect_periodicity, detect_states, integrate,
    meta_quality, preview, segment_by_markers,
)


# ---------------------------------------------------------------------------
# Frame helpers
# ---------------------------------------------------------------------------
def _data_frame(rec_type: int, payload: bytes, seq: int = 0) -> bytes:
    head = struct.pack("<BBBBBBIH", PROTO_MAGIC0, PROTO_MAGIC1, PROTO_VERSION,
                       rec_type, 0, 0, seq, len(payload))
    return head + payload + b"\x00\x00"


def _wave_hdr(start, ts, rate, count, decim=1):
    return struct.pack("<QQIHBB", start, ts, rate, count, decim, 0)


def _wave_i(start, values, rate=1000, ts=0, meta=None):
    meta = meta if meta is not None else bytes(len(values))
    p = (_wave_hdr(start, ts, rate, len(values))
         + struct.pack(f"<{len(values)}f", *values) + meta)
    return _data_frame(REC_WAVE_I, p)


def _wave_v(start, values, rate=1000, ts=0):
    p = _wave_hdr(start, ts, rate, len(values)) + struct.pack(
        f"<{len(values)}f", *values)
    return _data_frame(REC_WAVE_V, p)


# ---------------------------------------------------------------------------
# Decoding
# ---------------------------------------------------------------------------
def test_parse_wave_i():
    frame = _wave_i(100, [0.001, 0.002, 0.5], rate=250000, ts=42,
                    meta=bytes([0x00, 0x11, 0x22]))
    rec, consumed = parse_frame(frame)
    assert consumed == len(frame)
    assert isinstance(rec, WaveIRecord)
    assert rec.start_index == 100
    assert rec.timestamp_us == 42
    assert rec.sample_rate == 250000
    assert rec.meta == bytes([0x00, 0x11, 0x22])
    assert rec.current == pytest.approx([0.001, 0.002, 0.5])


def test_parse_wave_v():
    frame = _wave_v(7, [3.3, 3.29])
    rec, consumed = parse_frame(frame)
    assert consumed == len(frame)
    assert isinstance(rec, WaveVRecord)
    assert rec.voltage == pytest.approx([3.3, 3.29])


def test_parse_marker_and_energy():
    mk = _data_frame(REC_MARKER, struct.pack("<QQBBBB", 900, 1234, 5, 1,
                                             MARK_KIND_TRIGGER, 0))
    rec, _ = parse_frame(mk)
    assert isinstance(rec, MarkerRecord)
    assert rec.as_dict() == {"sample_index": 900, "timestamp_us": 1234,
                             "channel": 5, "edge": "rising", "kind": "trigger"}

    en = _data_frame(REC_ENERGY, struct.pack("<dddddfff", 1.0, 3.6, 2.0, 7.2,
                                             10.0, 0.1, 3.3, 0.33))
    rec, _ = parse_frame(en)
    assert isinstance(rec, EnergyRecord)
    assert rec.energy_j == pytest.approx(3.6)


def test_parse_status_extensions_are_optional():
    base = struct.pack("<IIBBBBff", 250000, 0, 1, 1, 0, 1, 3.3, 0.5)
    rec, _ = parse_frame(_data_frame(REC_STATUS, base))
    assert isinstance(rec, StatusRecord)
    assert rec.raw["sample_rate"] == 250000
    assert rec.raw["range"] == "mid"
    assert "board_temp_analog_c" not in rec.raw

    full = base + bytes(100 - len(base))
    rec, _ = parse_frame(_data_frame(REC_STATUS, full))
    assert "board_temp_analog_c" in rec.raw


def test_parse_needs_more_bytes():
    frame = _wave_i(0, [1.0, 2.0])
    rec, consumed = parse_frame(frame[:FRAME_HEADER_LEN + 4])
    assert rec is None and consumed == 0


def test_parse_resyncs_past_garbage():
    frame = _wave_i(0, [1.0])
    stream = b"\xde\xad\xbe\xef" * 4 + frame
    off = 0
    recs = []
    while off < len(stream):
        rec, consumed = parse_frame(stream, off)
        if consumed == 0:
            break
        off += consumed
        if rec is not None:
            recs.append(rec)
    assert len(recs) == 1 and isinstance(recs[0], WaveIRecord)


def test_control_frame_carries_real_crc():
    frame = build_frame(0x80, b"", seq=3)
    body = frame[2:-2]
    assert struct.unpack("<H", frame[-2:])[0] == crc16_ccitt(body)


# ---------------------------------------------------------------------------
# Capture assembly
# ---------------------------------------------------------------------------
def test_accumulator_holds_voltage_onto_current_timebase():
    acc = CaptureAccumulator()
    acc.feed(WaveVRecord(0, 0, 100, [3.3]))
    acc.feed(WaveIRecord(0, 0, 1000, 1, [0.1, 0.2, 0.3, 0.4], bytes(4)))
    cap = acc.finish()
    assert cap.sample_count == 4
    assert cap.voltage == [3.3, 3.3, 3.3, 3.3]
    assert cap.sample_rate == 1000


def test_accumulator_fills_index_gaps_with_nan():
    acc = CaptureAccumulator()
    acc.feed(WaveIRecord(0, 0, 1000, 1, [0.1, 0.1], bytes(2)))
    acc.feed(WaveIRecord(5, 5000, 1000, 1, [0.2], bytes(1)))
    cap = acc.finish()
    assert cap.sample_count == 6
    assert cap.dropped_samples == 3
    assert all(math.isnan(x) for x in cap.current[2:5])


def test_accumulator_respects_max_samples():
    acc = CaptureAccumulator(max_samples=3)
    acc.feed(WaveIRecord(0, 0, 1000, 1, [0.1] * 10, bytes(10)))
    assert acc.finish().sample_count == 3
    assert acc.full


def test_decimation_scales_the_reported_rate():
    acc = CaptureAccumulator()
    acc.feed(WaveIRecord(0, 0, 250000, 10, [0.1, 0.2], bytes(2)))
    assert acc.finish().sample_rate == 25000


# ---------------------------------------------------------------------------
# Analysis
# ---------------------------------------------------------------------------
def _capture(current, voltage=None, rate=1000.0, meta=None, markers=None):
    n = len(current)
    return PowerCapture(
        sample_rate=rate,
        current=list(current),
        voltage=list(voltage if voltage is not None else [1.0] * n),
        meta=bytearray(meta if meta is not None else bytes(n)),
        markers=list(markers or []),
        start_index=0,
        start_timestamp_us=0,
    )


def test_integrate_constant_load():
    # 1 A at 1 V for 1 s (trapezoid over 1001 points) = 1 J, 1 C.
    n = 1001
    res = integrate([1.0] * n, [1.0] * n, 1000.0)
    assert res["energy_j"] == pytest.approx(1.0)
    assert res["charge_c"] == pytest.approx(1.0)
    assert res["energy_mwh"] == pytest.approx(1.0 / 3.6)
    assert res["charge_mah"] == pytest.approx(1.0 / 3.6)
    assert res["current_mean_a"] == pytest.approx(1.0)
    assert res["current_rms_a"] == pytest.approx(1.0)
    assert res["power_mean_w"] == pytest.approx(1.0)


def test_integrate_skips_nan_gaps():
    res = integrate([1.0, float("nan"), 1.0], [1.0, 1.0, 1.0], 1000.0)
    assert res["valid_samples"] == 2
    assert res["skipped_samples"] == 1
    # The two valid samples are not adjacent, so no trapezoid spans the gap.
    assert res["energy_j"] == pytest.approx(0.0)


def test_integrate_empty():
    assert integrate([], [], 1000.0)["valid_samples"] == 0


def test_battery_life_projection():
    # 1 mAh drawn over 1 hour = 1 mA average; a 100 mAh cell lasts 100 h.
    res = battery_life(charge_mah=1.0, duration_s=3600.0, capacity_mah=100.0)
    assert res["average_current_ma"] == pytest.approx(1.0)
    assert res["estimated_hours"] == pytest.approx(100.0)


def test_detect_states_separates_sleep_and_active():
    # 1 uA sleep / 20 mA active, alternating 100 ms blocks at 1 ksps.
    current = []
    for _ in range(5):
        current += [1e-6] * 100
        current += [20e-3] * 100
    cap = _capture(current, rate=1000.0)
    res = detect_states(cap.current, cap.voltage, cap.sample_rate)
    assert len(res["states"]) == 2
    low, high = res["states"]
    assert low["mean_current_a"] == pytest.approx(1e-6)
    assert high["mean_current_a"] == pytest.approx(20e-3)
    assert low["occurrences"] == 5 and high["occurrences"] == 5
    assert low["time_share"] == pytest.approx(0.5, abs=0.01)
    assert res["transitions"]


def test_detect_states_flat_load_is_one_state():
    res = detect_states([5e-3] * 500, [3.3] * 500, 1000.0)
    assert len(res["states"]) == 1


def test_detect_periodicity_finds_the_interval():
    # A 10 ms burst every 100 ms at 1 ksps -> 10 Hz.
    current = []
    for _ in range(10):
        current += [1e-6] * 90 + [10e-3] * 10
    res = detect_states(current, [3.3] * len(current), 1000.0)
    per = detect_periodicity(res["segments"], len(current) / 1000.0)
    assert per["periodic"] is True
    assert per["period_s"] == pytest.approx(0.1, rel=0.05)
    assert per["frequency_hz"] == pytest.approx(10.0, rel=0.05)
    assert per["jitter_rel"] < 0.05


def test_detect_periodicity_needs_enough_bursts():
    assert detect_periodicity([{"state": 1, "start_s": 0.0}], 1.0)["periodic"] is False


def test_segment_by_markers_brackets_energy():
    markers = [MarkerRecord(100, 0, 5, 1, MARK_KIND_FLAG),
               MarkerRecord(200, 0, 5, 0, MARK_KIND_FLAG)]
    cap = _capture([1.0] * 300, [1.0] * 300, rate=1000.0, markers=markers)
    windows = segment_by_markers(cap)
    assert len(windows) == 1
    assert windows[0]["duration_s"] == pytest.approx(0.1, rel=0.02)
    assert windows[0]["energy_j"] == pytest.approx(0.099, abs=0.002)


def test_meta_quality_reports_range_use_and_flags():
    meta = bytes([0x00, 0x01, 0x01 | 0x10, 0x01 | 0x20])
    q = meta_quality(meta)
    assert q["range_share"]["hi"] == pytest.approx(0.25)
    assert q["range_share"]["mid"] == pytest.approx(0.75)
    assert q["saturated_share"] == pytest.approx(0.25)
    assert q["settling_share"] == pytest.approx(0.25)
    assert q["range_changes"] == 1


def test_preview_is_bounded_and_keeps_extremes():
    current = [0.0] * 999 + [1.0]
    pts = preview(current, [1.0] * 1000, 1000.0, points=10)
    assert len(pts) == 10
    assert max(p["i_max_a"] for p in pts) == pytest.approx(1.0)


def test_analyze_end_to_end_report():
    current = []
    for _ in range(4):
        current += [1e-6] * 200 + [15e-3] * 50
    cap = _capture(current, [3.3] * len(current), rate=1000.0,
                   markers=[MarkerRecord(200, 0, 5, 1, MARK_KIND_FLAG),
                            MarkerRecord(450, 0, 5, 0, MARK_KIND_FLAG)])
    rep = analyze(cap, battery_capacity_mah=225.0)
    assert rep["capture"]["sample_count"] == len(current)
    assert rep["totals"]["energy_j"] > 0
    assert len(rep["states"]) == 2
    assert {s["label"] for s in rep["states"]} == {"sleep", "peak"}
    assert rep["duty_cycle"]["active_time_share"] == pytest.approx(0.2, abs=0.02)
    assert rep["periodicity"]["frequency_hz"] == pytest.approx(4.0, rel=0.1)
    assert rep["battery"]["estimated_hours"] > 0
    assert rep["marker_windows"]
    assert rep["preview"]


def test_analyze_warns_on_saturation_and_loss():
    cap = _capture([1.0] * 100, [1.0] * 100, rate=1000.0,
                   meta=bytes([0x10]) * 100)
    cap.dropped_samples = 50
    rep = analyze(cap)
    joined = " ".join(rep["warnings"])
    assert "saturated" in joined
    assert "lost" in joined


def test_state_labels_are_unique():
    # Six decades of current, so the clusterer produces the maximum state count.
    current = []
    for decade in (1e-8, 1e-7, 1e-6, 1e-5, 1e-4, 1e-3):
        current += [decade] * 100
    rep = analyze(_capture(current, [3.3] * len(current), rate=1000.0))
    labels = [s["label"] for s in rep["states"]]
    assert len(labels) == len(set(labels)), f"duplicate state labels: {labels}"
    assert labels[-1] == "peak"


def test_capture_rate_prefers_device_status_over_stale_header():
    acc = CaptureAccumulator()
    acc.feed(WaveIRecord(0, 0, 128000, 1, [0.1] * 10, bytes(10)))
    acc.feed(StatusRecord({"odr_sps": 8000.0, "stream_decimation": 1}))
    cap = acc.finish()
    assert cap.sample_rate == 8000.0
    assert cap.rate_source == "device_status"
    assert "stale" in cap.rate_warning


def test_capture_rate_keeps_header_when_status_agrees():
    acc = CaptureAccumulator()
    acc.feed(WaveIRecord(0, 0, 128000, 1, [0.1] * 10, bytes(10)))
    acc.feed(StatusRecord({"odr_sps": 128000.0, "stream_decimation": 1}))
    cap = acc.finish()
    assert cap.sample_rate == 128000.0
    assert cap.rate_source == "wave_header"
    assert cap.rate_warning is None


def test_capture_rate_accounts_for_stream_decimation():
    acc = CaptureAccumulator()
    acc.feed(WaveIRecord(0, 0, 128000, 1, [0.1] * 10, bytes(10)))
    acc.feed(StatusRecord({"odr_sps": 128000.0, "stream_decimation": 8}))
    cap = acc.finish()
    assert cap.sample_rate == 16000.0


def test_parse_frame_accepts_a_memoryview():
    frame = _wave_i(5, [1.0, 2.0])
    rec, consumed = parse_frame(memoryview(bytearray(frame)), 0)
    assert consumed == len(frame)
    assert isinstance(rec, WaveIRecord)
    assert rec.current == pytest.approx([1.0, 2.0])


def test_parse_frame_resyncs_within_a_memoryview():
    stream = bytearray(b"\x00\x01\x02" + _wave_i(0, [3.0]))
    rec, consumed = parse_frame(memoryview(stream), 0)
    assert rec is None and consumed == 3
    rec, _ = parse_frame(memoryview(stream), 3)
    assert isinstance(rec, WaveIRecord)
