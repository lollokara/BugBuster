# tests/lib/daq_records.py
"""Typed decoders for DAQ HAT stream records.

Every struct layout here mirrors Firmware/DAQ_HAT/ESP32P4/src/stream/usb_proto.h.
Decoders return None on a truncated payload rather than raising, so a partially
received or corrupt frame is counted and skipped instead of killing a capture.

STATUS is the exception: it is decoded into a plain dict, read defensively by
END offset, because the payload has grown across firmware revisions (20 -> 36 ->
56 -> 96 bytes). Fields from a newer extension are simply absent when talking to
older firmware, which is what forward-compatibility means here.
"""
from __future__ import annotations

import struct
from dataclasses import dataclass

from tests.lib import daq_proto as P

WAVE_HDR = "<QQIHBB"
WAVE_HDR_LEN = struct.calcsize(WAVE_HDR)  # 24


def meta_range(m: int) -> int:
    """Bits 0-1 of a WAVE_I meta byte: current_range_t."""
    return m & 0x03


def meta_source(m: int) -> int:
    """Bits 2-3: which ADC chain produced the fused sample."""
    return (m >> 2) & 0x03


def meta_saturated(m: int) -> bool:
    """Bit 4."""
    return bool(m & 0x10)


def meta_settling(m: int) -> bool:
    """Bit 5 — sample taken while the range latch was still settling."""
    return bool(m & 0x20)


@dataclass(frozen=True)
class WaveI:
    start_index: int
    timestamp_us: int
    sample_rate: int
    count: int
    decimation: int
    samples: tuple
    meta: bytes


@dataclass(frozen=True)
class WaveV:
    start_index: int
    timestamp_us: int
    sample_rate: int
    count: int
    decimation: int
    samples: tuple


@dataclass(frozen=True)
class StatBlock:
    min: float
    max: float
    mean: float
    rms: float
    std: float
    count: int


@dataclass(frozen=True)
class Stats:
    i: StatBlock
    v: StatBlock
    p: StatBlock


@dataclass(frozen=True)
class Energy:
    energy_mwh: float
    energy_j: float
    charge_mah: float
    charge_c: float
    elapsed_s: float
    last_i: float
    last_v: float
    last_p: float


@dataclass(frozen=True)
class Fft:
    sample_rate: int
    nbins: int
    source: int
    window: int
    bins: tuple


@dataclass(frozen=True)
class Marker:
    sample_index: int
    timestamp_us: int
    channel: int
    edge: int
    kind: int


def _decode_wave(payload: bytes, with_meta: bool):
    if len(payload) < WAVE_HDR_LEN:
        return None
    start_index, ts, rate, count, decim, _ = struct.unpack_from(WAVE_HDR, payload, 0)
    need = WAVE_HDR_LEN + count * 4 + (count if with_meta else 0)
    if len(payload) < need:
        return None
    samples = struct.unpack_from("<%df" % count, payload, WAVE_HDR_LEN)
    if with_meta:
        off = WAVE_HDR_LEN + count * 4
        return WaveI(start_index, ts, rate, count, decim, samples,
                     bytes(payload[off:off + count]))
    return WaveV(start_index, ts, rate, count, decim, samples)


def _decode_stats(payload: bytes):
    if len(payload) < 72:
        return None
    blocks = []
    for bi in range(3):
        mn, mx, mean, rms, std, cnt = struct.unpack_from("<fffffI", payload, bi * 24)
        blocks.append(StatBlock(mn, mx, mean, rms, std, cnt))
    return Stats(*blocks)


def _decode_energy(payload: bytes):
    if len(payload) < 52:
        return None
    emwh, ej, cmah, cc, els = struct.unpack_from("<ddddd", payload, 0)
    li, lv, lp = struct.unpack_from("<fff", payload, 40)
    return Energy(emwh, ej, cmah, cc, els, li, lv, lp)


def _decode_fft(payload: bytes):
    if len(payload) < 8:
        return None
    rate, nbins, source, window = struct.unpack_from("<IHBB", payload, 0)
    if len(payload) < 8 + nbins * 4:
        return None
    return Fft(rate, nbins, source, window,
               struct.unpack_from("<%df" % nbins, payload, 8))


def _decode_marker(payload: bytes):
    if len(payload) < 20:
        return None
    idx, ts, ch, edge, kind, _ = struct.unpack_from("<QQBBBB", payload, 0)
    return Marker(idx, ts, ch, edge, kind)


def decode_status(p: bytes) -> dict:
    """Decode usb_status_payload_t defensively, by END offset."""
    d = {}
    if len(p) >= 20:
        d["sample_rate"], d["overflow_count"] = struct.unpack_from("<II", p, 0)
        (d["range"], d["streaming"], d["range_locked"],
         d["source_enabled"]) = struct.unpack_from("<BBBB", p, 8)
        d["vdut_set"], d["ilimit_set"] = struct.unpack_from("<ff", p, 12)
    if len(p) >= 28:
        d["in_voltage"], d["in_current"] = struct.unpack_from("<ff", p, 20)
    if len(p) >= 36:
        d["adaq_ok_bits"], d["fine_err_pct"] = struct.unpack_from("<BB", p, 28)
        d["drop_fine"], d["drop_coarse"] = struct.unpack_from("<HH", p, 30)
        d["fine_diag_sticky"] = p[34]
    if len(p) >= 56:
        (d["frames_tx"], d["bytes_per_sec"], d["fifo_drop_frames"],
         d["ring_high_water"], d["wave_i_index_lo"]) = struct.unpack_from("<IIIII", p, 36)
    if len(p) >= 72:
        d["relay_target"], d["relay_state"] = struct.unpack_from("<BB", p, 56)
        (d["relay_image_size"], d["relay_staged_bytes"],
         d["relay_pushed_bytes"]) = struct.unpack_from("<III", p, 60)
    if len(p) >= 88:
        (d["wave_i_frames"], d["wave_v_frames"],
         d["wave_i_drops"], d["wave_v_drops"]) = struct.unpack_from("<IIII", p, 72)
    if len(p) >= 96:
        d["filter"], d["adc_dec"] = struct.unpack_from("<BB", p, 88)
        d["stream_decim"], = struct.unpack_from("<H", p, 90)
        d["odr_mhz"], = struct.unpack_from("<I", p, 92)
    if len(p) >= 100:
        # Extension v7: board temperatures, 0.1 C, 0x7FFF = not available.
        t0, t1 = struct.unpack_from("<hh", p, 96)
        d["t_board0_c"] = None if t0 == 0x7FFF else t0 / 10.0
        d["t_board1_c"] = None if t1 == 0x7FFF else t1 / 10.0
    return d


def decode(typ: int, payload: bytes):
    """Decode one record payload. Returns None if the payload is truncated."""
    if typ == P.REC_WAVE_I:
        return _decode_wave(payload, with_meta=True)
    if typ == P.REC_WAVE_V:
        return _decode_wave(payload, with_meta=False)
    if typ == P.REC_STATS:
        return _decode_stats(payload)
    if typ == P.REC_ENERGY:
        return _decode_energy(payload)
    if typ == P.REC_FFT:
        return _decode_fft(payload)
    if typ == P.REC_MARKER:
        return _decode_marker(payload)
    if typ == P.REC_STATUS:
        return decode_status(payload)
    return None
