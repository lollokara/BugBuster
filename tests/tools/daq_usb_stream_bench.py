#!/usr/bin/env python3
"""
daq_usb_stream_bench.py — end-to-end USB-HS stream benchmark for the DAQ HAT.

Why this exists (and why `faststat` alone is not enough):

`faststat` measures only the CAPTURE half of the pipeline -- DRDY edges
serviced into the ring. It says nothing about the consumer half, and worse,
with no USB host attached the consumer half barely runs at all:
`usb_stream.c:emit_frame_inplace()` early-returns on its `transport.connected()`
check before it builds a header, computes anything, or touches the transport.
So an unattached board never exercises frame assembly, batching, or the
back-pressure path, and core 0 idles through the entire back half of the
pipeline. Benchmarking a firmware change without a consumer attached measures
a stub.

This tool is that consumer. It opens the P4's vendor-bulk interface
(VID 0x303A / PID 0x4001, EP OUT 0x01 / EP IN 0x81 -- the same endpoints
DesktopApp's `daq_usb.rs` uses), issues USB_CMD_START, drains the stream for a
fixed window, and reports what actually arrived:

  * host-side throughput (MB/s) and per-record-type frame counts
  * WAVE_I / WAVE_V sample rates achieved END TO END, which is the number that
    matters to a user -- capture rate minus every loss downstream of it
  * frame loss inferred from gaps in the wire `seq` field
  * the device's own STATUS counters (extension v3/v5): frames_tx,
    bytes_per_sec, fifo_drop_frames, ring_high_water, drop_fine/drop_coarse,
    overflow_count -- so device-side drops and wire-side drops can be told
    apart

Wire format: Firmware/DAQ_HAT/ESP32P4/src/stream/usb_proto.h (v2).
Frame = 12-byte header + payload + 2-byte CRC. Device->PC data frames (type
< 0x80) carry an unchecked 0x0000 CRC; PC->device control frames (>= 0x80)
carry a real CRC-16/CCITT-FALSE, which this tool computes for CMD_START/STOP.

Usage:
    python3 tests/tools/daq_usb_stream_bench.py --seconds 5 --json after.json
    python3 tests/tools/daq_usb_stream_bench.py --compare before.json after.json

Requires pyusb + libusb (`brew install libusb && pip3 install pyusb`).
"""

from __future__ import annotations

import argparse
import json
import struct
import sys
import time
from dataclasses import dataclass, asdict, field

try:
    import usb.core
    import usb.util
except ImportError:  # pragma: no cover
    sys.exit("pyusb is required: pip3 install pyusb  (and: brew install libusb)")


VID = 0x303A
PID = 0x4001
EP_OUT = 0x01
EP_IN = 0x81

MAGIC0, MAGIC1 = 0xBB, 0x50
PROTO_VERSION = 2
HDR_LEN = 12
CRC_LEN = 2

REC_WAVE_I = 0x01
REC_STATS = 0x02
REC_ENERGY = 0x03
REC_FFT = 0x04
REC_MARKER = 0x05
REC_STATUS = 0x06
REC_WAVE_V = 0x07

CMD_START = 0x80
CMD_STOP = 0x81

TYPE_NAMES = {
    REC_WAVE_I: "WAVE_I", REC_STATS: "STATS", REC_ENERGY: "ENERGY",
    REC_FFT: "FFT", REC_MARKER: "MARKER", REC_STATUS: "STATUS",
    REC_WAVE_V: "WAVE_V",
}
KNOWN_TYPES = set(TYPE_NAMES)


def crc16_ccitt(data: bytes, init: int = 0xFFFF) -> int:
    crc = init
    for b in data:
        crc ^= b << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) & 0xFFFF if crc & 0x8000 else (crc << 1) & 0xFFFF
    return crc


def build_control_frame(cmd: int, payload: bytes = b"") -> bytes:
    body = bytes([PROTO_VERSION, cmd, 0, 0]) + struct.pack("<I", 0) + \
        struct.pack("<H", len(payload)) + payload
    return bytes([MAGIC0, MAGIC1]) + body + struct.pack("<H", crc16_ccitt(body))


