#!/usr/bin/env python3
"""
daq_p4_sanity.py — verify the DAQ HAT is producing REAL measurements.

A throughput benchmark proves samples are arriving; it does not prove they mean
anything. A capture path can hit 100% of its expected rate while handing back
the same stale word forever — a stuck SPI read, a frozen DSP tail, or a ring
that is being re-read rather than advanced all look perfect to `faststat`.

This tool checks the other half:

  * VARIATION — over a burst of reads, are I/V/P actually changing? A real ADC
    at 24 bits always dithers by at least a few LSB. Zero variance across a
    burst is the signature of a stuck value, so it is reported as a failure
    rather than as suspiciously clean data.
  * RESPONSE  — drive the DUT supply between OFF and a setpoint and confirm the
    measured voltage follows. This is the only check here that proves the
    analog chain is connected to reality and not just self-consistently noisy.
  * RAW CODES — read the ADC conversion registers directly, so a failure can be
    localised to the ADC/SPI layer rather than the DSP tail above it.

The DUT supply is restored to OFF on exit, including on failure.

Usage:
    python3 tests/tools/daq_p4_sanity.py                 # 1.8 V, default
    python3 tests/tools/daq_p4_sanity.py --volts 3300 --samples 40
"""

from __future__ import annotations

import argparse
import os
import re
import statistics
import sys
import time

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", ".."))

from tests.lib.p4_console import P4Console, P4ConsoleUnavailable, find_p4_port  # noqa: E402


# I = -443.151 uA   V = 603.065 mV   P = -267.249 uW
RE_READ = re.compile(
    r"I\s*=\s*(?P<i>-?[\d.]+)\s*(?P<iu>[a-zA-Z]*A)\s+"
    r"V\s*=\s*(?P<v>-?[\d.]+)\s*(?P<vu>[a-zA-Z]*V)\s+"
    r"P\s*=\s*(?P<p>-?[\d.]+)\s*(?P<pu>[a-zA-Z]*W)"
)
# ADAQ #0 (FINE   ): raw=531566 (0x081C6E) ...
RE_RAW = re.compile(
    r"ADAQ\s*#(?P<idx>\d)\s*\((?P<role>\w+)\s*\):\s*raw=(?P<raw>-?\d+)"
)

SCALE = {
    "pA": 1e-12, "nA": 1e-9, "uA": 1e-6, "mA": 1e-3, "A": 1.0,
    "nV": 1e-9, "uV": 1e-6, "mV": 1e-3, "V": 1.0,
    "pW": 1e-12, "nW": 1e-9, "uW": 1e-6, "mW": 1e-3, "W": 1.0,
}


def si(value: str, unit: str) -> float:
    return float(value) * SCALE.get(unit, 1.0)


def burst(con: P4Console, n: int, gap: float):
    """Collect n (I, V, P) triples in SI units."""
    out = []
    for _ in range(n):
        m = RE_READ.search(con.cmd("read", deadline=15.0))
        if m:
            out.append((si(m.group("i"), m.group("iu")),
                        si(m.group("v"), m.group("vu")),
                        si(m.group("p"), m.group("pu"))))
        time.sleep(gap)
    return out


def raw_burst(con: P4Console, n: int, gap: float):
    """Collect raw ADC codes per role, to localise a stuck value."""
    codes = {}
    for _ in range(n):
        for m in RE_RAW.finditer(con.cmd("adaqraw", deadline=15.0)):
            codes.setdefault(m.group("role").strip(), []).append(int(m.group("raw")))
        time.sleep(gap)
    return codes


def describe(name: str, xs, unit: str) -> dict:
    if not xs:
        return {"name": name, "ok": False, "why": "no samples parsed"}
    uniq = len(set(xs))
    spread = max(xs) - min(xs)
    mean = statistics.fmean(xs)
    sd = statistics.pstdev(xs) if len(xs) > 1 else 0.0
    ok = uniq > 1
    print("  %-8s n=%-3d unique=%-3d mean=%12.6g %-3s  sd=%10.4g  "
          "span=%10.4g  %s" % (
              name, len(xs), uniq, mean, unit, sd, spread,
              "ok" if ok else "STUCK"))
    return {"name": name, "ok": ok, "unique": uniq, "mean": mean,
            "sd": sd, "span": spread}


