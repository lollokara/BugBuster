#!/usr/bin/env python3
"""
daq_p4_profile.py — full pipeline profile + ODR sweep for the ESP32-P4 DAQ HAT.

WHY THIS EXISTS, next to daq_p4_bench.py:

`daq_p4_bench.py` answers "how many samples/s survived at each ODR". That is a
throughput curve, and it is the right tool for regression-checking a firmware
change. What it cannot tell you is WHERE the cycles went, so when the curve
bends you still have to guess whether the capture core ran out of SPI budget,
the consumer core ran out of DSP budget, or the wire push started blocking.
Those three have completely different fixes.

This tool reads the firmware's per-stage cycle profiler (`perf` CLI command,
src/perf/daq_perf.h) at every ODR point, so each throughput number arrives with
a cycle budget attached. It then does the arithmetic that turns those budgets
into decisions:

  * per-core utilisation, split into CPU work vs blocked/idle-spin wall time
  * ns per sample for every pipeline stage, so the expensive ones are ranked
  * which core is the binding constraint at this ODR, and by how much
  * the projected maximum sustainable rate from the measured per-sample costs,
    which is the number that says how much headroom is actually left

IMPORTANT — `odr` is an ADC OVERSAMPLING RATIO, not a rate. Per-channel sample
rate is 8_192_000 / ratio, so LOWER ratio = HIGHER rate (see daq_p4_bench.py).

CAVEAT that changes the numbers: with no USB host attached,
`usb_stream.c:emit_frame_inplace()` early-returns, so the `fast.wire` stage is a
stub and the consumer core looks far cheaper than it is in real use. Run
`daq_usb_stream_bench.py` concurrently, or pass --note, when that matters.

Usage:
    # one deep profile at the current ODR
    python3 tests/tools/daq_p4_profile.py --once

    # sweep, saved for later comparison
    python3 tests/tools/daq_p4_profile.py --json before.json
    python3 tests/tools/daq_p4_profile.py --compare before.json after.json
"""

from __future__ import annotations

import argparse
import json
import os
import re
import sys
import time
from dataclasses import dataclass, field, asdict

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", ".."))

from tests.lib.p4_console import P4Console, P4ConsoleUnavailable, find_p4_port  # noqa: E402


ADAQ_BASE_SPS = 8_192_000
DEFAULT_ODRS = [1024, 512, 256, 128, 64, 32]

# stage   cpu k      count    avg_ns    min_ns    max_ns    tot_ms   %core  drop
RE_STAGE = re.compile(
    r"^(?P<name>[a-z]+\.[a-z_]+)\s+(?P<cpu>\d)\s+(?P<kind>[CW])\s+"
    r"(?P<count>\d+)\s+(?P<avg>[\d.]+)\s+(?P<min>[\d.]+)\s+(?P<max>[\d.]+)\s+"
    r"(?P<tot_ms>[\d.]+)\s+(?P<pct>[\d.]+)%\s+(?P<drop>\d+)",
    re.M,
)
# task               prio core state    window_us   %core stack_hwm
RE_TASK = re.compile(
    r"^(?P<name>\S+)\s+(?P<prio>\d+)\s+(?P<core>\d+|any)\s+(?P<state>\w+)\s+"
    r"(?P<us>\d+)\s+(?P<pct>[\d.]+)%\s+(?P<hwm>\d+)",
    re.M,
)
RE_WINDOW = re.compile(
    r"window\s+(?P<sec>[\d.]+)\s*s\s*@\s*(?P<mhz>\d+)\s*MHz\s+"
    r"probe overhead\s+(?P<oh>\d+)"
)
RE_TOTAL = re.compile(
    r"total captured\s*:\s*(?P<sps>\d+)\s*SPS\s*\(overflow\s*\+(?P<overflow>\d+),"
    r"\s*missed\s*~(?P<missed>\d+)\s*of\s*(?P<expected>\d+)\)"
)
RE_BUS = re.compile(
    r"Bus\s+(?P<bus>\S+)\s+(?P<name>[\w+]+)\s*:\s*(?P<sps>\d+)\s*SPS\s+"
    r"edges\s+(?P<edges>\d+)/s\s+overflow\s*\+(?P<overflow>\d+)\s+missed\s*~(?P<missed>\d+)"
)
RE_DROPS = re.compile(r"drops\s+F/C\s*=\s*(?P<fine>\d+)\s*/\s*(?P<coarse>\d+)")
RE_HEAP = re.compile(r"heap internal free:\s*(?P<free>\d+)\s*B\s*\(largest\s*(?P<largest>\d+)")
RE_OPT = re.compile(r"compiler\s*:\s*(?P<opt>\S+)")
RE_DECIM = re.compile(r"dsp_decim\s*:\s*(?P<dsp>\d+)\s+wave_decim:\s*(?P<wave>\d+)\s+volt_decim:\s*(?P<volt>\d+)")