@dataclass
class Result:
    seconds: float = 0.0
    bytes_rx: int = 0
    frames: dict = field(default_factory=dict)
    wave_i_samples: int = 0
    wave_v_samples: int = 0
    seq_first: int = -1
    seq_last: int = -1
    seq_gaps: int = 0
    seq_lost: int = 0
    resyncs: int = 0
    device: dict = field(default_factory=dict)
    stats: dict = field(default_factory=dict)
    energy: dict = field(default_factory=dict)
    energy_first: dict = field(default_factory=dict)
    error: str = ""

    @property
    def mb_per_s(self) -> float:
        return self.bytes_rx / self.seconds / (1024 * 1024) if self.seconds else 0.0

    @property
    def wave_i_sps(self) -> float:
        return self.wave_i_samples / self.seconds if self.seconds else 0.0

    @property
    def wave_v_sps(self) -> float:
        return self.wave_v_samples / self.seconds if self.seconds else 0.0

    @property
    def frame_loss_pct(self) -> float:
        span = self.seq_last - self.seq_first + 1
        return 100.0 * self.seq_lost / span if span > 0 else 0.0


def parse_status(p: bytes) -> dict:
    """Decode usb_status_payload_t. Fields are read defensively by END offset
    so a frame from older firmware (which is shorter) degrades to absent keys
    rather than raising."""
    d = {}
    if len(p) >= 20:
        d["sample_rate"], d["overflow_count"] = struct.unpack_from("<II", p, 0)
        d["range"], d["streaming"], d["range_locked"], d["source_enabled"] = \
            struct.unpack_from("<BBBB", p, 8)
    if len(p) >= 36:
        d["adaq_ok_bits"], d["fine_err_pct"] = struct.unpack_from("<BB", p, 28)
        d["drop_fine"], d["drop_coarse"] = struct.unpack_from("<HH", p, 30)
    if len(p) >= 56:
        (d["frames_tx"], d["bytes_per_sec"], d["fifo_drop_frames"],
         d["ring_high_water"], d["wave_i_index_lo"]) = struct.unpack_from("<IIIII", p, 36)
    if len(p) >= 88:
        (d["wave_i_frames"], d["wave_v_frames"],
         d["wave_i_drops"], d["wave_v_drops"]) = struct.unpack_from("<IIII", p, 72)
    if len(p) >= 96:
        d["filter"], d["adc_dec"] = struct.unpack_from("<BB", p, 88)
        d["stream_decim"], = struct.unpack_from("<H", p, 90)
        d["odr_mhz"], = struct.unpack_from("<I", p, 92)
    return d


