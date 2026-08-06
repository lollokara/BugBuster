#!/usr/bin/env python3
"""
daq_noise_plot.py — plot a daq_noise_sweep.py result set.

Produces a 4-panel figure from the JSON written by
`daq_noise_sweep.py --json`:

  1. sigma vs output rate, per filter family (log/log). The white-noise
     reference line (sigma proportional to sqrt(rate)) shows how much of each
     point is explained by bandwidth alone.
  2. Noise density vs rate. Density is sigma/sqrt(BW), so it is FLAT for white
     noise. A rise toward low rates is the signature of 1/f (flicker) noise,
     which averaging cannot remove -- this is what explains an SR gain that
     falls short of the sqrt(N) ideal.
  3. Bits gained vs the reference setting, sorted.
  4. Peak-to-peak (robust 0.1-99.9 percentile) vs sigma, to show whether a
     setting is dominated by broadband noise or by occasional outliers.

Super Resolution is highlighted in every panel because it is the point the
sweep exists to justify.

Usage:
    python tests/tools/daq_noise_plot.py data/sr_noise_vdut5.json
    python tests/tools/daq_noise_plot.py data/a.json --out noise.png --show
"""

from __future__ import annotations

import argparse
import json
import math
import os
import sys

import matplotlib

if not os.environ.get("DISPLAY") and sys.platform not in ("win32", "darwin"):
    matplotlib.use("Agg")
import matplotlib.pyplot as plt

SR_LABEL = "Super Resolution"

# Filter family -> (colour, marker). Keyed by the prefix daq_noise_sweep emits.
FAMILY = {
    "Wideband": ("#2563eb", "o"),
    "Sinc5x":   ("#9333ea", "D"),   # fixed-rate variants; checked before Sinc5
    "Sinc5":    ("#059669", "s"),
    "Sinc3":    ("#d97706", "^"),
}


def family_of(label: str) -> str:
    for name in ("Wideband", "Sinc5x", "Sinc5", "Sinc3"):
        if label.startswith(name):
            return name
    return "other"


def load(path: str):
    with open(path) as f:
        rows = json.load(f)["rows"]
    # `suspect` rows are dropped, not drawn. A short capture still yields a
    # perfectly plausible sigma, so plotting one puts a fabricated point on the
    # chart that looks exactly like a real measurement.
    good, dropped = [], []
    for r in rows:
        if r.get("error"):
            dropped.append((r.get("label", "?"), r["error"]))
        elif r.get("suspect"):
            dropped.append((r.get("label", "?"), r["suspect"]))
        elif r.get("sigma") and r.get("rate"):
            good.append(r)
    if dropped:
        print(f"{path}: dropped {len(dropped)} invalid row(s)")
        for lbl, why in dropped:
            print(f"    {lbl}: {why}")
    if not good:
        raise SystemExit(f"{path}: no usable rows")
    return good


def ref_row(rows):
    for r in rows:
        if "(ref)" in r["label"]:
            return r
    return rows[0]


def mean_temp(rows):
    ts = [r["t_board0_c"] for r in rows if r.get("t_board0_c") is not None]
    return sum(ts) / len(ts) if ts else None