@dataclass
class Point:
    odr_ratio: int
    window_s: float = 0.0
    cpu_mhz: int = 360
    probe_overhead_cyc: int = 0
    total_sps: int = 0
    expected_sps: int = 0
    overflow: int = 0
    missed: int = 0
    drop_fine: int = 0
    drop_coarse: int = 0
    stages: dict = field(default_factory=dict)
    tasks: dict = field(default_factory=dict)
    buses: list = field(default_factory=list)
    heap_free: int = 0
    heap_largest: int = 0
    optimization: str = ""
    dsp_decim: int = 0
    wave_decim: int = 0
    volt_decim: int = 0
    error: str = ""

    @property
    def per_ch_sps(self) -> int:
        return ADAQ_BASE_SPS // self.odr_ratio if self.odr_ratio else 0

    @property
    def capture_pct(self) -> float:
        return 100.0 * self.total_sps / self.expected_sps if self.expected_sps else 0.0

    def cpu_busy(self, core: int) -> float:
        """Percent of `core` spent in measured CPU stages (excludes W stages).

        Uses only the TOP-LEVEL stage of each core so nested stages are not
        double counted: cap.pass already contains scan/drain/begin/end, and
        fast.loop already contains readA/readB/pair/emit.
        """
        top = "cap.pass" if core == 1 else "fast.loop"
        s = self.stages.get(top)
        return s["pct"] if s else 0.0

    def spin_pct(self) -> float:
        s = self.stages.get("cap.spin")
        return s["pct"] if s else 0.0


def parse_perf(text: str, p: Point) -> None:
    m = RE_WINDOW.search(text)
    if m:
        p.window_s = float(m.group("sec"))
        p.cpu_mhz = int(m.group("mhz"))
        p.probe_overhead_cyc = int(m.group("oh"))
    for m in RE_STAGE.finditer(text):
        p.stages[m.group("name")] = {
            "core": int(m.group("cpu")),
            "kind": m.group("kind"),
            "count": int(m.group("count")),
            "avg_ns": float(m.group("avg")),
            "min_ns": float(m.group("min")),
            "max_ns": float(m.group("max")),
            "tot_ms": float(m.group("tot_ms")),
            "pct": float(m.group("pct")),
            "drop": int(m.group("drop")),
        }
    for m in RE_TASK.finditer(text):
        # The stage table's header row also matches the task shape; stages all
        # contain a dot, task names never do.
        if "." in m.group("name"):
            continue
        p.tasks[m.group("name")] = {
            "prio": int(m.group("prio")),
            "core": m.group("core"),
            "state": m.group("state"),
            "window_us": int(m.group("us")),
            "pct": float(m.group("pct")),
            "stack_hwm": int(m.group("hwm")),
        }
    m = RE_HEAP.search(text)
    if m:
        p.heap_free = int(m.group("free"))
        p.heap_largest = int(m.group("largest"))
    m = RE_OPT.search(text)
    if m:
        p.optimization = m.group("opt")
    m = RE_DECIM.search(text)
    if m:
        p.dsp_decim = int(m.group("dsp"))
        p.wave_decim = int(m.group("wave"))
        p.volt_decim = int(m.group("volt"))


