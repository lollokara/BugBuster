"""
daq_stream.py - host-side reader for the DAQ HAT (ESP32-P4) USB-HS data plane.

Mirrors ``Firmware/DAQ_HAT/ESP32P4/src/stream/usb_proto.h``. The P4 exposes its
own USB-HS vendor interface to the PC, independent of the ESP32-S3 CDC/BBP
control link:

    VID 0x303A  PID 0x4001  interface 0  bulk IN 0x81  bulk OUT 0x01

Measurement frames stream device -> PC; control commands flow PC -> device.

This module owns transport + framing only. Post-processing (energy integrals,
state detection, marker segmentation) lives in :mod:`bugbuster.power_analysis`,
deliberately on the host: the P4 has to keep up with the converters, so
anything that can be computed later is computed here.

Usage::

    from bugbuster.daq_stream import DaqStream

    with DaqStream() as s:
        cap = s.capture(duration_s=5.0)
    print(cap.sample_count, cap.duration_s)

``pyusb`` (and a libusb backend) is required. On Windows the P4 interface must
be bound to WinUSB/libusb-win32 (use Zadig) before Python can claim it.
"""
from __future__ import annotations

import struct
import time
from collections import deque
from dataclasses import dataclass, field
from typing import Any, Deque, Dict, Iterable, List, Optional, Sequence, Tuple

# --- Wire constants (mirror usb_proto.h) --------------------------------------
DAQ_VID = 0x303A
DAQ_PID = 0x4001
DAQ_INTERFACE = 0
DAQ_EP_IN = 0x81
DAQ_EP_OUT = 0x01

PROTO_MAGIC0 = 0xBB
PROTO_MAGIC1 = 0x50
PROTO_VERSION = 2

FRAME_HEADER_LEN = 12
FRAME_CRC_LEN = 2
MAX_PAYLOAD = 16384

# Record types (device -> PC)
REC_WAVE_I = 0x01
REC_STATS = 0x02
REC_ENERGY = 0x03
REC_FFT = 0x04
REC_MARKER = 0x05
REC_STATUS = 0x06
REC_WAVE_V = 0x07

# Control commands (PC -> device)
CMD_START = 0x80
CMD_STOP = 0x81
CMD_SET_RATE = 0x82
CMD_RANGE_LOCK = 0x83
CMD_RESET_ENERGY = 0x84
CMD_RESET_STATS = 0x85
CMD_FFT_CONFIG = 0x86
CMD_SET_SOURCE = 0x87
CMD_ARM = 0x88

# meta byte bit layout (WAVE_I only)
META_RANGE_MASK = 0x03
META_SOURCE_SHIFT = 2
META_SOURCE_MASK = 0x03
META_SATURATED = 0x10
META_SETTLING = 0x20

RANGE_NAMES = {0: "hi", 1: "mid", 2: "lo", 0xFF: "unknown"}
SOURCE_NAMES = {0: "fine", 1: "coarse", 2: "blend"}

MARK_KIND_FLAG = 0
MARK_KIND_TRIGGER = 1

# Shunt values behind each range, ohms. Informational - the device already
# applies them; reported so the host can explain a burden-voltage question.
RANGE_SHUNT_OHM = {0: 51.0, 1: 2.0, 2: 0.05}

TEMP_NA = 0x7FFF


class DaqStreamError(RuntimeError):
    """Raised for transport/enumeration failures on the P4 data plane."""


def _libusb_backend():
    """Backend for pyusb, preferring the system libusb.

    Windows has no libusb-1.0 on PATH unless one was installed by hand, so
    ``usb.core.find`` returns None there with no error - indistinguishable from
    "device not plugged in". Fall back to the DLLs bundled by libusb-package.
    """
    try:
        import usb.backend.libusb1 as libusb1  # type: ignore
        backend = libusb1.get_backend()
        if backend is not None:
            return backend
    except ImportError:
        pass
    try:
        import libusb_package  # type: ignore
        return libusb_package.get_libusb1_backend()
    except Exception:
        return None


# --- CRC (control frames only) ------------------------------------------------
def crc16_ccitt(data: bytes, init: int = 0xFFFF) -> int:
    crc = init
    for byte in data:
        crc ^= byte << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) & 0xFFFF if crc & 0x8000 else (crc << 1) & 0xFFFF
    return crc