def linearity(con: P4Console, setpoints, n: int, gap: float, settle: float):
    """Step the supply across several setpoints and check the readback tracks.

    Variation alone cannot distinguish a working meter from a noisy one wired to
    nothing. Driving a known sequence and requiring the reading to follow it is
    what actually proves the analog chain is measuring the rail — and the
    residual from a straight-line fit exposes gain/offset error that any single
    setpoint would hide.
    """
    print("\n== 6. Voltage linearity sweep ==")
    print("  %10s %12s %10s" % ("set_V", "measured_V", "err_%"))
    xs, ys = [], []
    con.cmd("vdut on", deadline=15.0)
    for mv in setpoints:
        con.cmd("vdut %d" % mv, deadline=15.0)
        time.sleep(settle)
        vs = [x[1] for x in burst(con, n, gap)]
        if not vs:
            continue
        target = mv / 1000.0
        meas = statistics.fmean(vs)
        xs.append(target)
        ys.append(meas)
        print("  %10.3f %12.4f %+9.2f%%" % (
            target, meas, 100.0 * (meas - target) / target))
    if len(xs) < 3:
        return ["linearity sweep produced too few points"]

    # Least-squares gain/offset, then worst residual as % of full scale.
    n_pts = len(xs)
    mx, my = statistics.fmean(xs), statistics.fmean(ys)
    sxx = sum((x - mx) ** 2 for x in xs)
    sxy = sum((x - mx) * (y - my) for x, y in zip(xs, ys, strict=True))
    gain = sxy / sxx if sxx else 0.0
    offset = my - gain * mx
    resid = [y - (gain * x + offset) for x, y in zip(xs, ys, strict=True)]
    span = max(xs) - min(xs)
    worst = max(abs(r) for r in resid)
    print("  fit: gain=%.4f  offset=%+.4f V  worst residual=%.4f V (%.2f%% FS)"
          % (gain, offset, worst, 100.0 * worst / span if span else 0.0))

    problems = []
    if not (0.9 <= gain <= 1.1):
        problems.append("voltage gain %.4f is outside 0.90..1.10" % gain)
    if span and worst / span > 0.05:
        problems.append("voltage linearity residual %.2f%% FS exceeds 5%%"
                        % (100.0 * worst / span))
    if n_pts and len(set(round(y, 3) for y in ys)) < n_pts:
        problems.append("readback did not change for every setpoint")
    return problems


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port")
    ap.add_argument("--volts", type=int, default=1800,
                    help="DUT supply setpoint in millivolts")
    ap.add_argument("--samples", type=int, default=25)
    ap.add_argument("--gap", type=float, default=0.12,
                    help="delay between reads, seconds")
    ap.add_argument("--settle", type=float, default=1.5,
                    help="delay after switching the DUT supply, seconds")
    ap.add_argument("--sweep", default="2000,3300,5000,8000,12000",
                    help="linearity setpoints in mV; empty string to skip. "
                         "Hardware clamps to 1.76..19.94 V.")
    args = ap.parse_args()

    port = args.port or find_p4_port()
    if not port:
        print("No P4 DAQ console found.", file=sys.stderr)
        return 2

    failures = []
    warnings = []
    try:
        with P4Console(port) as con:
            if not con.alive():
                print("P4 console at %s did not answer" % port, file=sys.stderr)
                return 2
            try:
                print("== 1. DUT supply OFF — baseline ==")
                con.cmd("vdut off", deadline=15.0)
                time.sleep(args.settle)
                off = burst(con, args.samples, args.gap)
                i_off = [x[0] for x in off]
                v_off = [x[1] for x in off]
                for r in (describe("I", i_off, "A"), describe("V", v_off, "V")):
                    if not r["ok"]:
                        failures.append("baseline %s is stuck" % r["name"])

                print("\n== 2. Raw ADC conversion codes (supply OFF) ==")
                codes = raw_burst(con, min(args.samples, 12), args.gap)
                for role, xs in sorted(codes.items()):
                    r = describe(role, xs, "lsb")
                    if not r["ok"]:
                        failures.append("raw ADC %s is stuck" % role)

                print("\n== 3. DUT supply ON at %d mV ==" % args.volts)
                print(con.cmd("vdut %d" % args.volts, deadline=15.0).strip())
                con.cmd("vdut on", deadline=15.0)
                time.sleep(args.settle)
                on = burst(con, args.samples, args.gap)
                i_on = [x[0] for x in on]
                v_on = [x[1] for x in on]
                r_i = describe("I", i_on, "A")
                r_v = describe("V", v_on, "V")
                for r in (r_i, r_v):
                    if not r["ok"]:
                        failures.append("supply-on %s is stuck" % r["name"])

                print("\n== 4. Raw ADC conversion codes (supply ON) ==")
                codes_on = raw_burst(con, min(args.samples, 12), args.gap)
                for role, xs in sorted(codes_on.items()):
                    r = describe(role, xs, "lsb")
                    if not r["ok"]:
                        failures.append("raw ADC %s is stuck (supply on)" % role)

                print("\n== 5. Response to the supply step ==")
                v_mean_off = statistics.fmean(v_off) if v_off else 0.0
                v_mean_on = statistics.fmean(v_on) if v_on else 0.0
                target = args.volts / 1000.0
                step = v_mean_on - v_mean_off
                print("  V(off) = %.4f V   V(on) = %.4f V   step = %+.4f V   "
                      "target = %.3f V" % (v_mean_off, v_mean_on, step, target))
                # Generous tolerance: this asks "did the rail actually move to
                # roughly the right place", not "is the calibration good".
                if abs(v_mean_on - target) > max(0.25 * target, 0.15):
                    failures.append(
                        "V(on) %.4f V is not within 25%% of the %.3f V setpoint"
                        % (v_mean_on, target))
                if abs(step) < 0.05:
                    failures.append(
                        "V did not respond to the supply step (%.4f V)" % step)

                # The FINE/COARSE codes must differ from each other; identical
                # streams would mean one bus is being read for both.
                if len(codes_on) >= 2:
                    fine = codes_on.get("FINE", [])
                    coarse = codes_on.get("COARSE", [])
                    if fine and coarse and fine == coarse:
                        failures.append(
                            "FINE and COARSE returned identical code streams")

                if args.sweep.strip():
                    pts = [int(x) for x in args.sweep.split(",") if x.strip()]
                    failures.extend(
                        linearity(con, pts, max(6, args.samples // 3),
                                  args.gap, args.settle))

                # Zero-current offset. Reported as a WARNING, not a failure:
                # an uncalibrated unit is a real finding but not a broken data
                # path, and only `cal` (which writes NVS) can fix it.
                print("\n== 7. Zero-current offset (supply off, no load) ==")
                con.cmd("vdut off", deadline=15.0)
                time.sleep(args.settle)
                i_zero = [x[0] for x in burst(con, args.samples, args.gap)]
                cal = con.cmd("cal status", deadline=20.0)
                m = re.search(r"ical\s+have=(?P<have>\d+)\s+points=(?P<pts>\d+)", cal)
                have_ical = bool(m and m.group("have") == "1" and int(m.group("pts")) > 0)
                if i_zero:
                    z = statistics.fmean(i_zero)
                    print("  I(no load) = %+.3f uA   current cal table: %s"
                          % (z * 1e6, "present" if have_ical else "EMPTY"))
                    if abs(z) > 1e-5:
                        warnings.append(
                            "zero-current offset %+.1f uA with no load%s"
                            % (z * 1e6,
                               " — current calibration (`cal i`) has never been run"
                               if not have_ical else ""))
                    elif not have_ical:
                        warnings.append("current calibration table is empty")
            finally:
                con.cmd("vdut off", deadline=15.0)
                print("\n(DUT supply returned to OFF)")
    except P4ConsoleUnavailable as exc:
        print("P4 console unavailable: %s" % exc, file=sys.stderr)
        return 2

    print("\n" + "=" * 70)
    for w in warnings:
        print("WARN  %s" % w)
    if failures:
        print("FAIL (%d)" % len(failures))
        for f in failures:
            print("  - %s" % f)
        return 1
    print("PASS — samples vary, raw codes vary, and V tracked the supply step")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