def parse_faststat(text: str, p: Point) -> None:
    m = RE_TOTAL.search(text)
    if m:
        p.total_sps = int(m.group("sps"))
        p.overflow = int(m.group("overflow"))
        p.missed = int(m.group("missed"))
        p.expected_sps = int(m.group("expected"))
    for m in RE_BUS.finditer(text):
        p.buses.append({
            "bus": m.group("bus"),
            "name": m.group("name"),
            "sps": int(m.group("sps")),
            "edges": int(m.group("edges")),
            "overflow": int(m.group("overflow")),
            "missed": int(m.group("missed")),
        })


def parse_status(text: str, p: Point) -> None:
    m = RE_DROPS.search(text)
    if m:
        p.drop_fine = int(m.group("fine"))
        p.drop_coarse = int(m.group("coarse"))


def measure(con: P4Console, odr: int, settle: float, window: float,
            set_odr: bool) -> Point:
    """Profile one ODR point: reprogram, settle, then sample both instruments."""
    p = Point(odr_ratio=odr)
    if set_odr:
        con.cmd("fast off", deadline=15.0)
        out = con.cmd("odr %d" % odr, deadline=15.0)
        if "err" in out.lower() or "invalid" in out.lower():
            p.error = "odr %d rejected: %s" % (odr, out.strip()[:120])
            con.cmd("fast on", deadline=15.0)
            return p
        con.cmd("fast on", deadline=15.0)
        time.sleep(settle)

    # faststat burns 1 s of its own; take it first so the perf window brackets
    # steady state rather than the ramp after a reprogram.
    parse_faststat(con.cmd("faststat", deadline=15.0), p)
    con.cmd("perf on", deadline=15.0)
    time.sleep(window)
    parse_perf(con.cmd("perf show", deadline=40.0), p)
    con.cmd("perf off", deadline=15.0)
    parse_status(con.cmd("status", deadline=15.0), p)
    parse_faststat(con.cmd("faststat", deadline=15.0), p)
    return p


# ---------------------------------------------------------------------------
# Reporting
# ---------------------------------------------------------------------------
def print_point(p: Point, verbose: bool) -> None:
    if p.error:
        print("  ratio %-5d ERROR: %s" % (p.odr_ratio, p.error))
        return
    print("\n=== ODR ratio %d  (%d SPS/ch, %d SPS expected aggregate) ===" % (
        p.odr_ratio, p.per_ch_sps, p.expected_sps))
    print("  captured %d SPS (%.1f%% of expected)  overflow +%d  missed ~%d  "
          "drops F/C %d/%d" % (p.total_sps, p.capture_pct, p.overflow,
                               p.missed, p.drop_fine, p.drop_coarse))
    c1, c0 = p.cpu_busy(1), p.cpu_busy(0)
    print("  core1 capture  : %5.1f%% CPU   %5.1f%% idle-spin" % (c1, p.spin_pct()))
    print("  core0 consumer : %5.1f%% CPU (daq_fast task %.1f%%)" % (
        c0, p.tasks.get("daq_fast", {}).get("pct", 0.0)))

    # Per-sample cost is the number that projects to a ceiling; %core alone
    # hides that a stage got cheaper only because fewer samples arrived.
    emitted = p.stages.get("fast.emit", {}).get("count", 0)
    if emitted and p.window_s:
        print("  fused samples  : %d in %.2f s = %.0f/s" % (
            emitted, p.window_s, emitted / p.window_s))

    if verbose and p.stages:
        print("  %-13s %4s %1s %10s %9s %8s %6s" % (
            "stage", "core", "k", "count", "avg_ns", "tot_ms", "%core"))
        for name, s in p.stages.items():
            print("  %-13s %4d %1s %10d %9.0f %8.1f %5.1f%%" % (
                name, s["core"], s["kind"], s["count"], s["avg_ns"],
                s["tot_ms"], s["pct"]))


