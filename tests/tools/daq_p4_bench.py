#!/usr/bin/env python3
"""
daq_p4_bench.py — ODR sweep benchmark for the ESP32-P4 DAQ HAT capture engine.

Talks to the P4's own serial CLI (USB-Serial-JTAG console, NOT the S3's BBP
port) and, at each requested ODR, measures what the DRDY-gated capture engine
actually achieves versus what the ADCs are configured to produce.

Why this exists: the capture path is interrupt/edge driven, so throughput is a
function of the configured ODR, not a fixed number. A single measurement at one
ODR says nothing -- the interesting behaviour (ring overflow, missed DRDY
edges, FINE/COARSE pairing drops) only appears as the configured rate
approaches the engine's ceiling. Comparing a firmware change therefore requires
the same sweep before and after, which is what --json + --compare do.

IMPORTANT — the `odr` CLI argument is an ADC OVERSAMPLING RATIO, not a rate.
Per-channel sample rate is 8_192_000 / ratio, so LOWER ratio = HIGHER rate:
    odr 32  -> 256 kSPS/ch (768 kSPS aggregate across FINE+COARSE+VOLTAGE)
    odr 128 ->  64 kSPS/ch (192 kSPS aggregate)
    odr 256 ->  32 kSPS/ch  (96 kSPS aggregate)
Two distinct saturation regimes were measured on real hardware, and they have
different root causes -- a benchmark that samples only one of them is
misleading:
  * ratio <= 64 : the CAPTURE task saturates. DRDY edges go unserviced
    (`missed` climbs to ~48%) while `overflow` stays 0 -- the ring never fills
    because the producer itself cannot keep up.
  * ratio ~128  : the CONSUMER saturates. `overflow` explodes (190k+/s) and
    FINE/COARSE pairing drops appear, because daq_fast_task cannot drain the
    ring as fast as the capture task fills it.
Changes to the downstream pipeline (DSP tail, FFT, transport) move the SECOND
regime; changes to the SPI/DRDY path move the first. Sweep both.

The device reports, per 1 s window (`faststat`):
  * per-bus achieved SPS and DRDY edges/s
  * `overflow` -- ring pushes rejected because the consumer fell behind
  * `missed`   -- DRDY edges the capture task never serviced
  * `overlap`  -- passes where both SPI hosts clocked concurrently
plus, from `status`, the FINE/COARSE pairing drop counters.

Usage:
    # baseline sweep, saved for later comparison
    python3 tests/tools/daq_p4_bench.py --json before.json

    # after a firmware change
    python3 tests/tools/daq_p4_bench.py --json after.json
    python3 tests/tools/daq_p4_bench.py --compare before.json after.json

    # custom sweep
    python3 tests/tools/daq_p4_bench.py --odr 32,64,128,256,512,1024

The port defaults to the first ESP32 USB-Serial-JTAG that answers with the
`daq>` prompt; override with --port.
"""

from __future__ import annotations

import argparse
import glob
import json
import re
import sys
import time
from dataclasses import dataclass, asdict, field

try:
    import serial  # type: ignore
except ImportError:  # pragma: no cover
    sys.exit("pyserial is required: pip3 install pyserial")


PROMPT = b"daq> "
# Sample rate per channel for a given oversampling ratio.
ADAQ_BASE_SPS = 8_192_000

# Focused on the two saturation regimes described above, with extra resolution
# around the consumer-side knee near ratio 128 where pipeline changes show up.
DEFAULT_ODRS = [32, 48, 64, 96, 128, 160, 192, 256, 512]


# ---------------------------------------------------------------------------
# CLI transport
# ---------------------------------------------------------------------------
class P4Console:
    """Line-oriented client for the P4 CLI.

    Reads until the `daq> ` prompt rather than sleeping a fixed interval: some
    commands (`fast on`, `odr`) reprogram both ADAQs over SPI and take far
    longer than others, and a fixed sleep either truncates their output or
    wastes seconds on every fast command.
    """

    def __init__(self, port: str, baud: int = 115200, timeout: float = 1.0):
        self.ser = serial.Serial(port, baud, timeout=timeout)
        self.port = port
        time.sleep(0.3)
        self.ser.reset_input_buffer()

    def close(self) -> None:
        self.ser.close()

    def cmd(self, line: str, deadline: float = 8.0) -> str:
        self.ser.reset_input_buffer()
        self.ser.write((line + "\r\n").encode())
        buf = bytearray()
        end = time.time() + deadline
        while time.time() < end:
            chunk = self.ser.read(4096)
            if chunk:
                buf += chunk
                if buf.endswith(PROMPT):
                    break
            elif buf:
                # Quiet for a full read timeout with data already in hand:
                # the prompt may have been consumed by a prior read.
                break
        return buf.decode("utf8", "replace")

    def alive(self) -> bool:
        return "daq>" in self.cmd("", deadline=2.0)


