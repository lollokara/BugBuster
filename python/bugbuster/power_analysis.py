"""
power_analysis.py - host-side post-processing for DAQ HAT power captures.

Everything here deliberately runs on the PC rather than the ESP32-P4: the P4
has to keep the converters fed, so it streams samples and the interesting
analysis (energy integrals, state segmentation, periodicity, marker
correlation) is done where CPU is free.

Input is a :class:`bugbuster.daq_stream.PowerCapture` (or plain lists of
current/voltage samples plus a sample rate). Output is plain dicts, sized for
an LLM to read - full sample arrays are never returned, only summaries and
bounded previews.

Usage::

    from bugbuster.daq_stream import DaqStream
    from bugbuster.power_analysis import analyze

    with DaqStream() as s:
        cap = s.capture(duration_s=5.0)
    report = analyze(cap)
"""
from __future__ import annotations

import math
from typing import Any, Dict, List, Optional, Sequence, Tuple

from .daq_stream import (
    META_RANGE_MASK, META_SATURATED, META_SETTLING, META_SOURCE_MASK,
    META_SOURCE_SHIFT, RANGE_NAMES, SOURCE_NAMES, PowerCapture,
)


def _isnum(x: float) -> bool:
    return x == x and x not in (float("inf"), float("-inf"))


# ---------------------------------------------------------------------------
# Integrals and basic statistics
# ---------------------------------------------------------------------------
def integrate(
    current: Sequence[float],
    voltage: Sequence[float],
    sample_rate: float,
) -> Dict[str, Any]:
    """Trapezoidal integral of instantaneous power and current.

    Returns energy (J / mWh / uWh), charge (C / mAh / uAh), and the mean/peak
    figures a battery-life estimate needs. NaN samples (index gaps) are skipped
    and counted, so a lossy capture reports honest coverage instead of a
    silently low integral.
    """
    n = min(len(current), len(voltage))
    if n == 0 or sample_rate <= 0:
        return {"valid_samples": 0, "energy_j": 0.0, "charge_c": 0.0}

    dt = 1.0 / sample_rate
    energy_j = 0.0
    charge_c = 0.0
    valid = 0
    skipped = 0
    prev_p: Optional[float] = None
    prev_i: Optional[float] = None

    i_min = float("inf")
    i_max = float("-inf")
    i_sum = 0.0
    i_sq = 0.0
    p_min = float("inf")
    p_max = float("-inf")
    p_sum = 0.0
    v_sum = 0.0
    v_min = float("inf")
    v_max = float("-inf")

    for k in range(n):
        i = current[k]
        v = voltage[k]
        if not (_isnum(i) and _isnum(v)):
            skipped += 1
            prev_p = None
            prev_i = None
            continue
        p = i * v
        if prev_p is not None:
            energy_j += 0.5 * (p + prev_p) * dt
            charge_c += 0.5 * (i + prev_i) * dt
        prev_p, prev_i = p, i
        valid += 1
        i_sum += i
        i_sq += i * i
        v_sum += v
        if i < i_min:
            i_min = i
        if i > i_max:
            i_max = i
        if p < p_min:
            p_min = p
        if p > p_max:
            p_max = p
        if v < v_min:
            v_min = v
        if v > v_max:
            v_max = v
        p_sum += p

    if valid == 0:
        return {"valid_samples": 0, "energy_j": 0.0, "charge_c": 0.0,
                "skipped_samples": skipped}

    duration_s = valid * dt
    i_mean = i_sum / valid
    i_rms = math.sqrt(i_sq / valid)
    p_mean = p_sum / valid
    return {
        "duration_s": duration_s,
        "valid_samples": valid,
        "skipped_samples": skipped,
        "energy_j": energy_j,
        "energy_mwh": energy_j / 3.6,          # 1 mWh = 3.6 J
        "energy_uwh": energy_j / 0.0036,
        "charge_c": charge_c,
        "charge_mah": charge_c / 3.6,          # 1 mAh = 3.6 C
        "charge_uah": charge_c / 0.0036,
        "current_mean_a": i_mean,
        "current_rms_a": i_rms,
        "current_min_a": i_min,
        "current_max_a": i_max,
        "current_std_a": math.sqrt(max(0.0, i_sq / valid - i_mean * i_mean)),
        "current_crest_factor": (i_max / i_rms) if i_rms > 0 else None,
        "power_mean_w": p_mean,
        "power_min_w": p_min,
        "power_max_w": p_max,
        "voltage_mean_v": v_sum / valid,
        "voltage_min_v": v_min,
        "voltage_max_v": v_max,
    }


