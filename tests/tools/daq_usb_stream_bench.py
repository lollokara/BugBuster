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
import os
import sys
import time
from dataclasses import dataclass, asdict, field

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", ".."))

from tests.lib import daq_proto as P            # noqa: E402
from tests.lib.daq_link import DaqLink, DaqLinkUnavailable   # noqa: E402


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


def run(seconds: float, chunk: int, timeout_ms: int) -> Result:
    """Open the P4, stream for `seconds`, and fold the Capture into a Result.

    Result is kept as the tool's own reporting struct (rather than returning a
    Capture directly) so the --json schema, and therefore --compare against
    previously recorded runs, stays byte-compatible.
    """
    res = Result()
    try:
        link = DaqLink.usb(timeout_ms)
    except DaqLinkUnavailable as exc:
        res.error = str(exc)
        return res

    with link:
        link.drain()
        link.start()
        cap = link.collect(seconds, chunk=chunk, timeout_ms=timeout_ms)
        link.stop()

    res.seconds = cap.seconds
    res.bytes_rx = cap.bytes_rx
    res.frames = dict(cap.frames)
    res.wave_i_samples = cap.wave_i_samples
    res.wave_v_samples = cap.wave_v_samples
    res.seq_first = cap.seq_first
    res.seq_last = cap.seq_last
    res.seq_gaps = cap.seq_gaps
    res.seq_lost = cap.seq_lost
    res.resyncs = cap.resyncs
    res.device = dict(cap.last_status)

    if cap.stats:
        s = cap.stats[-1]
        res.stats = {
            nm: dict(min=b.min, max=b.max, mean=b.mean, rms=b.rms, std=b.std,
                     count=b.count)
            for nm, b in (("i", s.i), ("v", s.v), ("p", s.p))
        }
    if cap.energy:
        first, last = cap.energy[0], cap.energy[-1]
        res.energy_first = dict(charge_c=first.charge_c, energy_j=first.energy_j,
                                elapsed_s=first.elapsed_s)
        res.energy = dict(energy_mwh=last.energy_mwh, energy_j=last.energy_j,
                          charge_mah=last.charge_mah, charge_c=last.charge_c,
                          elapsed_s=last.elapsed_s, last_i=last.last_i,
                          last_v=last.last_v, last_p=last.last_p)
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
