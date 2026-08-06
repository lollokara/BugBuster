#!/usr/bin/env python3
"""Live memory-pressure monitor for the ESP32-S3 mainboard.

Polls BBP_CMD_MEM_STATUS (USB) or GET /api/system/memory (HTTP) and renders a
refreshing table of heap and per-task stack headroom.

Unlike the `heap` / `stack_hwm` serial CLI commands this does not need
exclusive use of CDC0's line discipline, tracks the session minimum rather
than only the instantaneous value, and exits non-zero when a threshold is
breached -- so the same tool serves interactive debugging and CI.

Examples:
    # Watch over USB until Ctrl-C
    python tests/tools/mem_watch.py --device-usb COM7

    # Sample under load for 60 s, fail if internal free ever drops below 24 KB
    python tests/tools/mem_watch.py --device-usb COM7 --stress \\
        --duration 60 --fail-under-kb 24 --json mem.json

    # Against the simulator, no hardware needed
    python tests/tools/mem_watch.py --sim --duration 3
"""

from __future__ import annotations

import argparse
import json
import os
import sys
import time
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO_ROOT / "python"))
sys.path.insert(0, str(REPO_ROOT))

import bugbuster as bb  # noqa: E402


def _bar(used_pct: float, width: int = 24) -> str:
    filled = max(0, min(width, int(round(width * used_pct / 100.0))))
    return "[" + "#" * filled + "." * (width - filled) + "]"


def _kb(n: int) -> str:
    return f"{n / 1024:7.1f} KB"


class Session:
    """Accumulates the low-water marks across a run."""

    def __init__(self) -> None:
        self.samples: list[dict] = []
        self.min_internal_free: int | None = None
        self.min_internal_largest: int | None = None
        self.min_task_free: dict[str, int] = {}

    def add(self, m) -> None:
        self.min_internal_free = (
            m.internal.free_bytes if self.min_internal_free is None
            else min(self.min_internal_free, m.internal.free_bytes))
        self.min_internal_largest = (
            m.internal.largest_block_bytes if self.min_internal_largest is None
            else min(self.min_internal_largest, m.internal.largest_block_bytes))
        for t in m.tasks:
            if not t.running:
                continue
            prev = self.min_task_free.get(t.name)
            self.min_task_free[t.name] = t.free_bytes if prev is None else min(prev, t.free_bytes)

        self.samples.append({
            "t": time.time(),
            "uptime_ms": m.uptime_ms,
            "internal_free": m.internal.free_bytes,
            "internal_min_ever": m.internal.min_ever_bytes,
            "internal_largest": m.internal.largest_block_bytes,
            "internal_total": m.internal.total_bytes,
            "psram_free": m.psram.free_bytes,
            "psram_total": m.psram.total_bytes,
            "tasks": {t.name: {"declared": t.declared_bytes, "free": t.free_bytes}
                      for t in m.tasks},
        })


def render(m, sess: Session, elapsed: float) -> str:
    lines = []
    lines.append(f"BugBuster memory  |  uptime {m.uptime_ms / 1000:8.1f} s  "
                 f"|  watching {elapsed:5.1f} s  |  {len(sess.samples)} samples")
    lines.append("")

    i = m.internal
    lines.append("INTERNAL SRAM (the pool that runs out)")
    lines.append(f"  free      {_bar(i.used_pct)} {_kb(i.free_bytes)} of {_kb(i.total_bytes)}"
                 f"   ({i.used_pct:5.1f}% used)")
    lines.append(f"  min-ever  {_kb(i.min_ever_bytes)}   (device-reported, since boot)")
    lines.append(f"  largest   {_kb(i.largest_block_bytes)}   "
                 f"fragmentation {i.fragmentation_pct:5.1f}%")
    if sess.min_internal_free is not None:
        lines.append(f"  session   min free {_kb(sess.min_internal_free)}   "
                     f"min largest {_kb(sess.min_internal_largest or 0)}")

    if m.has_psram:
        p = m.psram
        lines.append("")
        lines.append("PSRAM")
        lines.append(f"  free      {_bar(p.used_pct)} {_kb(p.free_bytes)} of {_kb(p.total_bytes)}"
                     f"   ({p.used_pct:5.1f}% used)")

    lines.append("")
    lines.append("TASK STACKS                declared      free    peak-used   session-min")
    for t in m.tasks:
        if not t.running:
            lines.append(f"  {t.name:<12} (not running)")
            continue
        smin = sess.min_task_free.get(t.name, t.free_bytes)
        lines.append(
            f"  {t.name:<12} {_bar(t.used_pct, 12)} {t.declared_bytes:6d} B "
            f"{t.free_bytes:7d} B {t.peak_used_bytes:8d} B {smin:9d} B")

    warns = m.warnings()
    lines.append("")
    if warns:
        lines.append("WARNINGS")
        for w in warns:
            lines.append(f"  ! {w}")
    else:
        lines.append("No pressure warnings.")
    return "\n".join(lines)


def make_client(args):
    if args.sim:
        from tests.mock import SimulatedDevice, SimulatedUSBTransport
        device = SimulatedDevice()
        client = bb.BugBuster(SimulatedUSBTransport(device, hat=True))
        client.connect()
        return client
    if args.device_usb:
        return bb.connect_usb(args.device_usb)
    if args.device_http:
        return bb.connect_http(args.device_http, admin_token=args.admin_token)
    sys.exit("specify one of --device-usb, --device-http or --sim")


