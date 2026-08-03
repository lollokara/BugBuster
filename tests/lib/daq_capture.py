"""Incremental DAQ frame parser + the Capture accumulator it feeds.

The parser is deliberately paranoid about frame boundaries. The 2-byte magic
0xBB 0x50 occurs freely inside f32 sample payloads, so magic alone is not a
frame start: a resync candidate is accepted only when version, type AND length
all validate. This is the same rule the iOS client applies in
`DaqStreamEngine.headerLooksValid`, added after a partial lwIP send() desynced
the stream and the phone decoded float bytes as fake samples.
"""
from __future__ import annotations

import struct
from dataclasses import dataclass, field
from typing import Callable

from tests.lib import daq_proto as P
from tests.lib import daq_records as R


class FrameParser:
    """Feed bytes, get decoded records via the on_record callback.

    on_record(typ: int, seq: int, record: object | None, payload_len: int)
    """

    def __init__(self, on_record: Callable[[int, int, object, int], None]):
        self.buf = bytearray()
        self.on_record = on_record
        self.resyncs = 0
        self.bad_frames = 0

    def _header_ok(self, i: int) -> bool:
        b = self.buf
        return (b[i] == P.MAGIC0 and b[i + 1] == P.MAGIC1
                and b[i + 2] == P.PROTO_VERSION
                and b[i + 3] in P.KNOWN_TYPES
                and struct.unpack_from("<H", b, i + 10)[0] <= P.MAX_PAYLOAD)

    def feed(self, data: bytes) -> None:
        self.buf += data
        i = 0
        n = len(self.buf)
        while n - i >= P.HDR_LEN:
            if not self._header_ok(i):
                nxt = self.buf.find(bytes([P.MAGIC0]), i + 1)
                if nxt < 0:
                    i = n
                    break
                self.resyncs += 1
                i = nxt
                continue
            plen = struct.unpack_from("<H", self.buf, i + 10)[0]
            total = P.HDR_LEN + plen + P.CRC_LEN
            if n - i < total:
                break
            typ = self.buf[i + 3]
            seq = struct.unpack_from("<I", self.buf, i + 6)[0]
            payload = bytes(self.buf[i + P.HDR_LEN:i + P.HDR_LEN + plen])
            rec = R.decode(typ, payload)
            if rec is None and typ != P.REC_STATUS:
                self.bad_frames += 1
            self.on_record(typ, seq, rec, plen)
            i += total
        del self.buf[:i]


@dataclass
class Capture:
    """Accumulates one capture window's worth of decoded records."""

    seconds: float = 0.0
    bytes_rx: int = 0
    frames: dict = field(default_factory=dict)
    wave_i: list = field(default_factory=list)
    wave_v: list = field(default_factory=list)
    stats: list = field(default_factory=list)
    energy: list = field(default_factory=list)
    fft: list = field(default_factory=list)
    markers: list = field(default_factory=list)
    status: list = field(default_factory=list)
    seq_first: int = -1
    seq_last: int = -1
    seq_gaps: int = 0
    seq_lost: int = 0
    resyncs: int = 0
    _expect_seq: int = None

    def on_record(self, typ: int, seq: int, rec, plen: int) -> None:
        name = P.TYPE_NAMES.get(typ, "0x%02X" % typ)
        self.frames[name] = self.frames.get(name, 0) + 1

        # Sequence is monotonic across ALL emitted frames; a gap means the device
        # dropped a frame it had already decided to send (back-pressure or a
        # failed write), so this is real, attributable loss.
        if self.seq_first < 0:
            self.seq_first = seq
        elif self._expect_seq is not None and seq != self._expect_seq:
            # _expect_seq is the seq we SHOULD have received next, so delta is
            # the count of frames that never arrived -- not the raw seq jump.
            # For 10 -> 13, expect=11 and delta=2 (frames 11 and 12 are lost).
            delta = (seq - self._expect_seq) & 0xFFFFFFFF
            if 0 < delta < 1 << 31:
                self.seq_gaps += 1
                self.seq_lost += delta
        self.seq_last = seq
        self._expect_seq = (seq + 1) & 0xFFFFFFFF

        if rec is None:
            return
        if typ == P.REC_WAVE_I:
            self.wave_i.append(rec)
        elif typ == P.REC_WAVE_V:
            self.wave_v.append(rec)
        elif typ == P.REC_STATS:
            self.stats.append(rec)
        elif typ == P.REC_ENERGY:
            self.energy.append(rec)
        elif typ == P.REC_FFT:
            self.fft.append(rec)
        elif typ == P.REC_MARKER:
            self.markers.append(rec)
        elif typ == P.REC_STATUS:
            self.status.append(rec)

    @property
    def wave_i_samples(self) -> int:
        return sum(w.count for w in self.wave_i)

    @property
    def wave_v_samples(self) -> int:
        return sum(w.count for w in self.wave_v)

    @property
    def wave_i_sps(self) -> float:
        return self.wave_i_samples / self.seconds if self.seconds else 0.0

    @property
    def wave_v_sps(self) -> float:
        return self.wave_v_samples / self.seconds if self.seconds else 0.0

    @property
    def mb_per_s(self) -> float:
        return self.bytes_rx / self.seconds / (1024 * 1024) if self.seconds else 0.0

    @property
    def frame_loss_pct(self) -> float:
        span = self.seq_last - self.seq_first + 1
        return 100.0 * self.seq_lost / span if span > 0 else 0.0

    @property
    def last_status(self) -> dict:
        return self.status[-1] if self.status else {}

    @property
    def contiguous_wave_i(self) -> bool:
        """True when consecutive WAVE_I blocks tile the sample index with no hole."""
        for a, b in zip(self.wave_i, self.wave_i[1:]):
            if a.start_index + a.count != b.start_index:
                return False
        return True

    def all_current(self) -> list:
        out = []
        for w in self.wave_i:
            out.extend(w.samples)
        return out

    def all_voltage(self) -> list:
        out = []
        for w in self.wave_v:
            out.extend(w.samples)
        return out

    def all_meta(self) -> bytes:
        return b"".join(w.meta for w in self.wave_i)