def bottleneck(p: Point) -> str:
    """Name the binding constraint, and say what evidence points at it."""
    c1, c0, spin = p.cpu_busy(1), p.cpu_busy(0), p.spin_pct()
    if p.overflow > 0:
        return ("CONSUMER (core 0): ring overflow +%d — the capture task fills "
                "faster than daq_fast drains" % p.overflow)
    if p.missed > 0.02 * max(p.expected_sps, 1):
        return ("CAPTURE (core 1): missed ~%d/s (%.1f%%) with no overflow — DRDY "
                "edges go unserviced, the producer itself cannot keep up"
                % (p.missed, 100.0 * p.missed / max(p.expected_sps, 1)))
    if spin > 25.0:
        return "HEADROOM: core 1 idle-spins %.0f%% of the time" % spin
    if c0 > 80.0:
        return "CONSUMER (core 0) approaching saturation at %.0f%% CPU" % c0
    return "no single constraint; core1 %.0f%% / core0 %.0f%% CPU" % (c1, c0)


def project_ceiling(p: Point) -> str:
    """Extrapolate a sustainable ceiling from measured per-sample costs."""
    out = []
    cap = p.stages.get("cap.pass")
    if cap and cap["avg_ns"] > 0 and p.window_s:
        passes_s = cap["count"] / p.window_s
        samples_per_pass = (p.total_sps / passes_s) if passes_s else 0
        max_passes = 1e9 / cap["avg_ns"]
        out.append("core1 ceiling ~%.0f kSPS aggregate (%.2f us/pass, "
                   "%.2f samples/pass)" % (
                       max_passes * samples_per_pass / 1000.0,
                       cap["avg_ns"] / 1000.0, samples_per_pass))
    loop = p.stages.get("fast.loop")
    emit = p.stages.get("fast.emit")
    if loop and emit and emit["count"] and p.window_s:
        # Cost of the consumer per FUSED sample, which is what its rate is
        # denominated in.
        ns_per_fused = loop["tot_ms"] * 1e6 / emit["count"]
        out.append("core0 ceiling ~%.0f kSPS fused (%.2f us/fused sample)" % (
            1e9 / ns_per_fused / 1000.0, ns_per_fused / 1000.0))
    return "; ".join(out) if out else "n/a"


def summarize(points: list) -> None:
    good = [p for p in points if not p.error and p.expected_sps]
    if not good:
        print("\nno usable points")
        return
    print("\n" + "=" * 100)
    print("SUMMARY")
    print("=" * 100)
    print("%-6s %9s %9s %7s %9s %8s %8s %8s %8s" % (
        "ratio", "expect", "captured", "cap%", "missed/s", "ovfl/s",
        "c1_cpu%", "c1_spin%", "c0_cpu%"))
    for p in good:
        print("%-6d %9d %9d %6.1f%% %9d %8d %7.1f%% %7.1f%% %7.1f%%" % (
            p.odr_ratio, p.expected_sps, p.total_sps, p.capture_pct,
            p.missed, p.overflow, p.cpu_busy(1), p.spin_pct(), p.cpu_busy(0)))

    print("\nBOTTLENECK PER POINT")
    for p in good:
        print("  ratio %-5d %s" % (p.odr_ratio, bottleneck(p)))

    print("\nPROJECTED CEILINGS (from measured per-sample cost)")
    for p in good:
        print("  ratio %-5d %s" % (p.odr_ratio, project_ceiling(p)))

    # Rank stages by cost per sample at the fastest point that stayed healthy.
    healthy = [p for p in good if p.capture_pct > 98.0]
    ref = min(healthy, key=lambda p: p.odr_ratio) if healthy else good[0]
    print("\nSTAGE COST RANKING at ratio %d (%d SPS/ch) — where the time goes"
          % (ref.odr_ratio, ref.per_ch_sps))
    ranked = sorted(
        (s for s in ref.stages.items() if s[1]["kind"] == "C"),
        key=lambda kv: kv[1]["pct"], reverse=True)
    for name, s in ranked:
        print("  %-13s core%d  %6.1f%% of core   %8.0f ns x %d" % (
            name, s["core"], s["pct"], s["avg_ns"], s["count"]))
    if ref.optimization:
        print("\n  build optimization: %s" % ref.optimization)