class FrameParser:
    """Incremental parser mirroring the iOS client's `drainFrames` logic:
    accept a resync candidate only when version, type AND length are all
    plausible, because the 2-byte magic occurs freely inside float payloads."""

    def __init__(self, res: Result):
        self.buf = bytearray()
        self.res = res
        self.expect_seq = None

    def header_ok(self, i: int) -> bool:
        b = self.buf
        return (b[i] == MAGIC0 and b[i + 1] == MAGIC1 and b[i + 2] == PROTO_VERSION
                and b[i + 3] in KNOWN_TYPES
                and struct.unpack_from("<H", b, i + 10)[0] <= 16384)

    def feed(self, data: bytes) -> None:
        self.buf += data
        i = 0
        n = len(self.buf)
        while n - i >= HDR_LEN:
            if not self.header_ok(i):
                nxt = self.buf.find(bytes([MAGIC0]), i + 1)
                if nxt < 0:
                    i = n
                    break
                self.res.resyncs += 1
                i = nxt
                continue
            plen = struct.unpack_from("<H", self.buf, i + 10)[0]
            total = HDR_LEN + plen + CRC_LEN
            if n - i < total:
                break
            typ = self.buf[i + 3]
            seq = struct.unpack_from("<I", self.buf, i + 6)[0]
            payload = bytes(self.buf[i + HDR_LEN:i + HDR_LEN + plen])
            self.on_frame(typ, seq, payload)
            i += total
        del self.buf[:i]

    def on_frame(self, typ: int, seq: int, payload: bytes) -> None:
        r = self.res
        name = TYPE_NAMES.get(typ, f"0x{typ:02X}")
        r.frames[name] = r.frames.get(name, 0) + 1

        # Sequence is per-stream and monotonic across ALL emitted frames; a gap
        # means the device dropped a frame it had decided to send (back-pressure
        # or a failed write), so this is real, attributable loss.
        if r.seq_first < 0:
            r.seq_first = seq
        elif self.expect_seq is not None and seq != self.expect_seq:
            delta = (seq - self.expect_seq) & 0xFFFFFFFF
            if 0 < delta < 1 << 31:
                r.seq_gaps += 1
                r.seq_lost += delta
        r.seq_last = seq
        self.expect_seq = (seq + 1) & 0xFFFFFFFF

        if typ in (REC_WAVE_I, REC_WAVE_V) and len(payload) >= 24:
            count = struct.unpack_from("<H", payload, 20)[0]
            if typ == REC_WAVE_I:
                r.wave_i_samples += count
            else:
                r.wave_v_samples += count
        elif typ == REC_STATUS:
            r.device = parse_status(payload)
        elif typ == REC_ENERGY and len(payload) >= 52:
            (emwh, ej, cmah, cc, els) = struct.unpack_from("<ddddd", payload, 0)
            li, lv, lp = struct.unpack_from("<fff", payload, 40)
            if not r.energy_first:
                r.energy_first = dict(charge_c=cc, energy_j=ej, elapsed_s=els)
            r.energy = dict(energy_mwh=emwh, energy_j=ej, charge_mah=cmah,
                            charge_c=cc, elapsed_s=els,
                            last_i=li, last_v=lv, last_p=lp)
        elif typ == REC_STATS and len(payload) >= 72:
            # usb_stats_payload_t = 3 x usb_stat_block_t{f32 min,max,mean,rms,std; u32 count}
            names = ("i", "v", "p")
            blocks = {}
            for bi, nm in enumerate(names):
                mn, mx, mean, rms, std, cnt = struct.unpack_from("<fffffI", payload, bi * 24)
                blocks[nm] = dict(min=mn, max=mx, mean=mean, rms=rms, std=std, count=cnt)
            r.stats = blocks


def run(seconds: float, chunk: int, timeout_ms: int) -> Result:
    res = Result()
    dev = usb.core.find(idVendor=VID, idProduct=PID)
    if dev is None:
        res.error = (f"no device {VID:04x}:{PID:04x}. The P4's USB-HS port must "
                     "be cabled to this host (the JTAG console port is a "
                     "different connector and does NOT carry the stream).")
        return res

    try:
        if dev.is_kernel_driver_active(0):
            dev.detach_kernel_driver(0)
    except (NotImplementedError, usb.core.USBError):
        pass  # macOS: vendor-class interfaces are not claimed by a kernel driver

    try:
        dev.set_configuration()
    except usb.core.USBError as e:
        res.error = f"set_configuration failed: {e}"
        return res
    usb.util.claim_interface(dev, 0)

    parser = FrameParser(res)
    try:
        # Drain anything the device queued before we attached, so the first
        # measured window starts clean and seq accounting is not polluted by a
        # partial frame from a previous session.
        while True:
            try:
                if not dev.read(EP_IN, chunk, timeout=50):
                    break
            except usb.core.USBError:
                break

        dev.write(EP_OUT, build_control_frame(CMD_START), timeout=1000)
        t0 = time.perf_counter()
        deadline = t0 + seconds
        while time.perf_counter() < deadline:
            try:
                data = dev.read(EP_IN, chunk, timeout=timeout_ms)
            except usb.core.USBError as e:
                if "timed out" in str(e).lower():
                    continue
                res.error = f"bulk read failed: {e}"
                break
            if data:
                res.bytes_rx += len(data)
                parser.feed(bytes(data))
        res.seconds = time.perf_counter() - t0
    finally:
        try:
            dev.write(EP_OUT, build_control_frame(CMD_STOP), timeout=1000)
        except usb.core.USBError:
            pass
        usb.util.release_interface(dev, 0)
        usb.util.dispose_resources(dev)
    return res