def autodetect_port() -> str:
    candidates = sorted(glob.glob("/dev/cu.usbmodem*"))
    for port in candidates:
        try:
            con = P4Console(port, timeout=0.6)
        except Exception:
            continue
        try:
            if con.alive():
                return port
        finally:
            con.close()
    raise SystemExit(
        "No P4 DAQ console found. Tried: %s\n"
        "Pass --port explicitly, or check the DAQ HAT is powered and its "
        "USB-Serial-JTAG port is connected." % (", ".join(candidates) or "none")
    )


# ---------------------------------------------------------------------------
# Output parsing
# ---------------------------------------------------------------------------
# Bus A  FINE           :    31989 SPS   edges    31989/s   overflow +0   missed ~11
RE_BUS = re.compile(
    r"Bus\s+(?P<bus>\S+)\s+(?P<name>[\w+]+)\s*:\s*(?P<sps>\d+)\s*SPS\s+"
    r"edges\s+(?P<edges>\d+)/s\s+overflow\s*\+(?P<overflow>\d+)\s+missed\s*~(?P<missed>\d+)"
)
# total captured         :   127955 SPS   (overflow +0, missed ~45 of 128000)
RE_TOTAL = re.compile(
    r"total captured\s*:\s*(?P<sps>\d+)\s*SPS\s*\(overflow\s*\+(?P<overflow>\d+),"
    r"\s*missed\s*~(?P<missed>\d+)\s*of\s*(?P<expected>\d+)\)"
)
RE_OVERLAP = re.compile(r"overlap.*?:\s*(?P<overlap>\d+)/s")
# fast acq      : running   drops F/C = 0/0
RE_DROPS = re.compile(r"drops\s+F/C\s*=\s*(?P<fine>\d+)\s*/\s*(?P<coarse>\d+)")
#   #0 FINE    : OK   ODR=32000 SPS   status=MERR
RE_ADAQ = re.compile(r"#(?P<idx>\d)\s+(?P<role>\w+)\s*:\s*(?P<state>\w+)\s+ODR=(?P<odr>\d+)")


@dataclass
class Point:
    odr_ratio: int
    volt_odr_ratio: int
    expected_sps: int = 0
    total_sps: int = 0
    overflow: int = 0
    missed: int = 0
    overlap: int = 0
    drop_fine: int = 0
    drop_coarse: int = 0
    buses: list = field(default_factory=list)
    adaq_odr: dict = field(default_factory=dict)
    error: str = ""

    @property
    def per_ch_sps(self) -> int:
        return ADAQ_BASE_SPS // self.odr_ratio if self.odr_ratio else 0

    @property
    def capture_pct(self) -> float:
        return 100.0 * self.total_sps / self.expected_sps if self.expected_sps else 0.0

    @property
    def missed_pct(self) -> float:
        return 100.0 * self.missed / self.expected_sps if self.expected_sps else 0.0


def parse_faststat(text: str, point: Point) -> None:
    for m in RE_BUS.finditer(text):
        point.buses.append(
            {
                "bus": m.group("bus"),
                "name": m.group("name"),
                "sps": int(m.group("sps")),
                "edges": int(m.group("edges")),
                "overflow": int(m.group("overflow")),
                "missed": int(m.group("missed")),
            }
        )
    m = RE_TOTAL.search(text)
    if m:
        point.total_sps = int(m.group("sps"))
        point.overflow = int(m.group("overflow"))
        point.missed = int(m.group("missed"))
        point.expected_sps = int(m.group("expected"))
    m = RE_OVERLAP.search(text)
    if m:
        point.overlap = int(m.group("overlap"))


def parse_status(text: str, point: Point) -> None:
    m = RE_DROPS.search(text)
    if m:
        point.drop_fine = int(m.group("fine"))
        point.drop_coarse = int(m.group("coarse"))
    for m in RE_ADAQ.finditer(text):
        point.adaq_odr[m.group("role")] = int(m.group("odr"))


