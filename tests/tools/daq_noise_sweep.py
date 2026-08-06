#!/usr/bin/env python3
"""
daq_noise_sweep.py — measure DAQ HAT noise across every ADC filter/decimation
setting and against Super Resolution.

WHY THIS IS NOT JUST "print the standard deviation"
---------------------------------------------------
Comparing raw sigma between two settings is misleading, because a slower
setting is quieter almost for free: for white noise, sigma falls as
sqrt(bandwidth). A mode running 32x slower "wins" by ~2.5 bits without its
filter doing anything clever. Three numbers are therefore reported per setting:

  sigma        what a user actually sees on screen at that setting
  density      sigma / sqrt(BW)  -- bandwidth-normalised. FLAT across settings
               means the chain is white-noise limited and every setting is
               behaving ideally. A setting whose density is HIGHER than its
               neighbours is adding noise beyond the bandwidth change.
  bits vs ref  log2(sigma_ref / sigma), i.e. resolution gained over the
               reference setting.

and, for Super Resolution specifically, the comparison that actually validates
the feature:

  SR vs naive  SR's FIR decimator versus plain keep-1-of-N decimation of the
               reference stream to the SAME output rate. Naive decimation does
               no anti-alias filtering, so broadband noise folds straight back
               into the passband and sigma barely improves. This is what the
               firmware did before SR existed, so it is the honest "before".

MEASUREMENT HYGIENE
-------------------
  * sigma is computed on a LINEARLY DETRENDED window. Thermal drift or a slow
    load change is not noise, and would otherwise dominate the long, slow
    captures (an SR window is 30x longer in wall-clock terms per sample).
  * samples flagged SETTLING or SATURATED in the WAVE_I meta byte are dropped,
    and the first `--settle` seconds after every reconfiguration are discarded:
    changing filter/decimation restarts acquisition and the ADAQ needs 3-7
    output periods to settle (datasheet Table 5), plus the SR FIR has its own
    group delay.
  * autoranging is left to the caller. If the range hunts mid-capture the
    range bits in the meta byte will vary; that is reported as `rng` and any
    setting showing more than one range is flagged, because a range change is
    a step, not noise.

The DUT should be in a STATIC state for the whole sweep (steady load or supply
off). Anything that moves during the sweep shows up as noise and invalidates
the comparison.

Transport: stream over the P4 USB-HS vendor-bulk link (VID 0x303A/PID 0x4001);
configuration over the S3 HTTP API, because filter/decimation/SR live in the
settings registry on the S3 control plane, not in the P4's USB command set.

Usage:
    python tests/tools/daq_noise_sweep.py --host 192.168.3.9 --token <tok>
    python tests/tools/daq_noise_sweep.py --host ... --token ... --quick
    python tests/tools/daq_noise_sweep.py --host ... --token ... --json out.json
    python tests/tools/daq_noise_sweep.py --compare before.json after.json

Requires pyusb + libusb for the stream half.
"""

from __future__ import annotations

import argparse
import json
import math
import os
import sys
import time
import urllib.error
import urllib.request

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", ".."))

from tests.lib.daq_link import DaqLink, DaqLinkUnavailable
from tests.lib.daq_records import meta_range, meta_saturated, meta_settling

# ADAQ filter codes on the acq_config wire. These are the ADAQ7769-1's own
# register values (Firmware/ESP32/src/hat/hat.h HAT_ACQ_FILTER_*), NOT the
# registry's 0=wideband/1=sinc5/2=sinc3 enum -- they are different numberings
# and mixing them silently selects the wrong filter.
F_SINC5, F_SINC5_X8, F_SINC5_X16, F_SINC3, F_WIDEBAND = 0, 1, 2, 3, 4
FILTER_NAMES = {F_SINC5: "Sinc5", F_SINC5_X8: "Sinc5x8", F_SINC5_X16: "Sinc5x16",
                F_SINC3: "Sinc3", F_WIDEBAND: "Wideband"}