def print_report(r: Result, title: str = "") -> None:
    if title:
        print(f"\n== {title} ==")
    if r.error:
        print(f"ERROR: {r.error}")
        return
    print(f"window            : {r.seconds:.2f} s")
    print(f"host throughput   : {r.mb_per_s:.2f} MB/s  ({r.bytes_rx} bytes)")
    print(f"WAVE_I end-to-end : {r.wave_i_sps:,.0f} Sa/s  ({r.wave_i_samples} samples)")
    print(f"WAVE_V end-to-end : {r.wave_v_sps:,.0f} Sa/s  ({r.wave_v_samples} samples)")
    print(f"frames            : " +
          "  ".join(f"{k}={v}" for k, v in sorted(r.frames.items())))
    print(f"wire frame loss   : {r.seq_lost} frames in {r.seq_gaps} gaps "
          f"({r.frame_loss_pct:.3f}%), resyncs={r.resyncs}")
    d = r.device
    if d:
        print("device counters   :")
        print(f"    frames_tx={d.get('frames_tx')}  fifo_drop_frames={d.get('fifo_drop_frames')}  "
              f"bytes_per_sec={d.get('bytes_per_sec')}")
        print(f"    ring_high_water={d.get('ring_high_water')}  overflow_count={d.get('overflow_count')}")
        print(f"    drop_fine={d.get('drop_fine')}  drop_coarse={d.get('drop_coarse')}  "
              f"fine_err_pct={d.get('fine_err_pct')}")
        print(f"    wave_i_frames={d.get('wave_i_frames')} drops={d.get('wave_i_drops')}  "
              f"wave_v_frames={d.get('wave_v_frames')} drops={d.get('wave_v_drops')}")
    if r.energy and r.energy_first:
        # Cross-check the on-device integrators against independently-known
        # quantities. Two things must hold or the integration is wrong:
        #   elapsed_s must advance at 1 s per wall second, and
        #   d(charge) must equal mean current x d(elapsed).
        d_el = r.energy["elapsed_s"] - r.energy_first["elapsed_s"]
        d_q  = r.energy["charge_c"] - r.energy_first["charge_c"]
        d_e  = r.energy["energy_j"] - r.energy_first["energy_j"]
        print("device integrators:")
        print(f"    elapsed={r.energy['elapsed_s']:.3f} s  charge={r.energy['charge_mah']:.6f} mAh  "
              f"energy={r.energy['energy_mwh']:.6f} mWh")
        clock_err = abs(d_el - r.seconds) / r.seconds if r.seconds else 0
        ok_clock = "OK" if clock_err < 0.05 else "*** CLOCK DRIFT ***"
        print(f"    d(elapsed)={d_el:.3f} s vs wall {r.seconds:.3f} s "
              f"({clock_err*100:.1f}%) -> {ok_clock}")
        if r.stats and d_el > 0:
            i_mean = r.stats["i"]["mean"]
            expect_q = i_mean * d_el
            err = abs(d_q - expect_q) / max(abs(expect_q), 1e-12)
            ok_q = "OK" if err < 0.05 else "*** CHARGE MISMATCH ***"
            print(f"    d(charge)={d_q:.6e} C vs mean_i*d(elapsed)={expect_q:.6e} C "
                  f"({err*100:.1f}%) -> {ok_q}")
            p_mean = r.stats["p"]["mean"]
            expect_e = p_mean * d_el
            err_e = abs(d_e - expect_e) / max(abs(expect_e), 1e-12)
            ok_e = "OK" if err_e < 0.05 else "*** ENERGY MISMATCH ***"
            print(f"    d(energy)={d_e:.6e} J vs mean_p*d(elapsed)={expect_e:.6e} J "
                  f"({err_e*100:.1f}%) -> {ok_e}")
    if r.stats:
        # Validate the device's own statistics rather than just printing them:
        # rms^2 == var + mean^2 is an algebraic identity, so a violation means
        # the on-device variance/RMS math is wrong (this is exactly what the
        # shifted-data rewrite in power_dsp.c had to preserve).
        print("device statistics :")
        for nm, b in r.stats.items():
            lhs = b["rms"] ** 2
            rhs = b["std"] ** 2 + b["mean"] ** 2
            scale = max(abs(lhs), abs(rhs), 1e-30)
            ok = "OK" if abs(lhs - rhs) / scale < 1e-3 else "*** IDENTITY VIOLATED ***"
            print(f"    {nm}: mean={b['mean']:+.6g} rms={b['rms']:.6g} std={b['std']:.6g} "
                  f"min={b['min']:+.6g} max={b['max']:+.6g} n={b['count']}")
            print(f"       rms^2={lhs:.6g} vs std^2+mean^2={rhs:.6g}  -> {ok}")
    if d:
        if d.get("odr_mhz"):
            print(f"    device ODR={d['odr_mhz'] / 1000.0:.0f} SPS  stream_decim={d.get('stream_decim')}")