# --- Decoded records ----------------------------------------------------------
@dataclass
class WaveIRecord:
    start_index: int
    timestamp_us: int
    sample_rate: int
    decimation: int
    current: List[float]
    meta: bytes


@dataclass
class WaveVRecord:
    start_index: int
    timestamp_us: int
    sample_rate: int
    voltage: List[float]


@dataclass
class MarkerRecord:
    sample_index: int
    timestamp_us: int
    channel: int
    edge: int          # 0 = falling, 1 = rising
    kind: int          # MARK_KIND_FLAG | MARK_KIND_TRIGGER

    def as_dict(self) -> Dict[str, Any]:
        return {
            "sample_index": self.sample_index,
            "timestamp_us": self.timestamp_us,
            "channel": self.channel,
            "edge": "rising" if self.edge else "falling",
            "kind": "trigger" if self.kind == MARK_KIND_TRIGGER else "flag",
        }


@dataclass
class EnergyRecord:
    energy_mwh: float
    energy_j: float
    charge_mah: float
    charge_c: float
    elapsed_s: float
    last_i: float
    last_v: float
    last_p: float

    def as_dict(self) -> Dict[str, Any]:
        return dict(self.__dict__)


@dataclass
class StatusRecord:
    raw: Dict[str, Any]

    def as_dict(self) -> Dict[str, Any]:
        return dict(self.raw)


_WAVE_HDR = struct.Struct("<QQIHBB")          # 24 bytes
_MARKER = struct.Struct("<QQBBBB")            # 20 bytes
_ENERGY = struct.Struct("<dddddfff")          # 52 bytes
_STAT_BLOCK = struct.Struct("<fffffI")        # 24 bytes


def _parse_status(p: bytes) -> Dict[str, Any]:
    """Parse usb_status_payload_t, tolerating older/shorter firmware frames."""
    out: Dict[str, Any] = {}
    if len(p) < 20:
        return out
    (sample_rate, overflow, rng, streaming, locked, src_en,
     vdut, ilimit) = struct.unpack_from("<IIBBBBff", p, 0)
    out.update(
        sample_rate=sample_rate,
        overflow_count=overflow,
        range=RANGE_NAMES.get(rng, str(rng)),
        streaming=bool(streaming),
        range_locked=bool(locked),
        source_enabled=bool(src_en),
        vdut_set=vdut,
        ilimit_set=ilimit,
    )
    if len(p) >= 28:
        in_v, in_i = struct.unpack_from("<ff", p, 20)
        out.update(in_voltage=in_v, in_current=in_i)
    if len(p) >= 36:
        ok_bits, err_pct, d_fine, d_coarse, sticky = struct.unpack_from("<BBHHB", p, 28)
        out.update(
            adaq_fine_ok=bool(ok_bits & 0x01),
            adaq_coarse_ok=bool(ok_bits & 0x02),
            adaq_voltage_ok=bool(ok_bits & 0x04),
            fine_err_pct=err_pct,
            drop_fine=d_fine,
            drop_coarse=d_coarse,
            fine_diag_sticky=sticky,
        )
    if len(p) >= 56:
        frames_tx, bps, fifo_drop, ring_hw, idx_lo = struct.unpack_from("<IIIII", p, 36)
        out.update(
            frames_tx=frames_tx,
            bytes_per_sec=bps,
            fifo_drop_frames=fifo_drop,
            ring_high_water=ring_hw,
            wave_i_index_lo=idx_lo,
        )
    if len(p) >= 88:
        wi_f, wv_f, wi_d, wv_d = struct.unpack_from("<IIII", p, 72)
        out.update(wave_i_frames=wi_f, wave_v_frames=wv_f,
                   wave_i_drops=wi_d, wave_v_drops=wv_d)
    if len(p) >= 96:
        filt, adc_dec, stream_dec, odr_mhz = struct.unpack_from("<BBHI", p, 88)
        out.update(filter=filt, adc_decimation=adc_dec,
                   stream_decimation=stream_dec, odr_sps=odr_mhz / 1000.0)
    if len(p) >= 100:
        t0, t1 = struct.unpack_from("<hh", p, 96)
        out["board_temp_analog_c"] = None if t0 == TEMP_NA else t0 / 10.0
        out["board_temp_power_c"] = None if t1 == TEMP_NA else t1 / 10.0
    return out