# ---------------------------------------------------------------------------
# Sweep
# ---------------------------------------------------------------------------
def measure(con: P4Console, odr: int, volt_odr: int, settle: float,
            repeats: int) -> Point:
    """Configure one ODR pair and measure it.

    `odr`/`voltodr` require acquisition to be stopped (the capture task holds
    the SPI bus for the whole session), so every point is a full stop /
    reprogram / start cycle. The first faststat after a start is discarded:
    it straddles the ramp-up and consistently under-reports.
    """
    point = Point(odr_ratio=odr, volt_odr_ratio=volt_odr)

    con.cmd("fast off", deadline=10.0)
    out = con.cmd(f"odr {odr}", deadline=10.0)
    if "err" in out.lower() or "invalid" in out.lower():
        point.error = f"odr {odr} rejected: {out.strip()[:160]}"
        con.cmd("fast on", deadline=10.0)
        return point
    out = con.cmd(f"voltodr {volt_odr}", deadline=10.0)
    if "err" in out.lower() or "invalid" in out.lower():
        point.error = f"voltodr {volt_odr} rejected: {out.strip()[:160]}"
        con.cmd("fast on", deadline=10.0)
        return point

    con.cmd("fast on", deadline=10.0)
    time.sleep(settle)

    con.cmd("faststat", deadline=6.0)  # discard the ramp-up window
    best = None
    for _ in range(repeats):
        probe = Point(odr_ratio=odr, volt_odr_ratio=volt_odr)
        parse_faststat(con.cmd("faststat", deadline=6.0), probe)
        # Keep the best window: transient scheduling noise (a log flush, a C6
        # link poll) only ever costs samples, so the maximum is the closest
        # estimate of the engine's real capability.
        if best is None or probe.total_sps > best.total_sps:
            best = probe
    if best is not None:
        point.buses = best.buses
        point.total_sps = best.total_sps
        point.overflow = best.overflow
        point.missed = best.missed
        point.expected_sps = best.expected_sps
        point.overlap = best.overlap

    parse_status(con.cmd("status", deadline=6.0), point)
    return point


def sweep(con: P4Console, odrs: list, volt_odr: int, settle: float,
          repeats: int) -> list:
    results = []
    for odr in odrs:
        v = volt_odr if volt_odr else odr
        sys.stderr.write(
            f"  ratio={odr:<4} ({ADAQ_BASE_SPS // odr:>7} SPS/ch) ... ")
        sys.stderr.flush()
        p = measure(con, odr, v, settle, repeats)
        results.append(p)
        if p.error:
            sys.stderr.write(f"SKIP ({p.error})\n")
        else:
            sys.stderr.write(
                f"{p.total_sps} SPS of {p.expected_sps} "
                f"({p.capture_pct:.1f}%), ovf {p.overflow}, missed {p.missed}\n"
            )
    return results


# ---------------------------------------------------------------------------
# Reporting
# ---------------------------------------------------------------------------
def print_table(results: list, title: str = "") -> None:
    if title:
        print(f"\n== {title} ==")
    print(f"{'ratio':>6} {'SPS/ch':>8} {'expect':>9} {'actual':>9} {'cap%':>7} "
          f"{'ovf':>8} {'missed':>8} {'miss%':>7} {'overlap':>8} {'dropF/C':>11}")
    print("-" * 96)
    for p in results:
        if p.error:
            print(f"{p.odr_ratio:>6} {'--':>8}   {p.error[:60]}")
            continue
        print(f"{p.odr_ratio:>6} {p.per_ch_sps:>8} {p.expected_sps:>9} "
              f"{p.total_sps:>9} {p.capture_pct:>6.1f}% {p.overflow:>8} "
              f"{p.missed:>8} {p.missed_pct:>6.2f}% {p.overlap:>8} "
              f"{p.drop_fine:>4}/{p.drop_coarse:<5}")