def compare(before_path: str, after_path: str) -> None:
    with open(before_path) as f:
        bj = json.load(f)
    with open(after_path) as f:
        aj = json.load(f)
    before = {p["odr_ratio"]: p for p in bj["points"]}
    after = {p["odr_ratio"]: p for p in aj["points"]}
    print("before: %s" % (bj.get("note") or before_path))
    print("after : %s\n" % (aj.get("note") or after_path))

    print("THROUGHPUT")
    print("%-6s %11s %11s %8s %10s %10s" % (
        "ratio", "sps_before", "sps_after", "delta%", "miss_before", "miss_after"))
    for ratio in sorted(set(before) & set(after), reverse=True):
        b, a = before[ratio], after[ratio]
        bs, as_ = b.get("total_sps", 0), a.get("total_sps", 0)
        d = 100.0 * (as_ - bs) / bs if bs else 0.0
        print("%-6d %11d %11d %+7.1f%% %10d %10d" % (
            ratio, bs, as_, d, b.get("missed", 0), a.get("missed", 0)))

    # Per-stage ns/sample is the honest comparison: %core moves when throughput
    # moves, so only the per-sample cost isolates the change itself.
    print("\nPER-STAGE COST (avg ns) at the fastest common ratio")
    common = sorted(set(before) & set(after))
    if not common:
        return
    ratio = common[0]
    bstg = before[ratio].get("stages", {})
    astg = after[ratio].get("stages", {})
    print("  ratio %d" % ratio)
    print("  %-13s %10s %10s %9s" % ("stage", "before_ns", "after_ns", "delta%"))
    for name in sorted(set(bstg) & set(astg)):
        if bstg[name]["kind"] != "C":
            continue
        bn, an = bstg[name]["avg_ns"], astg[name]["avg_ns"]
        d = 100.0 * (an - bn) / bn if bn else 0.0
        print("  %-13s %10.0f %10.0f %+8.1f%%" % (name, bn, an, d))


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", help="P4 console port (auto-detected if omitted)")
    ap.add_argument("--odr", help="comma-separated oversampling ratios")
    ap.add_argument("--once", action="store_true",
                    help="profile the CURRENT ODR only, without reprogramming")
    ap.add_argument("--window", type=float, default=5.0,
                    help="perf sampling window per point, seconds")
    ap.add_argument("--settle", type=float, default=1.5,
                    help="delay after reprogramming an ODR, seconds")
    ap.add_argument("--json", help="write results here")
    ap.add_argument("--compare", nargs=2, metavar=("BEFORE", "AFTER"))
    ap.add_argument("--verbose", action="store_true",
                    help="print the full stage table for every point")
    ap.add_argument("--note", default="", help="free-text note stored in --json")
    args = ap.parse_args()

    if args.compare:
        compare(*args.compare)
        return 0

    port = args.port or find_p4_port()
    if not port:
        print("No P4 DAQ console found. Pass --port explicitly.", file=sys.stderr)
        return 2

    odrs = ([int(x) for x in args.odr.split(",")] if args.odr else DEFAULT_ODRS)

    points = []
    try:
        with P4Console(port) as con:
            if not con.alive():
                print("P4 console at %s did not answer" % port, file=sys.stderr)
                return 2
            if args.once:
                p = measure(con, 0, args.settle, args.window, set_odr=False)
                # Recover the real ratio from the reported expectation.
                if p.buses:
                    per_ch = p.buses[0]["sps"]
                    p.odr_ratio = round(ADAQ_BASE_SPS / per_ch) if per_ch else 0
                points.append(p)
                print_point(p, verbose=True)
            else:
                for odr in odrs:
                    p = measure(con, odr, args.settle, args.window, set_odr=True)
                    points.append(p)
                    print_point(p, args.verbose)
    except P4ConsoleUnavailable as exc:
        print("P4 console unavailable: %s" % exc, file=sys.stderr)
        return 2

    summarize(points)

    if args.json:
        with open(args.json, "w") as f:
            json.dump({"note": args.note, "points": [asdict(p) for p in points]},
                      f, indent=2)
        print("\nwrote %s" % args.json)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