def _parse_stats(p: bytes) -> Dict[str, Any]:
    names = ("current", "voltage", "power")
    out: Dict[str, Any] = {}
    for i, name in enumerate(names):
        off = i * _STAT_BLOCK.size
        if len(p) < off + _STAT_BLOCK.size:
            break
        mn, mx, mean, rms, std, count = _STAT_BLOCK.unpack_from(p, off)
        out[name] = {"min": mn, "max": mx, "mean": mean,
                     "rms": rms, "std": std, "count": count}
    return out


def parse_frame(buf, off: int = 0) -> Tuple[Optional[Any], int]:
    """Decode one frame at ``off``. ``buf`` may be bytes or a memoryview.

    Returns ``(record, consumed)``. ``record`` is None when the frame is a type
    this host does not decode (FFT), when the version is unsupported, or when
    resynchronising past garbage. ``consumed`` is 0 if more bytes are needed.
    """
    avail = len(buf) - off
    if avail < FRAME_HEADER_LEN:
        return None, 0
    if buf[off] != PROTO_MAGIC0 or buf[off + 1] != PROTO_MAGIC1:
        # Resync: skip to the next plausible magic. Only reached on a corrupt
        # stream, so the copy here is not on the hot path.
        nxt = bytes(buf[off + 1:]).find(bytes((PROTO_MAGIC0, PROTO_MAGIC1)))
        return None, (nxt + 1) if nxt >= 0 else avail
    version = buf[off + 2]
    rec_type = buf[off + 3]
    payload_len = struct.unpack_from("<H", buf, off + 10)[0]
    total = FRAME_HEADER_LEN + payload_len + FRAME_CRC_LEN
    if payload_len > MAX_PAYLOAD:
        return None, 2  # implausible - resync
    if avail < total:
        return None, 0
    if version != PROTO_VERSION:
        return None, total
    p = buf[off + FRAME_HEADER_LEN: off + FRAME_HEADER_LEN + payload_len]

    if rec_type == REC_WAVE_I:
        if len(p) < _WAVE_HDR.size:
            return None, total
        start, ts, rate, count, decim, _pad = _WAVE_HDR.unpack_from(p, 0)
        base = _WAVE_HDR.size
        need = base + count * 4 + count
        if len(p) < need:
            return None, total
        current = list(struct.unpack_from(f"<{count}f", p, base))
        meta = bytes(p[base + count * 4: base + count * 4 + count])
        return WaveIRecord(start, ts, rate, decim, current, meta), total

    if rec_type == REC_WAVE_V:
        if len(p) < _WAVE_HDR.size:
            return None, total
        start, ts, rate, count, _decim, _pad = _WAVE_HDR.unpack_from(p, 0)
        base = _WAVE_HDR.size
        if len(p) < base + count * 4:
            return None, total
        volts = list(struct.unpack_from(f"<{count}f", p, base))
        return WaveVRecord(start, ts, rate, volts), total

    if rec_type == REC_MARKER:
        if len(p) < _MARKER.size:
            return None, total
        idx, ts, ch, edge, kind, _pad = _MARKER.unpack_from(p, 0)
        return MarkerRecord(idx, ts, ch, edge, kind), total

    if rec_type == REC_ENERGY:
        if len(p) < _ENERGY.size:
            return None, total
        return EnergyRecord(*_ENERGY.unpack_from(p, 0)), total

    if rec_type == REC_STATUS:
        return StatusRecord(_parse_status(p)), total

    if rec_type == REC_STATS:
        return StatusRecord({"stats": _parse_stats(p)}), total

    return None, total


def build_frame(cmd_type: int, payload: bytes = b"", seq: int = 0) -> bytes:
    """Frame a PC -> device control command (real CRC-16/CCITT-FALSE)."""
    head = struct.pack("<BBBBBBIH", PROTO_MAGIC0, PROTO_MAGIC1, PROTO_VERSION,
                       cmd_type, 0, 0, seq & 0xFFFFFFFF, len(payload))
    body = head[2:] + payload
    return head + payload + struct.pack("<H", crc16_ccitt(body))