class Stressor:
    """Drives the device's known memory consumers while sampling.

    Idle memory figures say little: the interesting question is what headroom
    survives an ADC stream plus a resident script VM. Every step is wrapped
    because a device without a HAT, or one already streaming, must not abort
    the measurement run.
    """

    def __init__(self, client) -> None:
        self._c = client
        self._started = False

    def start(self) -> list[str]:
        applied = []
        for label, fn in (
            ("adc_stream", lambda: self._c.start_adc_stream([0], divider=1,
                                                            callback=lambda *_: None)),
            ("scope_stream", lambda: self._c.on_scope_data(lambda *_: None)),
        ):
            try:
                fn()
                applied.append(label)
            except Exception as exc:
                applied.append(f"{label}:skipped({type(exc).__name__})")
        self._started = True
        return applied

    def stop(self) -> None:
        if not self._started:
            return
        for fn in (self._c.stop_adc_stream, self._c.stop_scope_stream):
            try:
                fn()
            except Exception:
                pass


def main() -> int:
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    src = ap.add_argument_group("device")
    src.add_argument("--device-usb", metavar="PORT", help="serial port, e.g. COM7")
    src.add_argument("--device-http", metavar="HOST", help="IP or hostname")
    src.add_argument("--admin-token", default=os.environ.get("BUGBUSTER_ADMIN_TOKEN"))
    src.add_argument("--sim", action="store_true", help="use the in-process simulator")

    run = ap.add_argument_group("run")
    run.add_argument("--interval", type=float, default=1.0, help="seconds between samples")
    run.add_argument("--duration", type=float, default=0.0,
                     help="stop after N seconds (0 = until Ctrl-C)")
    run.add_argument("--stress", action="store_true",
                     help="start streams while sampling to measure under load")
    run.add_argument("--once", action="store_true", help="print one sample and exit")

    out = ap.add_argument_group("output")
    out.add_argument("--json", metavar="PATH", help="write the full sample series")
    out.add_argument("--csv", metavar="PATH", help="write a flat CSV of key figures")
    out.add_argument("--no-clear", action="store_true", help="append instead of redrawing")

    gate = ap.add_argument_group("thresholds (non-zero exit when breached)")
    gate.add_argument("--fail-under-kb", type=float, default=None,
                      help="fail if internal free ever drops below this")
    gate.add_argument("--fail-largest-under-kb", type=float, default=None,
                      help="fail if the largest internal block drops below this")
    gate.add_argument("--fail-task-pct", type=float, default=None,
                      help="fail if any task exceeds this %% of its stack")

    args = ap.parse_args()

    client = make_client(args)
    sess = Session()
    stressor = Stressor(client) if args.stress else None
    started = time.time()
    breaches: list[str] = []

    try:
        if stressor:
            print("stress: " + ", ".join(stressor.start()))
            time.sleep(0.2)

        while True:
            m = client.get_memory_status()
            sess.add(m)

            if args.fail_under_kb is not None and \
                    m.internal.free_bytes < args.fail_under_kb * 1024:
                breaches.append(
                    f"internal free {m.internal.free_bytes / 1024:.1f} KB < "
                    f"{args.fail_under_kb} KB")
            if args.fail_largest_under_kb is not None and \
                    m.internal.largest_block_bytes < args.fail_largest_under_kb * 1024:
                breaches.append(
                    f"largest internal block {m.internal.largest_block_bytes / 1024:.1f} KB < "
                    f"{args.fail_largest_under_kb} KB")
            if args.fail_task_pct is not None:
                for t in m.tasks:
                    if t.running and t.used_pct > args.fail_task_pct:
                        breaches.append(
                            f"task {t.name} at {t.used_pct:.0f}% > {args.fail_task_pct}%")

            elapsed = time.time() - started
            if not args.no_clear and not args.once and sys.stdout.isatty():
                # Home the cursor instead of clearing, so the table does not flicker.
                sys.stdout.write("\033[H\033[J")
            print(render(m, sess, elapsed))

            if args.once:
                break
            if args.duration and elapsed >= args.duration:
                break
            time.sleep(args.interval)
    except KeyboardInterrupt:
        print("\ninterrupted")
    finally:
        if stressor:
            stressor.stop()
        try:
            client.disconnect()
        except Exception:
            pass

    if args.json:
        Path(args.json).write_text(json.dumps({
            "samples": sess.samples,
            "min_internal_free": sess.min_internal_free,
            "min_internal_largest": sess.min_internal_largest,
            "min_task_free": sess.min_task_free,
        }, indent=2), encoding="utf-8")
        print(f"wrote {args.json}")

    if args.csv:
        rows = ["t,uptime_ms,internal_free,internal_min_ever,internal_largest,psram_free"]
        for s in sess.samples:
            rows.append(f"{s['t']:.3f},{s['uptime_ms']},{s['internal_free']},"
                        f"{s['internal_min_ever']},{s['internal_largest']},{s['psram_free']}")
        Path(args.csv).write_text("\n".join(rows) + "\n", encoding="utf-8")
        print(f"wrote {args.csv}")

    if breaches:
        print("\nTHRESHOLD BREACHED:")
        for b in dict.fromkeys(breaches):
            print(f"  {b}")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