# adc_dec is dual-purpose: an ADAQ_DEC_* enum index for every filter except
# SINC3, where it carries (decimation / 32) instead.
DEC_ENUM = [(0, "x32"), (1, "x64"), (2, "x128"), (3, "x256"), (4, "x512"), (5, "x1024")]
# adc_dec carries (decimation / 32) for Sinc3 and is a uint8_t on the wire, so
# 255 (= x8160) is the hard ceiling -- x8192 would need 256 and is rejected.
SINC3_DECS = [(1, "x32"), (2, "x64"), (4, "x128"), (8, "x256"), (16, "x512"),
              (32, "x1024"), (128, "x4096"), (255, "x8160 (max)")]

# Reference point the "bits vs ref" column is measured against: the shipping
# default (wideband, x256).
REF_FILTER, REF_DEC = F_WIDEBAND, 3


def build_plan(quick: bool, with_fixed: bool = False):
    """(filter_code, adc_dec, label) for every setting to visit."""
    plan = []
    if quick:
        for d, dn in [DEC_ENUM[0], DEC_ENUM[3], DEC_ENUM[5]]:
            plan.append((F_WIDEBAND, d, f"Wideband {dn}"))
            plan.append((F_SINC5, d, f"Sinc5 {dn}"))
        for d, dn in [SINC3_DECS[3], SINC3_DECS[5]]:
            plan.append((F_SINC3, d, f"Sinc3 {dn}"))
        return plan
    for d, dn in DEC_ENUM:
        plan.append((F_WIDEBAND, d, f"Wideband {dn}"))
    for d, dn in DEC_ENUM:
        plan.append((F_SINC5, d, f"Sinc5 {dn}"))
    for d, dn in SINC3_DECS:
        plan.append((F_SINC3, d, f"Sinc3 {dn}"))
    # Sinc5x8 / Sinc5x16 ignore adc_dec (fixed fMOD/8 and /16), so probe once.
    #
    # OFF BY DEFAULT: at 1.024 MSPS / 512 kSPS these overrun the pipeline, and
    # every setting visited AFTER them comes back corrupted -- wrong sample
    # yield, and a WAVE_I header rate that disagrees with actual delivery.
    # Restoring the 24-bit interface format on exit (adaq7769.c, 2026-08-06)
    # fixed one cause but not all of it. Including them silently poisons the
    # rest of the sweep, so they are opt-in via --with-fixed until the
    # remaining recovery path is understood.
    if with_fixed:
        plan.append((F_SINC5_X8, 0, "Sinc5x8 (fixed)"))
        plan.append((F_SINC5_X16, 0, "Sinc5x16 (fixed)"))
    return plan


# ---------------------------------------------------------------------------
# S3 control plane
# ---------------------------------------------------------------------------
def _post(host: str, token: str, path: str, body: dict, timeout: float = 20.0):
    data = json.dumps(body).encode()
    req = urllib.request.Request(f"http://{host}{path}", data=data, method="POST",
                                 headers={"Content-Type": "application/json",
                                          "X-BugBuster-Admin-Token": token})
    try:
        with urllib.request.urlopen(req, timeout=timeout) as r:
            return json.loads(r.read().decode() or "{}")
    except urllib.error.HTTPError as e:
        return {"error": f"HTTP {e.code}: {e.read().decode()[:160]}"}
    except Exception as e:                                    # noqa: BLE001
        return {"error": f"{type(e).__name__}: {e}"}


def _get(host: str, token: str, path: str, timeout: float = 10.0):
    req = urllib.request.Request(f"http://{host}{path}",
                                 headers={"X-BugBuster-Admin-Token": token})
    try:
        with urllib.request.urlopen(req, timeout=timeout) as r:
            return json.loads(r.read().decode() or "{}")
    except urllib.error.HTTPError as e:
        return {"error": f"HTTP {e.code}: {e.read().decode()[:160]}"}
    except Exception as e:                                    # noqa: BLE001
        return {"error": f"{type(e).__name__}: {e}"}


def set_acq(host, token, filt, dec):
    return _post(host, token, "/api/daq/acq_config",
                 {"filter": filt, "adc_dec": dec})