def compare(before_path: str, after_path: str) -> int:
    with open(before_path) as f:
        before = json.load(f)
    with open(after_path) as f:
        after = json.load(f)

    b_by = {p["odr_ratio"]: p for p in before["results"]}
    a_by = {p["odr_ratio"]: p for p in after["results"]}

    print(f"\n== {before_path} -> {after_path} ==")
    print(f"{'ratio':>6} {'before':>10} {'after':>10} {'delta':>10} {'delta%':>8} "
          f"{'ovf b/a':>12} {'missed b/a':>14}")
    print("-" * 78)
    regressed = 0
    for odr in sorted(set(b_by) | set(a_by)):
        b, a = b_by.get(odr), a_by.get(odr)
        if not b or not a or b.get("error") or a.get("error"):
            print(f"{odr:>6}   (missing or errored on one side)")
            continue
        d = a["total_sps"] - b["total_sps"]
        dp = 100.0 * d / b["total_sps"] if b["total_sps"] else 0.0
        flag = ""
        # Regression criteria. Measured run-to-run spread on this rig is about
        # +/-0.7%, so the captured-rate threshold sits just outside it.
        #
        # `missed` needs an ABSOLUTE floor, not just a ratio: at the healthy
        # ODRs it is a handful of samples out of ~192000, where a 9 -> 123
        # change is 0.06% of the stream and pure scheduling noise, yet trips
        # any purely relative test. Only count it once it is a real fraction
        # of the expected sample count.
        missed_floor = 0.005 * a["expected_sps"]        # 0.5% of the stream
        worse_missed = (a["missed"] > b["missed"] * 1.5 + 5
                        and a["missed"] > missed_floor)
        if dp < -1.5 or a["overflow"] > b["overflow"] or worse_missed:
            flag = "  <-- REGRESSION"
            regressed += 1
        print(f"{odr:>6} {b['total_sps']:>10} {a['total_sps']:>10} {d:>+10} "
              f"{dp:>+7.1f}% {b['overflow']:>5}/{a['overflow']:<6} "
              f"{b['missed']:>6}/{a['missed']:<7}{flag}")
    print()
    if regressed:
        print(f"FAIL: {regressed} ODR point(s) regressed.")
    else:
        print("OK: no capture-rate regression.")
    return 1 if regressed else 0


# ---------------------------------------------------------------------------
def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", help="P4 CLI serial port (default: autodetect)")
    ap.add_argument("--odr", default=",".join(str(o) for o in DEFAULT_ODRS),
                    help="comma-separated oversampling RATIOS (32..1024); "
                         "per-channel SPS = 8192000/ratio, so lower = faster")
    ap.add_argument("--volt-odr", type=int, default=0,
                    help="VOLTAGE oversampling ratio; 0 = track FINE/COARSE")
    ap.add_argument("--settle", type=float, default=1.5,
                    help="seconds to wait after `fast on` before measuring")
    ap.add_argument("--repeats", type=int, default=3,
                    help="faststat windows per point (best is kept)")
    ap.add_argument("--json", help="write results to this path")
    ap.add_argument("--compare", nargs=2, metavar=("BEFORE", "AFTER"),
                    help="compare two saved runs and exit")
    ap.add_argument("--restore-odr", type=int, default=32,
                    help="oversampling ratio to leave the board at when done")
    args = ap.parse_args()

    if args.compare:
        return compare(*args.compare)

    odrs = [int(x) for x in args.odr.split(",") if x.strip()]
    for o in odrs:
        if not 32 <= o <= 1024:
            return int(sys.stderr.write(f"ODR {o} out of range 32..1024\n") or 2)

    port = args.port or autodetect_port()
    sys.stderr.write(f"P4 console: {port}\n")
    con = P4Console(port)
    try:
        fw = con.cmd("status", deadline=6.0)
        m = re.search(r"fw\s*:\s*(\S+\s+\S+)", fw)
        firmware = m.group(1) if m else "unknown"
        sys.stderr.write(f"firmware: {firmware}\n")

        results = sweep(con, odrs, args.volt_odr, args.settle, args.repeats)

        # Leave the board in a sane, low-rate state rather than wherever the
        # sweep happened to end -- a board left at 1024k keeps a core pinned.
        con.cmd("fast off", deadline=10.0)
        con.cmd(f"odr {args.restore_odr}", deadline=10.0)
        con.cmd(f"voltodr {args.restore_odr}", deadline=10.0)
        con.cmd("fast on", deadline=10.0)
    finally:
        con.close()

    print_table(results, f"DAQ P4 capture sweep ({firmware})")

    if args.json:
        payload = {
            "firmware": firmware,
            "port": port,
            "timestamp": time.strftime("%Y-%m-%dT%H:%M:%S"),
            "results": [asdict(p) for p in results],
        }
        with open(args.json, "w") as f:
            json.dump(payload, f, indent=2)
        print(f"\nwrote {args.json}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