def battery_life(charge_mah: float, duration_s: float,
                 capacity_mah: float) -> Dict[str, Any]:
    """Project runtime from an average draw. Ignores self-discharge and the
    cell's own capacity-vs-load curve, so treat it as an upper bound."""
    if duration_s <= 0 or capacity_mah <= 0:
        return {}
    avg_ma = charge_mah / (duration_s / 3600.0)
    if avg_ma <= 0:
        return {"average_current_ma": avg_ma}
    hours = capacity_mah / avg_ma
    return {
        "average_current_ma": avg_ma,
        "battery_capacity_mah": capacity_mah,
        "estimated_hours": hours,
        "estimated_days": hours / 24.0,
        "estimated_years": hours / 8766.0,
    }


# ---------------------------------------------------------------------------
# State (power mode) detection
# ---------------------------------------------------------------------------
def _log_current(i: float, floor_a: float) -> float:
    return math.log10(max(abs(i), floor_a))


def _kmeans_1d(values: Sequence[float], k: int, iters: int = 25) -> List[float]:
    """Deterministic 1-D k-means. Centroids seeded on quantiles, so repeated
    runs over the same capture give the same labels."""
    if not values:
        return []
    ordered = sorted(values)
    n = len(ordered)
    cents = [ordered[min(n - 1, int((j + 0.5) * n / k))] for j in range(k)]
    for _ in range(iters):
        sums = [0.0] * k
        counts = [0] * k
        for v in values:
            best = 0
            bd = abs(v - cents[0])
            for j in range(1, k):
                d = abs(v - cents[j])
                if d < bd:
                    bd, best = d, j
            sums[best] += v
            counts[best] += 1
        moved = False
        for j in range(k):
            if counts[j]:
                nc = sums[j] / counts[j]
                if abs(nc - cents[j]) > 1e-9:
                    moved = True
                cents[j] = nc
        if not moved:
            break
    return sorted(cents)