def set_sr(host, token, on: bool):
    if on:
        return _post(host, token, "/api/daq/acq_config", {"sr_mode": True})
    # Leaving SR restores the reference setting explicitly; sr_mode false alone
    # would hand control back to whatever the store still held.
    return _post(host, token, "/api/daq/acq_config",
                 {"sr_mode": False, "filter": REF_FILTER, "adc_dec": REF_DEC})


# ---------------------------------------------------------------------------
# DUT supply
# ---------------------------------------------------------------------------
# Noise measured with the supply off only characterises the zero/offset path.
# The number that matters is noise while sourcing, so bring V_DUT up first.
def vdut_status(host, token):
    return _get(host, token, "/api/daq/vdut/status")


def vdut_setup(host, token, volts, ilimit_ma):
    r = _post(host, token, "/api/daq/vdut/setpoint",
              {"voltageV": volts, "currentLimitMa": ilimit_ma})
    if r.get("error"):
        return r
    return _post(host, token, "/api/daq/vdut/enable", {"enabled": True})


def vdut_disable(host, token):
    return _post(host, token, "/api/daq/vdut/enable", {"enabled": False})


def pd_ok(host, token, min_mv=9000, min_ma=3000):
    """The P4 refuses to enable V_DUT without a >=9 V / 3 A USB-PD contract."""
    pd = _get(host, token, "/api/usbpd")
    if pd.get("error"):
        return False, pd["error"]
    mv = round(float(pd.get("voltageV") or 0) * 1000)
    ma = round(float(pd.get("currentA") or 0) * 1000)
    if not pd.get("attached"):
        return False, "no USB-PD contract (charger not attached)"
    if mv < min_mv or ma < min_ma:
        return False, f"USB-PD contract {mv/1000:.1f} V / {ma/1000:.1f} A is below 9 V / 3 A"
    return True, f"{mv/1000:.1f} V / {ma/1000:.1f} A"


# ---------------------------------------------------------------------------
# Statistics
# ---------------------------------------------------------------------------
def detrended_sigma(xs):
    """Std-dev after removing the best-fit line.

    Drift is not noise. Over a multi-second window a few hundred nA of thermal
    ramp can exceed the actual noise floor, which would make slower settings
    look worse purely because their window is longer in wall-clock terms.
    """
    n = len(xs)
    if n < 3:
        return 0.0
    mean_x = (n - 1) / 2.0
    mean_y = sum(xs) / n
    sxx = sum((i - mean_x) ** 2 for i in range(n))
    sxy = sum((i - mean_x) * (xs[i] - mean_y) for i in range(n))
    slope = (sxy / sxx) if sxx else 0.0
    acc = 0.0
    for i, y in enumerate(xs):
        r = y - (mean_y + slope * (i - mean_x))
        acc += r * r
    return math.sqrt(acc / (n - 1))


def percentile(sorted_xs, q):
    if not sorted_xs:
        return 0.0
    k = (len(sorted_xs) - 1) * q
    lo, hi = math.floor(k), math.ceil(k)
    if lo == hi:
        return sorted_xs[lo]
    return sorted_xs[lo] * (hi - k) + sorted_xs[hi] * (k - lo)


def analyse(samples, rate, label):
    """Noise metrics for one capture window."""
    n = len(samples)
    if n < 16:
        return {"label": label, "n": n, "error": "too few samples"}
    mean = sum(samples) / n
    sd = detrended_sigma(samples)
    ss = sorted(samples)
    bw = rate / 2.0 if rate else 0.0
    return {
        "label": label,
        "n": n,
        "rate": rate,
        "mean": mean,
        "sigma": sd,
        "pp": ss[-1] - ss[0],
        "pp_robust": percentile(ss, 0.999) - percentile(ss, 0.001),
        # A/sqrt(Hz): flat across settings == white-noise limited.
        "density": (sd / math.sqrt(bw)) if bw > 0 else 0.0,
    }


def naive_decimate(samples, factor):
    """keep-1-of-N, i.e. what the firmware did before SR's FIR existed."""
    if factor < 2:
        return list(samples)
    return list(samples[::factor])