# --- Capture container --------------------------------------------------------
@dataclass
class PowerCapture:
    """A contiguous block of fused measurement data pulled off the data plane."""

    sample_rate: float = 0.0
    current: List[float] = field(default_factory=list)
    voltage: List[float] = field(default_factory=list)
    meta: bytearray = field(default_factory=bytearray)
    markers: List[MarkerRecord] = field(default_factory=list)
    start_index: Optional[int] = None
    start_timestamp_us: Optional[int] = None
    dropped_samples: int = 0
    device_energy: Optional[EnergyRecord] = None
    device_status: Dict[str, Any] = field(default_factory=dict)
    rate_source: str = "wave_header"
    rate_warning: Optional[str] = None

    @property
    def sample_count(self) -> int:
        return len(self.current)

    @property
    def duration_s(self) -> float:
        if self.sample_rate <= 0:
            return 0.0
        return self.sample_count / self.sample_rate

    def marker_dicts(self) -> List[Dict[str, Any]]:
        base = self.start_index or 0
        out = []
        for m in self.markers:
            d = m.as_dict()
            rel = m.sample_index - base
            d["sample_offset"] = rel
            d["t_s"] = rel / self.sample_rate if self.sample_rate > 0 else None
            out.append(d)
        return out


class CaptureAccumulator:
    """Assembles decoded records into a :class:`PowerCapture`.

    Voltage arrives on its own, slower, record stream (the DUT rail moves
    slowly). It is zero-order held onto the current timebase here rather than
    on the device, so the fused arrays are always the same length.
    """

    def __init__(self, max_samples: int = 4_000_000) -> None:
        self.cap = PowerCapture()
        self.max_samples = max_samples
        self._next_index: Optional[int] = None
        self._last_v: Optional[float] = None
        # deque, not list: this is drained from the head on every waveform
        # record, and list.pop(0) made capture assembly O(n^2) - the host fell
        # behind the stream and the DEVICE dropped frames to back-pressure.
        self._v_pending: Deque[Tuple[int, float]] = deque()

    @property
    def full(self) -> bool:
        return self.cap.sample_count >= self.max_samples

    def feed(self, rec: Any) -> None:
        if isinstance(rec, WaveIRecord):
            self._feed_current(rec)
        elif isinstance(rec, WaveVRecord):
            self._feed_voltage(rec)
        elif isinstance(rec, MarkerRecord):
            self.cap.markers.append(rec)
        elif isinstance(rec, EnergyRecord):
            self.cap.device_energy = rec
        elif isinstance(rec, StatusRecord):
            self.cap.device_status.update(rec.raw)

    def _feed_current(self, rec: WaveIRecord) -> None:
        cap = self.cap
        if cap.start_index is None:
            cap.start_index = rec.start_index
            cap.start_timestamp_us = rec.timestamp_us
            cap.sample_rate = float(rec.sample_rate) / max(1, rec.decimation)
            self._next_index = rec.start_index
        elif self._next_index is not None and rec.start_index > self._next_index:
            # Fill the gap so the sample index stays an honest timebase.
            gap = int(rec.start_index - self._next_index)
            gap = min(gap, self.max_samples - cap.sample_count)
            if gap > 0:
                cap.current.extend([float("nan")] * gap)
                cap.meta.extend(b"\x00" * gap)
                cap.dropped_samples += gap
        room = self.max_samples - cap.sample_count
        if room <= 0:
            return
        vals = rec.current[:room]
        cap.current.extend(vals)
        cap.meta.extend(rec.meta[:len(vals)])
        self._next_index = rec.start_index + len(vals)
        self._drain_voltage()

    def _feed_voltage(self, rec: WaveVRecord) -> None:
        for k, v in enumerate(rec.voltage):
            self._v_pending.append((rec.start_index + k, v))
        self._drain_voltage()

    def _drain_voltage(self) -> None:
        """Zero-order-hold voltage onto the current timebase."""
        cap = self.cap
        if cap.start_index is None:
            return
        target = cap.sample_count
        while len(cap.voltage) < target:
            abs_idx = cap.start_index + len(cap.voltage)
            while self._v_pending and self._v_pending[0][0] <= abs_idx:
                self._last_v = self._v_pending.popleft()[1]
            if self._last_v is None:
                if self._v_pending:
                    self._last_v = self._v_pending[0][1]
                else:
                    break
            cap.voltage.append(self._last_v)

    def finish(self) -> PowerCapture:
        self._drain_voltage()
        cap = self.cap
        if len(cap.voltage) < cap.sample_count:
            fill = self._last_v if self._last_v is not None else 0.0
            cap.voltage.extend([fill] * (cap.sample_count - len(cap.voltage)))
        self._reconcile_rate()
        return cap

    def _reconcile_rate(self) -> None:
        """Prefer the STATUS frame's ODR over the WAVE_I header's rate.

        The header carries the acquisition task's cached rate, which is not
        updated when the ODR is changed through the settings registry - it has
        been seen advertising 128000 sps while the part converted at 8000.
        STATUS extension v6 is documented as what the device ACTUALLY applied,
        so it wins; otherwise every integral is silently scaled by the ratio.
        """
        cap = self.cap
        odr = cap.device_status.get("odr_sps")
        if not odr or odr <= 0:
            return
        decim = cap.device_status.get("stream_decimation") or 1
        streamed = odr / max(1, decim)
        if cap.sample_rate <= 0:
            cap.sample_rate = streamed
            cap.rate_source = "device_status"
            return
        if abs(streamed - cap.sample_rate) / cap.sample_rate > 0.05:
            cap.rate_warning = (
                "Waveform header advertised {:.0f} sps but the device reports "
                "an actual ODR of {:.0f} sps (stream decimation {}). Using "
                "{:.0f} sps - the header rate is stale after an ODR change."
            ).format(cap.sample_rate, odr, decim, streamed)
            cap.sample_rate = streamed
            cap.rate_source = "device_status"


