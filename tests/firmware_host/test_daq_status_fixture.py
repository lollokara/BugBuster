"""DAQ STATUS decoders vs the golden fixture generated from usb_proto.h.

Both Python decoders already agree with the firmware layout, so these are
regression guards (they pass on the audit base). The Rust decoder has its own
fixture test in ``daq_proto.rs`` (DAQ-01).
"""

from __future__ import annotations

import json

import pytest

from bugbuster.daq_stream import _parse_status
from tests.firmware_host.gen_daq_status_fixture import BIN, META, header_sha256
from tests.lib.daq_records import decode_status

RAW = BIN.read_bytes()
FIELDS = json.loads(META.read_text(encoding="utf-8"))["fields"]


def v(name):
    return FIELDS[name]["value"]


def test_fixture_is_fresh():
    meta = json.loads(META.read_text(encoding="utf-8"))
    assert meta["source_sha256"] == header_sha256(), (
        "usb_proto.h changed: run `python -m tests.firmware_host.gen_daq_status_fixture` "
        "and update every STATUS decoder")
    assert len(RAW) == meta["size"]


# (fixture field, daq_stream key, transform applied to the fixture value)
_DAQ_STREAM = [
    ("sample_rate", "sample_rate", None), ("overflow_count", "overflow_count", None),
    ("vdut_set", "vdut_set", None), ("ilimit_set", "ilimit_set", None),
    ("in_voltage", "in_voltage", None), ("in_current", "in_current", None),
    ("fine_err_pct", "fine_err_pct", None), ("drop_fine", "drop_fine", None),
    ("drop_coarse", "drop_coarse", None), ("fine_diag_sticky", "fine_diag_sticky", None),
    ("frames_tx", "frames_tx", None), ("bytes_per_sec", "bytes_per_sec", None),
    ("fifo_drop_frames", "fifo_drop_frames", None), ("ring_high_water", "ring_high_water", None),
    ("wave_i_index_lo", "wave_i_index_lo", None), ("wave_i_frames", "wave_i_frames", None),
    ("wave_v_frames", "wave_v_frames", None), ("wave_i_drops", "wave_i_drops", None),
    ("wave_v_drops", "wave_v_drops", None), ("filter", "filter", None),
    ("adc_dec", "adc_decimation", None), ("stream_decim", "stream_decimation", None),
    ("odr_mhz", "odr_sps", lambda x: x / 1000.0),
    ("t_board0_c10", "board_temp_analog_c", lambda x: x / 10.0),
    ("t_board1_c10", "board_temp_power_c", lambda x: x / 10.0),
    ("cal_have_rcal", "cal_have_hi", lambda x: bool(x & 1)),
    ("cal_have_rcal", "cal_have_mid", lambda x: bool(x & 2)),
    ("cal_have_rcal", "cal_have_lo", lambda x: bool(x & 4)),
]


@pytest.mark.parametrize("field,key,fn", _DAQ_STREAM, ids=[k for _f, k, _ in _DAQ_STREAM])
def test_daq_stream_decoder(field, key, fn):
    got = _parse_status(RAW)[key]
    want = fn(v(field)) if fn else v(field)
    assert got == pytest.approx(want), f"{key}: got {got}, want {want}"


_TEST_LIB = [
    ("sample_rate", "sample_rate", None), ("drop_fine", "drop_fine", None),
    ("drop_coarse", "drop_coarse", None), ("fine_diag_sticky", "fine_diag_sticky", None),
    ("frames_tx", "frames_tx", None), ("wave_i_index_lo", "wave_i_index_lo", None),
    ("relay_target", "relay_target", None), ("relay_pushed_bytes", "relay_pushed_bytes", None),
    ("wave_v_drops", "wave_v_drops", None), ("stream_decim", "stream_decim", None),
    ("odr_mhz", "odr_mhz", None), ("t_board0_c10", "t_board0_c", lambda x: x / 10.0),
    ("t_board1_c10", "t_board1_c", lambda x: x / 10.0),
    ("cal_have_rcal", "cal_have_lo", lambda x: bool(x & 4)),
]


@pytest.mark.parametrize("field,key,fn", _TEST_LIB, ids=[k for _f, k, _ in _TEST_LIB])
def test_test_lib_decoder(field, key, fn):
    got = decode_status(RAW)[key]
    want = fn(v(field)) if fn else v(field)
    assert got == pytest.approx(want), f"{key}: got {got}, want {want}"