# ---------------------------------------------------------------------------
# Capture
# ---------------------------------------------------------------------------
def capture_once(link, seconds, settle, chunk, timeout_ms):
    """Stream for `settle + seconds`, discard the settle head, return samples."""
    link.drain()
    link.start()
    try:
        if settle > 0:
            link.collect(settle, chunk=chunk, timeout_ms=timeout_ms)
        cap = link.collect(seconds, chunk=chunk, timeout_ms=timeout_ms)
    finally:
        link.stop()

    cur, ranges, dropped = [], set(), 0
    for w in cap.wave_i:
        for k in range(w.count):
            m = w.meta[k] if k < len(w.meta) else 0
            if meta_settling(m) or meta_saturated(m):
                dropped += 1
                continue
            ranges.add(meta_range(m))
            cur.append(w.samples[k])
    volt = [s for w in cap.wave_v for s in w.samples]

    i_rate = cap.wave_i[-1].sample_rate if cap.wave_i else 0
    v_rate = cap.wave_v[-1].sample_rate if cap.wave_v else 0
    # STATUS extension v7 carries the two AD7415 board sensors. Take the last
    # frame of the window: thermal drift is the largest known contributor to
    # the low-frequency residual, so a sigma without a temperature beside it
    # cannot be told apart from converter 1/f.
    st = cap.last_status or {}
    return {
        "i": cur, "v": volt,
        "i_rate": i_rate, "v_rate": v_rate,
        "i_sps_measured": cap.wave_i_sps, "v_sps_measured": cap.wave_v_sps,
        "ranges": sorted(ranges), "excluded": dropped,
        "seq_lost": cap.seq_lost,
        "t_board0_c": st.get("t_board0_c"),
        "t_board1_c": st.get("t_board1_c"),
    }


def run_point(link, host, token, label, apply_fn, args):
    rsp = apply_fn()
    if isinstance(rsp, dict) and rsp.get("error"):
        return {"label": label, "error": rsp["error"]}
    # The S3 only queues the reconfiguration; the P4 applies it on its control
    # task after stopping acquisition, so give it a beat before streaming.
    time.sleep(args.reconfig_delay)
    try:
        c = capture_once(link, args.seconds, args.settle, args.chunk, args.timeout_ms)
    except Exception as e:                                    # noqa: BLE001
        return {"label": label, "error": f"{type(e).__name__}: {e}"}

    out = analyse(c["i"], c["i_rate"], label)
    out["v"] = analyse(c["v"], c["v_rate"], label + " (V)")
    out["ranges"] = c["ranges"]
    out["excluded"] = c["excluded"]
    out["seq_lost"] = c["seq_lost"]
    out["i_sps_measured"] = c["i_sps_measured"]
    out["t_board0_c"] = c["t_board0_c"]
    out["t_board1_c"] = c["t_board1_c"]
    out["raw_i"] = c["i"]

    # Yield check. Dropping out of the 1 MSPS modes can leave the stream
    # desynced, and a half-empty window still produces a perfectly plausible
    # sigma -- which is how a corrupted capture gets mistaken for a real
    # regression. Anything well short of rate x window is not trustworthy.
    expected = (c["i_rate"] or 0) * args.seconds
    if expected > 0:
        yield_pct = 100.0 * len(c["i"]) / expected
        out["yield_pct"] = yield_pct
        if yield_pct < 70.0:
            out["suspect"] = (f"only {yield_pct:.0f}% of expected samples "
                              f"({len(c['i'])} of {expected:,.0f})")
        elif yield_pct > 150.0:
            # More samples than the advertised rate can produce: the WAVE_I
            # header rate and the real delivery rate disagree, so sigma is
            # being computed over a stream that is not what it claims to be.
            out["suspect"] = (f"{yield_pct:.0f}% of expected samples "
                              f"({len(c['i'])} of {expected:,.0f}) - "
                              "header rate disagrees with delivery")
    return out