# --- USB transport ------------------------------------------------------------
class DaqStream:
    """Vendor-bulk connection to the P4 measurement stream."""

    READ_CHUNK = 65536

    def __init__(self, vid: int = DAQ_VID, pid: int = DAQ_PID) -> None:
        self._vid = vid
        self._pid = pid
        self._dev = None
        self._rx = bytearray()
        self._seq = 0

    # -- lifecycle ---------------------------------------------------------
    def open(self) -> "DaqStream":
        try:
            import usb.core  # type: ignore
            import usb.util  # type: ignore
        except ImportError as exc:
            raise DaqStreamError(
                "pyusb is required for the DAQ data plane. Install it with "
                "'pip install pyusb' (a libusb backend must also be present; "
                "on Windows bind the P4 interface to WinUSB with Zadig)."
            ) from exc

        dev = usb.core.find(idVendor=self._vid, idProduct=self._pid,
                            backend=_libusb_backend())
        if dev is None:
            raise DaqStreamError(
                f"DAQ HAT data plane not found on USB (VID={self._vid:04X} "
                f"PID={self._pid:04X}). Check that the P4's own USB port is "
                f"cabled to this PC, not just the mainboard CDC port."
            )
        try:
            if dev.is_kernel_driver_active(DAQ_INTERFACE):
                dev.detach_kernel_driver(DAQ_INTERFACE)
        except Exception:   # not implemented on Windows/macOS backends
            pass
        try:
            usb.util.claim_interface(dev, DAQ_INTERFACE)
        except Exception as exc:
            raise DaqStreamError(
                f"Could not claim DAQ interface {DAQ_INTERFACE}: {exc}. "
                "Another application (e.g. the desktop app) may hold it."
            ) from exc
        self._dev = dev
        self._rx.clear()
        return self

    def close(self) -> None:
        if self._dev is not None:
            try:
                import usb.util  # type: ignore
                usb.util.release_interface(self._dev, DAQ_INTERFACE)
                usb.util.dispose_resources(self._dev)
            except Exception:
                pass
        self._dev = None
        self._rx.clear()

    def __enter__(self) -> "DaqStream":
        return self.open()

    def __exit__(self, *exc) -> None:
        self.close()

    @property
    def is_open(self) -> bool:
        return self._dev is not None

    # -- IO ----------------------------------------------------------------
    def send(self, cmd_type: int, payload: bytes = b"") -> None:
        if self._dev is None:
            raise DaqStreamError("DAQ stream is not open.")
        self._seq = (self._seq + 1) & 0xFFFFFFFF
        self._dev.write(DAQ_EP_OUT, build_frame(cmd_type, payload, self._seq), 1000)

    def read_records(self, timeout_ms: int = 200) -> List[Any]:
        """Drain whatever is available and return the decoded records."""
        if self._dev is None:
            raise DaqStreamError("DAQ stream is not open.")
        try:
            data = self._dev.read(DAQ_EP_IN, self.READ_CHUNK, timeout_ms)
        except Exception as exc:
            if "timeout" in str(exc).lower() or getattr(exc, "errno", None) == 110:
                data = b""
            else:
                raise DaqStreamError(f"DAQ bulk read failed: {exc}") from exc
        if data:
            self._rx.extend(bytes(data))

        out: List[Any] = []
        off = 0
        buf = memoryview(self._rx)   # parse in place; copying the RX buffer
        blen = len(buf)              # on every read is O(n) per call
        while off < blen:
            rec, consumed = parse_frame(buf, off)
            if consumed == 0:
                break
            off += consumed
            if rec is not None:
                out.append(rec)
        buf.release()
        if off:
            del self._rx[:off]
        return out

    # -- control -----------------------------------------------------------
    def start(self) -> None:
        self.send(CMD_START)

    def stop(self) -> None:
        self.send(CMD_STOP)

    def reset_energy(self) -> None:
        self.send(CMD_RESET_ENERGY)

    def reset_stats(self) -> None:
        self.send(CMD_RESET_STATS)

    def set_rate(self, current_sps: int, voltage_sps: int, decimation: int = 1) -> None:
        self.send(CMD_SET_RATE, struct.pack("<IIB3x", int(current_sps),
                                            int(voltage_sps), int(decimation)))

    def set_range_lock(self, range_idx: Optional[int]) -> None:
        """Lock the current range (0=hi/51R, 1=mid/2R, 2=lo/50mR) or None=auto."""
        self.send(CMD_RANGE_LOCK, bytes([0xFF if range_idx is None else int(range_idx)]))

    def set_source(self, vdut_v: float, ilimit_a: float, enable: bool) -> None:
        self.send(CMD_SET_SOURCE, struct.pack("<ffB3x", float(vdut_v),
                                              float(ilimit_a), 1 if enable else 0))

    def arm(self, armed: bool, trig_logic: int = 0, pre_samples: int = 0) -> None:
        self.send(CMD_ARM, struct.pack("<BBHI", 1 if armed else 0,
                                       int(trig_logic), 0, int(pre_samples)))

    # -- high level --------------------------------------------------------
    def capture(
        self,
        duration_s: float = 1.0,
        max_samples: int = 2_000_000,
        start_stream: bool = True,
        wait_for_trigger: bool = False,
        trigger_timeout_s: float = 10.0,
    ) -> PowerCapture:
        """Record ``duration_s`` of fused measurement data.

        With ``wait_for_trigger``, samples before the TRIGGER marker are
        discarded and the capture window starts at t=0 (arm the trigger engine
        over BBP first - the S3 owns the IO event logic).
        """
        if start_stream:
            self.start()
        acc = CaptureAccumulator(max_samples=max_samples)
        triggered = not wait_for_trigger
        deadline_trig = time.monotonic() + trigger_timeout_s
        deadline: Optional[float] = None if wait_for_trigger else time.monotonic() + duration_s

        try:
            while True:
                now = time.monotonic()
                if not triggered and now > deadline_trig:
                    raise DaqStreamError(
                        f"No trigger within {trigger_timeout_s:.1f} s. Check the "
                        f"trigger IO role/edge and that the DUT event occurred.")
                if deadline is not None and now > deadline:
                    break
                if acc.full:
                    break
                for rec in self.read_records(timeout_ms=100):
                    if not triggered:
                        if isinstance(rec, MarkerRecord) and rec.kind == MARK_KIND_TRIGGER:
                            triggered = True
                            acc = CaptureAccumulator(max_samples=max_samples)
                            deadline = time.monotonic() + duration_s
                            acc.feed(rec)
                        continue
                    acc.feed(rec)
                    # A single read can return a large batch; without this the
                    # loop only checks the clock between batches and overshoots.
                    if acc.full or (deadline is not None
                                    and time.monotonic() > deadline):
                        break
        finally:
            if start_stream:
                try:
                    self.stop()
                except Exception:
                    pass
        return acc.finish()


def daq_stream_present(vid: int = DAQ_VID, pid: int = DAQ_PID) -> bool:
    """True if the P4 data-plane interface is enumerated on this host."""
    try:
        import usb.core  # type: ignore
    except ImportError:
        return False
    try:
        return usb.core.find(idVendor=vid, idProduct=pid,
                             backend=_libusb_backend()) is not None
    except Exception:
        return False


def decode_meta(meta: Sequence[int]) -> Iterable[Dict[str, Any]]:
    """Expand per-sample meta bytes into dicts (range/source/flags)."""
    for m in meta:
        yield {
            "range": RANGE_NAMES.get(m & META_RANGE_MASK, "unknown"),
            "source": SOURCE_NAMES.get((m >> META_SOURCE_SHIFT) & META_SOURCE_MASK, "?"),
            "saturated": bool(m & META_SATURATED),
            "settling": bool(m & META_SETTLING),
        }