def compare(before_path: str, after_path: str) -> int:
    with open(before_path) as f:
        b = json.load(f)
    with open(after_path) as f:
        a = json.load(f)

    def row(label, bv, av, higher_better=True, fmt="{:,.0f}"):
        if bv in (None, 0) and av in (None, 0):
            return 0
        d = (av or 0) - (bv or 0)
        pct = 100.0 * d / bv if bv else float("inf")
        better = (d > 0) if higher_better else (d < 0)
        flag = "" if abs(pct) < 1.0 else ("  ok" if better else "  <-- REGRESSION")
        print(f"{label:<22} {fmt.format(bv or 0):>14} {fmt.format(av or 0):>14} "
              f"{d:>+14,.0f} {pct:>+8.1f}%{flag}")
        return 0 if (better or abs(pct) < 1.0) else 1

    print(f"\n== {before_path} -> {after_path} ==")
    print(f"{'metric':<22} {'before':>14} {'after':>14} {'delta':>14} {'pct':>9}")
    print("-" * 82)
    bad = 0
    bad += row("host MB/s", b["mb_per_s"], a["mb_per_s"], True, "{:,.2f}")
    bad += row("WAVE_I Sa/s", b["wave_i_sps"], a["wave_i_sps"], True)
    bad += row("WAVE_V Sa/s", b["wave_v_sps"], a["wave_v_sps"], True)
    bad += row("wire frames lost", b["seq_lost"], a["seq_lost"], False)
    bad += row("device fifo drops", b["device"].get("fifo_drop_frames"),
               a["device"].get("fifo_drop_frames"), False)
    bad += row("device overflow", b["device"].get("overflow_count"),
               a["device"].get("overflow_count"), False)
    print()
    print("FAIL: regression detected." if bad else "OK: no stream regression.")
    return 1 if bad else 0


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--seconds", type=float, default=5.0)
    ap.add_argument("--chunk", type=int, default=262144,
                    help="bulk read size; large reads keep the host off the "
                         "critical path (desktop uses 4x64KB queued)")
    ap.add_argument("--timeout-ms", type=int, default=1000)
    ap.add_argument("--json", help="write results here")
    ap.add_argument("--compare", nargs=2, metavar=("BEFORE", "AFTER"))
    args = ap.parse_args()

    if args.compare:
        return compare(*args.compare)

    res = run(args.seconds, args.chunk, args.timeout_ms)
    print_report(res, "DAQ USB-HS stream")
    if res.error:
        return 2
    if args.json:
        payload = asdict(res)
        payload.update(mb_per_s=res.mb_per_s, wave_i_sps=res.wave_i_sps,
                       wave_v_sps=res.wave_v_sps, frame_loss_pct=res.frame_loss_pct)
        with open(args.json, "w") as f:
            json.dump(payload, f, indent=2)
        print(f"\nwrote {args.json}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