# ---------------------------------------------------------------------------
# Reporting
# ---------------------------------------------------------------------------
def fmt_a(x):
    """Amps -> a human unit, keeping 3 significant figures."""
    ax = abs(x)
    if ax >= 1e-3:
        return f"{x * 1e3:8.3f} mA"
    if ax >= 1e-6:
        return f"{x * 1e6:8.3f} uA"
    return f"{x * 1e9:8.1f} nA"


def fmt_density(x):
    ax = abs(x)
    if ax >= 1e-6:
        return f"{x * 1e6:7.3f} uA/rtHz"
    return f"{x * 1e9:7.2f} nA/rtHz"


def print_table(rows, ref):
    hdr = (f"{'setting':<18}{'rate':>9}{'n':>8}{'sigma':>14}"
           f"{'p-p(.1-99.9)':>16}{'density':>17}{'bits vs ref':>13}{'T degC':>9}  flags")
    print(hdr)
    print("-" * len(hdr))
    for r in rows:
        if r.get("error"):
            print(f"{r['label']:<18}{'--':>9}{'--':>8}  ERROR: {r['error']}")
            continue
        bits = ""
        if ref and ref.get("sigma") and r.get("sigma"):
            bits = f"{math.log2(ref['sigma'] / r['sigma']):+.2f}"
        t0 = r.get("t_board0_c")
        tstr = f"{t0:.1f}" if t0 is not None else "--"
        flags = []
        if r.get("suspect"):
            flags.append(f"!! SUSPECT: {r['suspect']}")
        if len(r.get("ranges", [])) > 1:
            flags.append(f"RANGE-HUNT{r['ranges']}")
        if r.get("excluded"):
            flags.append(f"excl={r['excluded']}")
        if r.get("seq_lost"):
            flags.append(f"lost={r['seq_lost']}")
        print(f"{r['label']:<18}{r['rate']:>9,}{r['n']:>8,}"
              f"{fmt_a(r['sigma']):>14}{fmt_a(r['pp_robust']):>16}"
              f"{fmt_density(r['density']):>17}{bits:>13}{tstr:>9}  {' '.join(flags)}")


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--host", help="S3 IP or hostname (control plane)")
    ap.add_argument("--token", default=os.environ.get("BB_TOKEN", ""),
                    help="admin token (or $BB_TOKEN)")
    ap.add_argument("--seconds", type=float, default=3.0, help="capture window per setting")
    ap.add_argument("--settle", type=float, default=2.0,
                    help="stream time discarded after each reconfiguration")
    ap.add_argument("--reconfig-delay", type=float, default=2.5,
                    help="pause after the HTTP config write before streaming")
    ap.add_argument("--chunk", type=int, default=65536)
    ap.add_argument("--timeout-ms", type=int, default=1000)
    ap.add_argument("--quick", action="store_true", help="8 representative settings only")
    ap.add_argument("--sr-only", action="store_true", help="skip the sweep, only SR vs reference")
    ap.add_argument("--with-fixed", action="store_true",
                    help="also probe Sinc5x8/x16 (1.024 MSPS/512 kSPS). These "
                         "corrupt every later setting - see build_plan().")
    ap.add_argument("--json", help="write full results here")
    ap.add_argument("--vdut", type=float, default=5.0,
                    help="DUT supply voltage during the sweep, V (0 = leave off)")
    ap.add_argument("--ilimit", type=float, default=500.0,
                    help="DUT current limit, mA")
    ap.add_argument("--vdut-settle", type=float, default=2.0,
                    help="seconds to let the supply settle after enabling")
    ap.add_argument("--compare", nargs=2, metavar=("A", "B"), help="diff two saved runs")
    args = ap.parse_args()

    if args.compare:
        with open(args.compare[0]) as fa:
            a = json.load(fa)
        with open(args.compare[1]) as fb:
            b = json.load(fb)
        ib = {r["label"]: r for r in b.get("rows", []) if not r.get("error")}
        print(f"{'setting':<18}{'sigma A':>14}{'sigma B':>14}{'change':>12}")
        print("-" * 58)
        for r in a.get("rows", []):
            o = ib.get(r["label"])
            if not o or r.get("error"):
                continue
            d = math.log2(r["sigma"] / o["sigma"]) if o["sigma"] else 0.0
            print(f"{r['label']:<18}{fmt_a(r['sigma']):>14}{fmt_a(o['sigma']):>14}"
                  f"{d:>+11.2f} b")
        return 0

    if not args.host or not args.token:
        ap.error("--host and --token (or $BB_TOKEN) are required unless --compare")

    try:
        link = DaqLink.usb(args.timeout_ms)
    except DaqLinkUnavailable as e:
        print(f"ERROR: P4 USB link unavailable: {e}")
        print("The DAQ HAT's own USB-C port must be plugged into this PC "
              "(VID 0x303A / PID 0x4001); the S3's port is not the stream link.")
        return 2

    print("NOTE: keep the DUT load static for the whole sweep — anything that "
          "moves is counted as noise.\n")

    # ---- DUT supply bring-up ------------------------------------------------
    supply_on = False
    if args.vdut > 0:
        ok, why = pd_ok(args.host, args.token)
        if not ok:
            print(f"ERROR: cannot enable V_DUT — {why}.")
            print("The P4 hard-guards the DUT output behind a 9 V / 3 A USB-PD "
                  "contract. Attach a PD supply, or pass --vdut 0 to measure "
                  "the zero path with the supply off.")
            return 2
        print(f"USB-PD contract   : {why}")
        r = vdut_setup(args.host, args.token, args.vdut, args.ilimit)
        if r.get("error"):
            print(f"ERROR: V_DUT enable failed: {r['error']}")
            return 2
        time.sleep(args.vdut_settle)
        st = vdut_status(args.host, args.token)
        meas_v = st.get("measVoltageV", st.get("meas_v"))
        print(f"V_DUT             : set {args.vdut:.2f} V @ {args.ilimit:.0f} mA limit"
              + (f", measured {float(meas_v):.3f} V" if meas_v is not None else "")
              + f"  (enabled={st.get('enabled')})")
        if st.get("enabled") is False:
            print("ERROR: the supply reports disabled after enable — aborting so "
                  "the sweep does not silently measure the zero path.")
            return 2
        supply_on = True
        print()
    else:
        print("V_DUT             : left OFF (--vdut 0) — measuring the zero path only\n")

    rows, sr_row, ref_row = [], None, None
    try:
        with link:
            # Reference first so every later row has something to be measured against.
            ref_row = run_point(link, args.host, args.token,
                                f"{FILTER_NAMES[REF_FILTER]} x256 (ref)",
                                lambda: set_acq(args.host, args.token, REF_FILTER, REF_DEC),
                                args)
            rows.append(ref_row)

            if not args.sr_only:
                for filt, dec, label in build_plan(args.quick, args.with_fixed):
                    if filt == REF_FILTER and dec == REF_DEC:
                        continue
                    print(f"  ... {label}", flush=True)
                    rows.append(run_point(link, args.host, args.token, label,
                                          lambda f=filt, d=dec: set_acq(args.host, args.token, f, d),
                                          args))

            print("  ... Super Resolution", flush=True)
            sr_row = run_point(link, args.host, args.token, "Super Resolution",
                               lambda: set_sr(args.host, args.token, True), args)
            rows.append(sr_row)

            # Always hand the device back in a known state.
            set_sr(args.host, args.token, False)
    finally:
        # Never leave the DUT rail live because the sweep aborted or was Ctrl-C'd.
        if supply_on:
            vdut_disable(args.host, args.token)
            print("\nV_DUT disabled.")

    print()
    print_table(rows, ref_row if not ref_row.get("error") else None)

    # ---- The comparison that actually validates SR -------------------------
    bad = [r for r in (sr_row, ref_row) if r and r.get("suspect")]
    if bad:
        print("\n== Super Resolution verdict SKIPPED ==")
        for r in bad:
            print(f"  {r['label']}: {r['suspect']}")
        print("  A short capture still yields a plausible sigma, so comparing it")
        print("  would invent a result. Re-run with a longer --settle, or use")
        print("  --sr-only to avoid the 1 MSPS modes that desync the stream.")
    elif (sr_row and not sr_row.get("error") and ref_row and not ref_row.get("error")
            and sr_row.get("rate") and ref_row.get("rate")):
        factor = max(1, round(ref_row["rate"] / sr_row["rate"]))
        naive = naive_decimate(ref_row["raw_i"], factor)
        naive_stats = analyse(naive, sr_row["rate"], "naive keep-1-of-N")
        ideal_bits = 0.5 * math.log2(factor)

        print("\n== Super Resolution vs the alternatives "
              f"(reference decimated {factor}x to {sr_row['rate']:,} sps) ==")
        print(f"  reference @ {ref_row['rate']:,} sps      sigma {fmt_a(ref_row['sigma'])}")
        print(f"  naive keep-1-of-{factor:<3} @ {sr_row['rate']:,} sps  "
              f"sigma {fmt_a(naive_stats['sigma'])}   "
              f"{math.log2(ref_row['sigma'] / naive_stats['sigma']):+.2f} bits vs ref"
              if naive_stats.get("sigma") else "  naive: n/a")
        print(f"  Super Resolution @ {sr_row['rate']:,} sps  "
              f"sigma {fmt_a(sr_row['sigma'])}   "
              f"{math.log2(ref_row['sigma'] / sr_row['sigma']):+.2f} bits vs ref")
        print(f"\n  ideal for white noise at {factor}x decimation: "
              f"{ideal_bits:+.2f} bits")
        if naive_stats.get("sigma") and sr_row.get("sigma"):
            gain = math.log2(naive_stats["sigma"] / sr_row["sigma"])
            print(f"  SR buys {gain:+.2f} bits over naive decimation to the same rate.")
            print("  That is the ADC's Sinc3 at max decimation plus the FIR combined --")
            print("  compare the Sinc3 x8160 row to separate the two: whatever SR gains")
            print("  beyond that row is the FIR's contribution, and it is small because")
            print("  the residual is already 1/f by the time the FIR sees it.")

        # Density is sigma/sqrt(BW). For WHITE noise it is invariant under
        # decimation, so a rise is the signature of 1/f (flicker) noise in the
        # front end -- which averaging cannot remove, and which is why the
        # measured gain falls short of the white-noise ideal. It is NOT
        # evidence that the FIR is misbehaving: the naive-decimation row above
        # isolates that, since both see the identical analog noise.
        d_ref, d_sr = ref_row.get("density") or 0.0, sr_row.get("density") or 0.0
        print(f"\n  density  ref {fmt_density(d_ref)}   SR {fmt_density(d_sr)}")
        if d_ref > 0 and d_sr > 0:
            ratio = d_sr / d_ref
            shortfall = ideal_bits - math.log2(ref_row["sigma"] / sr_row["sigma"])
            if ratio > 1.4:
                print(f"    density rose {ratio:.1f}x as bandwidth fell -> the residual is "
                      "1/f (flicker) dominated, not white.")
                print(f"    That is why SR lands {shortfall:.2f} bits short of the "
                      f"{ideal_bits:+.2f} b white-noise ideal: averaging only "
                      "beats white noise.")
                print("    This is an analog front-end property, not a filter fault -- "
                      "the naive row above shares the same analog path.")
            elif ratio < 0.7:
                print(f"    density FELL {1/ratio:.1f}x -> the ADC's Sinc3 at max "
                      "decimation is also cutting in-band noise, beyond pure averaging.")
            else:
                print("    density is flat -> white-noise limited; averaging is "
                      "delivering close to the theoretical sqrt(N).")
        if sr_row.get("v", {}).get("sigma"):
            print(f"\n  voltage: SR @ {sr_row['v']['rate']:,} sps  "
                  f"sigma {sr_row['v']['sigma'] * 1e6:.2f} uV")

    if args.json:
        with open(args.json, "w") as f:
            json.dump({"rows": [{k: v for k, v in r.items() if k != "raw_i"}
                                for r in rows]}, f, indent=2)
        print(f"\nwrote {args.json}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