def detect_states(
    current: Sequence[float],
    voltage: Sequence[float],
    sample_rate: float,
    max_states: int = 6,
    min_duration_s: float = 0.0005,
    current_floor_a: float = 1e-9,
) -> Dict[str, Any]:
    """Segment a capture into discrete power states.

    Consumer devices sit at a handful of current levels (deep sleep, idle, CPU
    active, radio TX). Clustering is done in log10(current) because those levels
    span decades - a linear clusterer puts every sub-mA state in one bin.

    Segments shorter than ``min_duration_s`` are absorbed into their neighbour,
    which removes transition ringing without hiding real short bursts (default
    is 500 us; raise it for a noisy DUT).

    Returns the state table (level, mean current/power, total and mean time,
    energy, share of the run) plus the ordered segment list and the transition
    counts between states.
    """
    n = min(len(current), len(voltage))
    if n == 0 or sample_rate <= 0:
        return {"states": [], "segments": [], "transitions": []}

    dt = 1.0 / sample_rate
    idx = [k for k in range(n) if _isnum(current[k])]
    if len(idx) < 2:
        return {"states": [], "segments": [], "transitions": []}

    # Subsample for the clustering pass - the centroids do not need every point.
    step = max(1, len(idx) // 20000)
    logs = [_log_current(current[k], current_floor_a) for k in idx[::step]]

    spread = max(logs) - min(logs)
    if spread < 0.15:                      # under ~1.4x - one flat level
        k = 1
    else:
        k = min(max_states, max(2, int(spread / 0.35) + 1))
    cents = _kmeans_1d(logs, k)

    def label_of(i_val: float) -> int:
        lv = _log_current(i_val, current_floor_a)
        best, bd = 0, abs(lv - cents[0])
        for j in range(1, len(cents)):
            d = abs(lv - cents[j])
            if d < bd:
                bd, best = d, j
        return best

    # Run-length encode labels over the full record.
    raw: List[List[Any]] = []   # [label, start, end_exclusive]
    for k2 in range(n):
        i = current[k2]
        lab = -1 if not _isnum(i) else label_of(i)
        if raw and raw[-1][0] == lab:
            raw[-1][2] = k2 + 1
        else:
            raw.append([lab, k2, k2 + 1])

    # Absorb sub-threshold runs into the previous segment.
    min_len = max(1, int(min_duration_s * sample_rate))
    merged: List[List[Any]] = []
    for seg in raw:
        if merged and (seg[2] - seg[1]) < min_len and merged[-1][0] != -1:
            merged[-1][2] = seg[2]
        elif merged and merged[-1][0] == seg[0]:
            merged[-1][2] = seg[2]
        else:
            merged.append(list(seg))

    segments: List[Dict[str, Any]] = []
    for lab, s, e in merged:
        cnt = 0
        i_sum = 0.0
        p_sum = 0.0
        i_pk = float("-inf")
        for k2 in range(s, e):
            i = current[k2]
            v = voltage[k2]
            if not (_isnum(i) and _isnum(v)):
                continue
            cnt += 1
            i_sum += i
            p_sum += i * v
            if i > i_pk:
                i_pk = i
        if cnt == 0:
            continue
        dur = (e - s) * dt
        segments.append({
            "state": lab,
            "start_s": s * dt,
            "end_s": e * dt,
            "duration_s": dur,
            "mean_current_a": i_sum / cnt,
            "peak_current_a": i_pk,
            "mean_power_w": p_sum / cnt,
            "energy_j": (p_sum / cnt) * dur,
        })

    total_s = n * dt
    states: Dict[int, Dict[str, Any]] = {}
    for seg in segments:
        st = states.setdefault(seg["state"], {
            "state": seg["state"], "occurrences": 0, "total_time_s": 0.0,
            "energy_j": 0.0, "_i_acc": 0.0, "_p_acc": 0.0,
            "min_duration_s": float("inf"), "max_duration_s": 0.0,
            "peak_current_a": float("-inf"),
        })
        st["occurrences"] += 1
        st["total_time_s"] += seg["duration_s"]
        st["energy_j"] += seg["energy_j"]
        st["_i_acc"] += seg["mean_current_a"] * seg["duration_s"]
        st["_p_acc"] += seg["mean_power_w"] * seg["duration_s"]
        st["min_duration_s"] = min(st["min_duration_s"], seg["duration_s"])
        st["max_duration_s"] = max(st["max_duration_s"], seg["duration_s"])
        st["peak_current_a"] = max(st["peak_current_a"], seg["peak_current_a"])

    table: List[Dict[str, Any]] = []
    for st in states.values():
        t = st["total_time_s"] or 1e-12
        table.append({
            "state": st["state"],
            "mean_current_a": st["_i_acc"] / t,
            "peak_current_a": st["peak_current_a"],
            "mean_power_w": st["_p_acc"] / t,
            "occurrences": st["occurrences"],
            "total_time_s": st["total_time_s"],
            "time_share": st["total_time_s"] / total_s if total_s else 0.0,
            "mean_duration_s": st["total_time_s"] / st["occurrences"],
            "min_duration_s": st["min_duration_s"],
            "max_duration_s": st["max_duration_s"],
            "energy_j": st["energy_j"],
        })
    table.sort(key=lambda r: r["mean_current_a"])
    # Rename states low->high so "state 0" always means the quietest level.
    remap = {row["state"]: rank for rank, row in enumerate(table)}
    for row in table:
        row["state"] = remap[row["state"]]
    for seg in segments:
        seg["state"] = remap.get(seg["state"], seg["state"])

    trans: Dict[Tuple[int, int], int] = {}
    for a, b in zip(segments, segments[1:]):
        if a["state"] != b["state"]:
            key = (a["state"], b["state"])
            trans[key] = trans.get(key, 0) + 1

    return {
        "states": table,
        "segments": segments,
        "transitions": [{"from": a, "to": b, "count": c}
                        for (a, b), c in sorted(trans.items())],
    }


def _name_states(table: List[Dict[str, Any]]) -> None:
    """Attach human labels by rank so a report reads without a legend."""
    if not table:
        return
    if len(table) == 1:
        table[0]["label"] = "steady"
        return
    # "peak" is reserved for the top rank; the rest must stay distinct or two
    # rows in the same table end up with the same name.
    names = ["sleep", "idle", "active", "busy", "high", "higher", "extreme"]
    last = len(table) - 1
    for rank, row in enumerate(table):
        if rank == last:
            row["label"] = "peak"
        elif rank < len(names):
            row["label"] = names[rank]
        else:
            row["label"] = f"level{rank}"


# ---------------------------------------------------------------------------
# Periodicity
# ---------------------------------------------------------------------------
def detect_periodicity(
    segments: Sequence[Dict[str, Any]],
    duration_s: float,
    active_state_min: int = 1,
) -> Dict[str, Any]:
    """Look for a repeating duty cycle in the segmentation.

    Uses the start times of every rising transition into an active state, which
    is robust against the burst itself varying in length (a BLE advertiser has
    a stable interval and a jittery payload).
    """
    starts = [s["start_s"] for s in segments if s["state"] >= active_state_min]
    if len(starts) < 3:
        return {"periodic": False, "active_bursts": len(starts)}
    gaps = [b - a for a, b in zip(starts, starts[1:])]
    mean = sum(gaps) / len(gaps)
    if mean <= 0:
        return {"periodic": False, "active_bursts": len(starts)}
    var = sum((g - mean) ** 2 for g in gaps) / len(gaps)
    jitter = math.sqrt(var) / mean
    return {
        "periodic": jitter < 0.15,
        "active_bursts": len(starts),
        "period_s": mean,
        "frequency_hz": 1.0 / mean,
        "jitter_rel": jitter,
        "min_interval_s": min(gaps),
        "max_interval_s": max(gaps),
        "bursts_per_hour": 3600.0 / mean,
    }


def duty_cycle(states: Sequence[Dict[str, Any]],
               active_state_min: int = 1) -> Dict[str, Any]:
    """Fraction of time and energy spent above the quietest state."""
    total_t = sum(s["total_time_s"] for s in states) or 1e-12
    total_e = sum(s["energy_j"] for s in states) or 1e-12
    act_t = sum(s["total_time_s"] for s in states if s["state"] >= active_state_min)
    act_e = sum(s["energy_j"] for s in states if s["state"] >= active_state_min)
    return {
        "active_time_share": act_t / total_t,
        "active_energy_share": act_e / total_e,
        "idle_time_share": 1.0 - act_t / total_t,
    }


# ---------------------------------------------------------------------------
# Markers
# ---------------------------------------------------------------------------
def segment_by_markers(
    capture: PowerCapture,
    channel: Optional[int] = None,
) -> List[Dict[str, Any]]:
    """Energy accounting for each interval between digital markers.

    Markers are timestamped by the ESP32-S3 in the shared sync epoch and carry
    the P4 sample index they align to, so a firmware GPIO toggle can bracket
    exactly the code region under test.
    """
    marks = capture.marker_dicts()
    if channel is not None:
        marks = [m for m in marks if m["channel"] == channel]
    if len(marks) < 2 or capture.sample_rate <= 0:
        return []
    out: List[Dict[str, Any]] = []
    for a, b in zip(marks, marks[1:]):
        s = max(0, int(a["sample_offset"]))
        e = min(capture.sample_count, int(b["sample_offset"]))
        if e <= s:
            continue
        res = integrate(capture.current[s:e], capture.voltage[s:e],
                        capture.sample_rate)
        out.append({
            "from_marker": a,
            "to_marker": b,
            "start_s": a["t_s"],
            "duration_s": res.get("duration_s", 0.0),
            "energy_j": res.get("energy_j", 0.0),
            "energy_uwh": res.get("energy_uwh", 0.0),
            "charge_uah": res.get("charge_uah", 0.0),
            "mean_current_a": res.get("current_mean_a"),
            "peak_current_a": res.get("current_max_a"),
        })
    return out


# ---------------------------------------------------------------------------
# Acquisition quality
# ---------------------------------------------------------------------------
def meta_quality(meta: Sequence[int]) -> Dict[str, Any]:
    """Per-sample range/source/flag accounting from the WAVE_I meta bytes.

    A capture with a large ``settling_share`` or any ``saturated_share`` is not
    trustworthy: lock the range or lower the source voltage and re-run.
    """
    n = len(meta)
    if n == 0:
        return {}
    rng: Dict[str, int] = {}
    src: Dict[str, int] = {}
    sat = 0
    settle = 0
    changes = 0
    prev = None
    for m in meta:
        r = RANGE_NAMES.get(m & META_RANGE_MASK, "unknown")
        rng[r] = rng.get(r, 0) + 1
        s = SOURCE_NAMES.get((m >> META_SOURCE_SHIFT) & META_SOURCE_MASK, "?")
        src[s] = src.get(s, 0) + 1
        if m & META_SATURATED:
            sat += 1
        if m & META_SETTLING:
            settle += 1
        if prev is not None and (m & META_RANGE_MASK) != (prev & META_RANGE_MASK):
            changes += 1
        prev = m
    return {
        "range_share": {k: v / n for k, v in rng.items()},
        "source_share": {k: v / n for k, v in src.items()},
        "saturated_share": sat / n,
        "settling_share": settle / n,
        "range_changes": changes,
    }


def preview(current: Sequence[float], voltage: Sequence[float],
            sample_rate: float, points: int = 200) -> List[Dict[str, Any]]:
    """Min/max/mean decimation down to ``points`` buckets.

    Min/max is kept rather than a plain average because an averaged preview
    hides exactly the short current spikes the capture was taken for.
    """
    n = min(len(current), len(voltage))
    if n == 0 or points <= 0:
        return []
    bucket = max(1, n // points)
    out: List[Dict[str, Any]] = []
    for start in range(0, n, bucket):
        end = min(n, start + bucket)
        mn, mx, acc, cnt, vacc = float("inf"), float("-inf"), 0.0, 0, 0.0
        for k in range(start, end):
            i = current[k]
            if not _isnum(i):
                continue
            mn = min(mn, i)
            mx = max(mx, i)
            acc += i
            cnt += 1
            v = voltage[k]
            if _isnum(v):
                vacc += v
        if cnt == 0:
            continue
        out.append({
            "t_s": start / sample_rate if sample_rate > 0 else None,
            "i_min_a": mn, "i_max_a": mx, "i_mean_a": acc / cnt,
            "v_mean_v": vacc / cnt,
        })
    return out


# ---------------------------------------------------------------------------
# Top-level report
# ---------------------------------------------------------------------------
def analyze(
    capture: PowerCapture,
    max_states: int = 6,
    min_state_duration_s: float = 0.0005,
    battery_capacity_mah: Optional[float] = None,
    preview_points: int = 200,
    max_segments: int = 50,
) -> Dict[str, Any]:
    """Full power-consumption report for a capture.

    Bounded on purpose: the segment list is truncated to ``max_segments`` and
    the waveform is returned only as a min/max preview, so the result stays
    readable by an LLM regardless of how many samples were taken.
    """
    sr = capture.sample_rate
    totals = integrate(capture.current, capture.voltage, sr)
    st = detect_states(capture.current, capture.voltage, sr,
                       max_states=max_states,
                       min_duration_s=min_state_duration_s)
    _name_states(st["states"])
    segments = st["segments"]

    report: Dict[str, Any] = {
        "capture": {
            "sample_rate_sps": sr,
            "sample_count": capture.sample_count,
            "duration_s": capture.duration_s,
            "dropped_samples": capture.dropped_samples,
            "loss_share": (capture.dropped_samples / capture.sample_count
                           if capture.sample_count else 0.0),
            "markers": len(capture.markers),
        },
        "totals": totals,
        "states": st["states"],
        "transitions": st["transitions"],
        "segment_count": len(segments),
        "segments": segments[:max_segments],
        "duty_cycle": duty_cycle(st["states"]),
        "periodicity": detect_periodicity(segments, capture.duration_s),
        "acquisition_quality": meta_quality(capture.meta),
        "preview": preview(capture.current, capture.voltage, sr, preview_points),
    }
    if len(segments) > max_segments:
        report["segments_truncated"] = True
    if battery_capacity_mah:
        report["battery"] = battery_life(
            totals.get("charge_mah", 0.0), totals.get("duration_s", 0.0),
            battery_capacity_mah)
    marker_windows = segment_by_markers(capture)
    if marker_windows:
        report["marker_windows"] = marker_windows[:max_segments]
        report["marker_list"] = capture.marker_dicts()[:100]
    if capture.device_energy is not None:
        report["device_accumulators"] = capture.device_energy.as_dict()
    if capture.device_status:
        report["device_status"] = capture.device_status
    report["warnings"] = _warnings(report)
    if capture.rate_warning:
        report["capture"]["rate_source"] = capture.rate_source
        report["warnings"].insert(0, capture.rate_warning)
    return report


def _warnings(report: Dict[str, Any]) -> List[str]:
    w: List[str] = []
    cap = report["capture"]
    if cap["loss_share"] > 0.001:
        w.append(
            f"{cap['loss_share'] * 100:.2f}% of samples were lost to stream "
            f"back-pressure; energy integrals skip the gaps. Lower the sample "
            f"rate or raise the stream decimation.")
    q = report.get("acquisition_quality") or {}
    if q.get("saturated_share", 0) > 0:
        w.append(
            f"{q['saturated_share'] * 100:.2f}% of samples were saturated - "
            f"the measured current exceeded the active range. Peak values are "
            f"floors, not true readings.")
    if q.get("settling_share", 0) > 0.02:
        w.append(
            f"{q['settling_share'] * 100:.1f}% of samples were taken while the "
            f"front-end was settling after a range change. Lock the range if "
            f"the DUT's current spans a boundary.")
    tot = report.get("totals") or {}
    if tot.get("valid_samples", 0) == 0:
        w.append("No valid samples in this capture - is the source enabled and "
                 "the DUT connected?")
    return w