def plot_compare(path_a, path_b, out, title):
    """Two thermal states, matched setting by setting.

    Only settings present AND valid in BOTH runs are compared -- a setting that
    was dropped as suspect in one run has no partner, and pairing it against
    nothing would manufacture a delta.
    """
    a_rows, b_rows = load(path_a), load(path_b)
    a_by = {r["label"]: r for r in a_rows}
    b_by = {r["label"]: r for r in b_rows}
    common = [lbl for lbl in a_by if lbl in b_by]
    if not common:
        raise SystemExit("no settings valid in both runs")
    common.sort(key=lambda lbl: a_by[lbl]["rate"])

    ta, tb = mean_temp(a_rows), mean_temp(b_rows)
    ta_s = f"{ta:.1f} degC" if ta is not None else "run A"
    tb_s = f"{tb:.1f} degC" if tb is not None else "run B"
    dt = (tb - ta) if (ta is not None and tb is not None) else None

    fig, axes = plt.subplots(1, 3, figsize=(20, 6.5))
    head = f"Thermal correlation: {ta_s} vs {tb_s}"
    if dt is not None:
        head += f"   (dT = {dt:+.1f} degC)"
    if title:
        head += f" — {title}"
    fig.suptitle(head, fontsize=13, fontweight="bold")

    # ---- 1. sigma vs rate, both states --------------------------------------
    ax = axes[0]
    for rows, colour, lbl, mk in ((a_rows, "#2563eb", ta_s, "o"),
                                  (b_rows, "#dc2626", tb_s, "s")):
        pts = sorted([r for r in rows if r["label"] in common],
                     key=lambda r: r["rate"])
        ax.plot([p["rate"] for p in pts], [p["sigma"] * 1e9 for p in pts],
                marker=mk, color=colour, label=lbl, lw=1.6, ms=6)
    for rows, colour, mk in ((a_rows, "#2563eb", "*"), (b_rows, "#dc2626", "*")):
        sr = next((r for r in rows if r["label"] == SR_LABEL), None)
        if sr:
            ax.plot([sr["rate"]], [sr["sigma"] * 1e9], marker=mk, ms=18,
                    color=colour, ls="none", zorder=5)
    ax.set_xscale("log")
    ax.set_yscale("log")
    ax.set_xlabel("output sample rate (Sa/s)")
    ax.set_ylabel("sigma (nA)")
    ax.set_title("Noise vs rate at both temperatures\n(stars = Super Resolution)",
                 fontsize=10)
    ax.grid(True, which="both", alpha=0.25)
    ax.legend(fontsize=9)

    # ---- 2. per-setting change ----------------------------------------------
    # log2 ratio, so the unit matches the "bits" used everywhere else: positive
    # = the hotter run is noisier.
    ax = axes[1]
    deltas = sorted(((math.log2(b_by[lbl]["sigma"] / a_by[lbl]["sigma"]), lbl)
                     for lbl in common), key=lambda t: t[0])
    vals = [d for d, _ in deltas]
    labels = [lbl for _, lbl in deltas]
    colours = ["#dc2626" if lbl == SR_LABEL
               else ("#64748b" if "(ref)" in lbl else FAMILY[family_of(lbl)][0])
               for _, lbl in deltas]
    ax.barh(range(len(vals)), vals, color=colours)
    ax.set_yticks(range(len(vals)))
    ax.set_yticklabels(labels, fontsize=8)
    ax.axvline(0, color="k", lw=1)
    ax.set_xlabel(f"bits worse at {tb_s}   (log2 sigma ratio)")
    ax.set_title("Per-setting thermal sensitivity\nright = degrades when hot",
                 fontsize=10)
    ax.grid(True, axis="x", alpha=0.25)

    # ---- 3. ratio vs rate ---------------------------------------------------
    # Flat => the tempco is a property of the front end, common to every
    # setting. Sloped => the sensitivity itself depends on bandwidth, which
    # would point at the filter rather than the analog path.
    ax = axes[2]
    for lbl in common:
        fam = family_of(lbl)
        colour = ("#dc2626" if lbl == SR_LABEL
                  else FAMILY.get(fam, ("#64748b", "x"))[0])
        mk = "*" if lbl == SR_LABEL else FAMILY.get(fam, ("#64748b", "x"))[1]
        ax.scatter(a_by[lbl]["rate"], b_by[lbl]["sigma"] / a_by[lbl]["sigma"],
                   color=colour, marker=mk, s=200 if lbl == SR_LABEL else 55,
                   zorder=5 if lbl == SR_LABEL else 3)
    ax.axhline(1.0, color="k", lw=1, ls="--", label="no change")
    ax.set_xscale("log")
    ax.set_xlabel("output sample rate (Sa/s)")
    ax.set_ylabel(f"sigma({tb_s}) / sigma({ta_s})")
    ax.set_title("Is the thermal effect rate-dependent?\nflat = front-end, sloped = filter",
                 fontsize=10)
    ax.grid(True, which="both", alpha=0.25)
    ax.legend(fontsize=8)

    fig.tight_layout(rect=(0, 0, 1, 0.93))
    fig.savefig(out, dpi=140)
    print(f"wrote {out}")

    worst = max(deltas, key=lambda t: abs(t[0]))
    print(f"compared {len(common)} settings present in both runs")
    print(f"  largest change: {worst[1]}  {worst[0]:+.2f} bits")
    med = sorted(vals)[len(vals) // 2]
    print(f"  median change : {med:+.2f} bits")
    for lbl in (SR_LABEL,):
        if lbl in a_by:
            d = math.log2(b_by[lbl]["sigma"] / a_by[lbl]["sigma"])
            print(f"  {lbl:<18}: {a_by[lbl]['sigma']*1e9:6.1f} -> "
                  f"{b_by[lbl]['sigma']*1e9:6.1f} nA  ({d:+.2f} bits)")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("json", help="result file from daq_noise_sweep.py --json")
    ap.add_argument("--compare", metavar="OTHER_JSON",
                    help="second run to correlate against (e.g. a hotter board); "
                         "produces the thermal-correlation figure instead")
    ap.add_argument("--out", help="output image (default: alongside the JSON)")
    ap.add_argument("--show", action="store_true", help="also open a window")
    ap.add_argument("--title", default="", help="extra title text (e.g. 'V_DUT 5 V')")
    args = ap.parse_args()

    if args.compare:
        out = args.out or os.path.splitext(args.json)[0] + "_vs_" + \
            os.path.splitext(os.path.basename(args.compare))[0] + ".png"
        plot_compare(args.json, args.compare, out, args.title)
        if args.show:
            plt.show()
        return 0

    rows = load(args.json)
    ref = ref_row(rows)
    sr = next((r for r in rows if r["label"] == SR_LABEL), None)
    out = args.out or os.path.splitext(args.json)[0] + ".png"

    sweep = [r for r in rows if r["label"] != SR_LABEL]

    fig, axes = plt.subplots(2, 3, figsize=(20, 9))
    suffix = f" — {args.title}" if args.title else ""
    temps = [r["t_board0_c"] for r in rows if r.get("t_board0_c") is not None]
    if temps:
        suffix += f"   [board {min(temps):.1f}–{max(temps):.1f} °C]"
    fig.suptitle(f"DAQ HAT noise vs filter / decimation{suffix}",
                 fontsize=14, fontweight="bold")

    # ---- 1. sigma vs rate ---------------------------------------------------
    ax = axes[0][0]
    for fam in ("Wideband", "Sinc5", "Sinc3", "Sinc5x"):
        pts = sorted([r for r in sweep if family_of(r["label"]) == fam],
                     key=lambda r: r["rate"])
        if not pts:
            continue
        colour, marker = FAMILY[fam]
        ax.plot([p["rate"] for p in pts], [p["sigma"] * 1e9 for p in pts],
                marker=marker, color=colour, label=fam, lw=1.6, ms=6)
    if sr:
        ax.plot([sr["rate"]], [sr["sigma"] * 1e9], marker="*", ms=20,
                color="#dc2626", ls="none", label=SR_LABEL, zorder=5)

    # sqrt(rate) line anchored on the reference: pure bandwidth scaling.
    rates = sorted({r["rate"] for r in rows})
    ideal = [ref["sigma"] * 1e9 * math.sqrt(x / ref["rate"]) for x in rates]
    ax.plot(rates, ideal, "k--", lw=1, alpha=0.5,
            label=r"white-noise $\propto\sqrt{f_s}$")
    ax.set_xscale("log")
    ax.set_yscale("log")
    ax.set_xlabel("output rate (Sa/s)")
    ax.set_ylabel("current noise sigma (nA)")
    ax.set_title("Noise vs output rate")
    ax.grid(True, which="both", alpha=0.25)
    ax.legend(fontsize=8)

    # ---- 2. density vs rate -------------------------------------------------
    ax = axes[0][1]
    for fam in ("Wideband", "Sinc5", "Sinc3", "Sinc5x"):
        pts = sorted([r for r in sweep if family_of(r["label"]) == fam],
                     key=lambda r: r["rate"])
        if not pts:
            continue
        colour, marker = FAMILY[fam]
        ax.plot([p["rate"] for p in pts], [p["density"] * 1e9 for p in pts],
                marker=marker, color=colour, label=fam, lw=1.6, ms=6)
    if sr:
        ax.plot([sr["rate"]], [sr["density"] * 1e9], marker="*", ms=20,
                color="#dc2626", ls="none", label=SR_LABEL, zorder=5)
    ax.axhline(ref["density"] * 1e9, color="k", ls="--", lw=1, alpha=0.5,
               label="reference density")
    ax.set_xscale("log")
    ax.set_yscale("log")
    ax.set_xlabel("output rate (Sa/s)")
    ax.set_ylabel(r"noise density (nA/$\sqrt{Hz}$)")
    ax.set_title("Density — flat = white noise, rising at low rate = 1/f")
    ax.grid(True, which="both", alpha=0.25)
    ax.legend(fontsize=8)

    # ---- 3. bits vs reference ----------------------------------------------
    ax = axes[1][0]
    scored = sorted(((math.log2(ref["sigma"] / r["sigma"]), r) for r in rows),
                    key=lambda t: t[0])
    labels = [r["label"] for _, r in scored]
    bits = [b for b, _ in scored]
    colours = ["#dc2626" if r["label"] == SR_LABEL
               else ("#64748b" if "(ref)" in r["label"] else FAMILY[family_of(r["label"])][0])
               for _, r in scored]
    ax.barh(range(len(bits)), bits, color=colours)
    ax.set_yticks(range(len(bits)))
    ax.set_yticklabels(labels, fontsize=8)
    ax.axvline(0, color="k", lw=1)
    ax.set_xlabel("bits gained vs reference  (log2 sigma ratio)")
    ax.set_title("Effective resolution vs the reference setting")
    ax.grid(True, axis="x", alpha=0.25)

    # ---- 4. robust p-p vs sigma --------------------------------------------
    ax = axes[1][1]
    for fam in ("Wideband", "Sinc5", "Sinc3", "Sinc5x"):
        pts = [r for r in sweep if family_of(r["label"]) == fam]
        if not pts:
            continue
        colour, marker = FAMILY[fam]
        ax.scatter([p["sigma"] * 1e9 for p in pts],
                   [(p.get("pp_robust") or 0) * 1e9 for p in pts],
                   color=colour, marker=marker, s=45, label=fam)
    if sr:
        ax.scatter([sr["sigma"] * 1e9], [(sr.get("pp_robust") or 0) * 1e9],
                   color="#dc2626", marker="*", s=320, label=SR_LABEL, zorder=5)
    lo = min(r["sigma"] for r in rows) * 1e9
    hi = max(r["sigma"] for r in rows) * 1e9
    span = [lo, hi]
    for k, style in ((6.6, ":"), (13.2, "--")):
        ax.plot(span, [k * lo, k * hi], "k", ls=style, lw=1, alpha=0.45,
                label=f"p-p = {k:g}x sigma")
    ax.set_xscale("log")
    ax.set_yscale("log")
    ax.set_xlabel("sigma (nA)")
    ax.set_ylabel("robust p-p, 0.1–99.9% (nA)")
    ax.set_title("Outlier behaviour — above the lines = spiky, not just noisy")
    ax.grid(True, which="both", alpha=0.25)
    ax.legend(fontsize=8)

    # ---- 5. board temperature through the run ------------------------------
    # Thermal drift is the largest known contributor to the low-frequency
    # residual, and it is indistinguishable from converter 1/f in a single
    # sigma. A sweep that warmed several degrees is comparing settings at
    # different operating points, so this panel is a validity check on the
    # other four, not a result in itself.
    ax = axes[0][2]
    t0 = [(i, r.get("t_board0_c")) for i, r in enumerate(rows)]
    t1 = [(i, r.get("t_board1_c")) for i, r in enumerate(rows)]
    have = [(i, t) for i, t in t0 if t is not None]
    if have:
        ax.plot([i for i, _ in have], [t for _, t in have],
                "o-", color="#dc2626", ms=5, lw=1.5, label="U2 (analog)")
        h1 = [(i, t) for i, t in t1 if t is not None]
        if h1:
            ax.plot([i for i, _ in h1], [t for _, t in h1],
                    "s-", color="#2563eb", ms=4, lw=1.2, label="U28 (power)")
        span = max(t for _, t in have) - min(t for _, t in have)
        ax.set_title(f"Board temperature during the sweep (drift {span:.1f} °C)",
                     fontsize=10)
        ax.set_xlabel("capture order")
        ax.set_ylabel("°C")
        ax.grid(True, alpha=0.25)
        ax.legend(fontsize=8)
        if span > 2.0:
            ax.text(0.5, 0.05,
                    f"drift {span:.1f} °C — settings measured at\ndifferent "
                    "operating points, compare with care",
                    transform=ax.transAxes, ha="center", fontsize=8,
                    color="#b91c1c",
                    bbox={"fc": "#fef2f2", "ec": "#dc2626", "alpha": 0.9})
    else:
        ax.text(0.5, 0.5, "no board temperature in this capture\n"
                          "(STATUS extension v7 — reflash the P4)",
                transform=ax.transAxes, ha="center", va="center",
                fontsize=9, color="#64748b")
        ax.set_title("Board temperature", fontsize=10)
        ax.set_xticks([])
        ax.set_yticks([])

    axes[1][2].axis("off")

    fig.tight_layout(rect=(0, 0, 1, 0.96))
    fig.savefig(out, dpi=140)
    print(f"wrote {out}")

    if sr:
        gain = math.log2(ref["sigma"] / sr["sigma"])
        factor = max(1, round(ref["rate"] / sr["rate"]))
        print(f"SR: {sr['sigma'] * 1e9:.1f} nA @ {sr['rate']:,} sps "
              f"({gain:+.2f} bits vs {ref['sigma'] * 1e9:.1f} nA @ "
              f"{ref['rate']:,} sps; white-noise ideal "
              f"{0.5 * math.log2(factor):+.2f} b)")

    if args.show:
        plt.show()
    return 0


if __name__ == "__main__":
    sys.exit(main())
