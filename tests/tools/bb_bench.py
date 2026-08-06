#!/usr/bin/env python3
"""bb_bench.py — cross-surface performance benchmark for the whole BugBuster stack.

One repeatable tool, one comparable JSON schema, so regressions and
improvements can be *seen* over time instead of guessed at from a single
ad-hoc run. It measures five independent surfaces, each optional so a
partial run (missing hardware, no USB device, no serial port) still
produces a usable report for whatever IS reachable:

    http      S3 HTTP/API latency for a set of representative endpoints.
    daq-usb   DAQ HAT USB-HS stream throughput -- delegates to the existing
              tests/tools/daq_usb_stream_bench.py, does not reimplement it.
    ota       S3 firmware-upload throughput over HTTP. NEVER RUNS BY
              DEFAULT: pass --image and --do-flash to actually flash the
              device (see "ota" subcommand --help). --include-slow adds an
              optional P4 push, which ALSO flashes and needs its own flags.
    link      HAT round-trip latency as observed from the S3 -- repeated
              /api/hat polls. This is HTTP + HAT UART overhead together,
              NOT the raw UART link in isolation; see "what this does NOT
              measure" below.
    mem       S3 memory snapshot over the serial CLI (`heap`, `stack_hwm`
              on the USB-CDC console): internal free / min-ever / largest
              contiguous block, PSRAM free/largest, and per-task stack
              high-water marks. A genuinely useful regression signal --
              e.g. internal-largest-contiguous going 13 KB -> 19 KB from a
              stack right-sizing pass, when the update worker needs a
              12 KB contiguous block to run at all.

Usage:
    python3 tests/tools/bb_bench.py all --json report.json
    python3 tests/tools/bb_bench.py http --host 192.168.3.35 --token $BB_TOKEN
    python3 tests/tools/bb_bench.py mem --serial-port /dev/cu.usbmodem1234561
    python3 tests/tools/bb_bench.py daq-usb --seconds 5
    python3 tests/tools/bb_bench.py ota --image firmware.bin --do-flash   # FLASHES
    python3 tests/tools/bb_bench.py --compare before.json after.json

what this tool does NOT measure (read before trusting a number):
    * iOS / desktop-app UI rendering, frame time, or perceived latency --
      this is host/firmware-side only.
    * the RP2040 HAT UART link in isolation -- the "link" figure is
      HTTP + HAT round trip end to end, and says so in its own output.
    * WiFi RF-layer behaviour (RSSI, retries, retransmits) -- only
      application-level latency as observed by the client.
    * flash wear, boot time, or rollback correctness for OTA -- only
      upload throughput.
    * anything about the DAQ USB stream beyond what daq_usb_stream_bench.py
      itself measures -- see that tool's own module docstring for ITS
      boundary.

Every measurement records its own unit and sample count; there are no bare
numbers in the JSON output. Missing hardware degrades one surface to
{"available": false, "error": "..."} without aborting the rest of the run.
"""

from __future__ import annotations

import argparse
import importlib
import json
import os
import re
import statistics
import sys
import time
from dataclasses import dataclass, field
from datetime import datetime, timezone
from typing import Any, Callable, Optional

_REPO_ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
if _REPO_ROOT not in sys.path:
    sys.path.insert(0, _REPO_ROOT)
_PY_ROOT = os.path.join(_REPO_ROOT, "python")
if _PY_ROOT not in sys.path:
    sys.path.insert(0, _PY_ROOT)

SCHEMA_VERSION = 1

NOT_MEASURED = [
    "iOS/desktop-app UI rendering, frame time, or perceived latency",
    "the RP2040 HAT UART link in isolation -- 'link' is HTTP+HAT round "
    "trip end to end, not raw UART",
    "WiFi RF-layer behaviour (RSSI, retries) -- only application-level "
    "latency",
    "OTA flash wear, boot time, or rollback correctness -- only upload "
    "throughput, and only when explicitly opted in",
    "anything about the DAQ USB stream beyond daq_usb_stream_bench.py's own "
    "scope",
]

ADMIN_TOKEN_HEADER = "X-BugBuster-Admin-Token"


# ---------------------------------------------------------------------------
# Pure stats helpers (unit-testable, no hardware)
# ---------------------------------------------------------------------------

def percentile(sorted_values: list, pct: float) -> Optional[float]:
    """Linear-interpolation percentile (matches numpy's default 'linear').

    Returns None for an empty input rather than raising -- callers building
    a report for a surface with zero successful samples should get a
    structurally valid (if empty) stats block, not a crash.
    """
    if not sorted_values:
        return None
    if len(sorted_values) == 1:
        return float(sorted_values[0])
    k = (len(sorted_values) - 1) * (pct / 100.0)
    f = int(k)
    c = min(f + 1, len(sorted_values) - 1)
    if f == c:
        return float(sorted_values[f])
    d0 = sorted_values[f] * (c - k)
    d1 = sorted_values[c] * (k - f)
    return float(d0 + d1)


def compute_stats(samples: list, unit: str) -> dict:
    """min/median/p95/max/count over @p samples, tagged with @p unit.

    Every field the report emits carries its own unit and sample count (a
    hard requirement -- no bare numbers), so this is the one place that
    shape gets built.
    """
    if not samples:
        return {"count": 0, "unit": unit, "min": None, "median": None,
                "p95": None, "max": None}
    s = sorted(samples)
    return {
        "count": len(s),
        "unit": unit,
        "min": float(s[0]),
        "median": float(statistics.median(s)),
        "p95": percentile(s, 95),
        "max": float(s[-1]),
    }


# ---------------------------------------------------------------------------
# http / link surface
# ---------------------------------------------------------------------------

HTTP_ENDPOINTS = [
    ("status", "/status", None, False),
    ("hat", "/hat", None, False),
    ("overview", "/overview", None, False),
    ("scope", "/scope", {"since": 0}, False),
    ("update_status", "/update/status", None, True),   # admin-gated GET
]


@dataclass
class _TimedGet:
    """Injectable HTTP GET used by both the `http` and `link` surfaces.

    Kept as its own small object (rather than passing a bare requests
    session around) so unit tests can substitute a fake without touching
    the network -- see tests/unit/test_bb_bench.py's _FakeSession.
    """
    session: Any
    base: str
    token: Optional[str]
    timeout: float

    def __call__(self, path: str, params: Optional[dict] = None,
                 need_admin: bool = False):
        headers = {}
        if need_admin and self.token:
            headers[ADMIN_TOKEN_HEADER] = self.token
        start = time.monotonic()
        r = self.session.get(f"{self.base}{path}", params=params,
                              headers=headers or None, timeout=self.timeout)
        elapsed_ms = (time.monotonic() - start) * 1000.0
        r.raise_for_status()
        return elapsed_ms


class SurfaceUnavailable(Exception):
    """A surface's hardware/module dependency is missing. Caught at the top
    of each `bench_*` entry point and turned into {"available": False}."""


def bench_http(get_fn: Callable[..., float], samples: int = 20,
                idle_wait: float = 0.0,
                endpoints=None) -> dict:
    """Latency for a set of representative S3 endpoints.

    The first request after idle is measurably slower (WiFi power-save
    wake-up) -- reported once as `cold_ms` rather than folded into the
    per-endpoint stats, where it would just look like noise or a phantom
    p95 regression.
    """
    endpoints = endpoints if endpoints is not None else HTTP_ENDPOINTS
    out: dict = {"available": True, "endpoints": {}, "cold_ms": None,
                 "cold_endpoint": None}

    if idle_wait > 0:
        time.sleep(idle_wait)

    if endpoints:
        name0, path0, params0, admin0 = endpoints[0]
        try:
            out["cold_ms"] = get_fn(path0, params0, admin0)
            out["cold_endpoint"] = name0
        except Exception as exc:
            out["cold_error"] = str(exc)

    for name, path, params, need_admin in endpoints:
        lat: list = []
        errors = 0
        last_error = ""
        for _ in range(samples):
            try:
                lat.append(get_fn(path, params, need_admin))
            except Exception as exc:
                errors += 1
                last_error = str(exc)
        entry = compute_stats(lat, "ms")
        entry["errors"] = errors
        entry["error_rate"] = errors / samples if samples else 0.0
        if last_error:
            entry["last_error"] = last_error
        out["endpoints"][name] = entry
    return out


def bench_link(get_fn: Callable[..., float], samples: int = 20) -> dict:
    """HAT round-trip latency via repeated /api/hat polls.

    Honesty note baked into the output, not just this docstring: this is
    HTTP + HAT UART round trip as observed from the S3's HTTP handler, NOT
    the raw UART link. There is no host-reachable surface that measures the
    UART hop alone.
    """
    lat: list = []
    errors = 0
    last_error = ""
    for _ in range(samples):
        try:
            lat.append(get_fn("/hat", None, False))
        except Exception as exc:
            errors += 1
            last_error = str(exc)
    entry = compute_stats(lat, "ms")
    entry["errors"] = errors
    entry["error_rate"] = errors / samples if samples else 0.0
    if last_error:
        entry["last_error"] = last_error
    entry["note"] = ("measures HTTP + HAT UART round trip end to end, as "
                      "seen from the S3's /api/hat handler -- NOT the raw "
                      "UART link in isolation")
    return {"available": True, "round_trip": entry}


def run_http(host: str, port: int, token: Optional[str], samples: int,
              timeout: float, idle_wait: float) -> dict:
    try:
        import requests
    except ImportError as exc:
        return {"available": False, "error": f"requests not installed: {exc}"}
    session = requests.Session()
    base = f"http://{host}:{port}/api"
    get_fn = _TimedGet(session, base, token, timeout)
    try:
        return bench_http(get_fn, samples=samples, idle_wait=idle_wait)
    except Exception as exc:
        return {"available": False, "error": str(exc)}


def run_link(host: str, port: int, token: Optional[str], samples: int,
             timeout: float) -> dict:
    try:
        import requests
    except ImportError as exc:
        return {"available": False, "error": f"requests not installed: {exc}"}
    session = requests.Session()
    base = f"http://{host}:{port}/api"
    get_fn = _TimedGet(session, base, token, timeout)
    try:
        return bench_link(get_fn, samples=samples)
    except Exception as exc:
        return {"available": False, "error": str(exc)}


# ---------------------------------------------------------------------------
# daq-usb surface -- delegates to the existing tool, does not reimplement it
# ---------------------------------------------------------------------------

def run_daq_usb(seconds: float, chunk: int, timeout_ms: int) -> dict:
    try:
        daq_bench = importlib.import_module("tests.tools.daq_usb_stream_bench")
    except ImportError as exc:
        return {"available": False, "error": f"cannot import daq_usb_stream_bench: {exc}"}

    try:
        res = daq_bench.run(seconds, chunk, timeout_ms)
    except Exception as exc:
        return {"available": False, "error": str(exc)}

    if res.error:
        return {"available": False, "error": res.error}

    from dataclasses import asdict
    payload = asdict(res)
    payload["mb_per_s"] = res.mb_per_s
    payload["wave_i_sps"] = res.wave_i_sps
    payload["wave_v_sps"] = res.wave_v_sps
    payload["frame_loss_pct"] = res.frame_loss_pct
    payload["available"] = True
    return payload


# ---------------------------------------------------------------------------
# ota surface -- opt-in only, never flashes by default
# ---------------------------------------------------------------------------

def run_ota(host: str, port: int, token: Optional[str], image: Optional[str],
            do_flash: bool, include_slow: bool, p4_image: Optional[str],
            p4_do_flash: bool, timeout: float) -> dict:
    if not (image and do_flash):
        return {
            "available": False,
            "skipped": True,
            "note": "opt-in required -- pass --image PATH --do-flash to "
                    "measure S3 OTA upload throughput. THIS WRITES FIRMWARE "
                    "AND REBOOTS THE DEVICE. Never enabled by default.",
        }
    if not token:
        return {"available": False, "error": "OTA requires an admin token "
                "(--token or BB_TOKEN)"}

    from bugbuster.transport import HTTPTransport
    from bugbuster.ota import OTAClient, OTAError

    out: dict = {"available": True, "s3": {}}
    try:
        size = os.path.getsize(image)
        transport = HTTPTransport(host, port=port, admin_token=token,
                                   timeout=timeout)
        ota = OTAClient(transport)
        start = time.monotonic()
        ota.upload_firmware(image, timeout=max(timeout, 120.0))
        elapsed = time.monotonic() - start
        out["s3"] = {
            "bytes": size,
            "seconds": elapsed,
            "bytes_per_sec": size / elapsed if elapsed else 0.0,
            "unit": "bytes/s",
        }
    except (OTAError, Exception) as exc:  # noqa: BLE001 -- degrade, don't crash the run
        out["s3"] = {"available": False, "error": str(exc)}

    out["p4"] = {"available": False, "skipped": True,
                 "note": "pass --include-slow --p4-image PATH --p4-do-flash "
                         "to measure P4 push throughput (also flashes)"}
    if include_slow and p4_image and p4_do_flash:
        try:
            from bugbuster.transport import HTTPTransport as _HT
            from bugbuster.ota import OTAClient as _OC
            transport2 = _HT(host, port=port, admin_token=token, timeout=timeout)
            ota2 = _OC(transport2)
            size2 = os.path.getsize(p4_image)
            start2 = time.monotonic()
            ota2.upload_p4(p4_image, timeout=600)
            elapsed2 = time.monotonic() - start2
            out["p4"] = {
                "available": True,
                "bytes": size2,
                "seconds": elapsed2,
                "bytes_per_sec": size2 / elapsed2 if elapsed2 else 0.0,
                "unit": "bytes/s",
            }
        except Exception as exc:
            out["p4"] = {"available": False, "error": str(exc)}
    return out


# ---------------------------------------------------------------------------
# mem surface -- S3 serial CLI (`heap`, `stack_hwm`)
# ---------------------------------------------------------------------------

_ANSI_RE = re.compile(r"\x1b\[[0-9;]*m")

_HEAP_FIELDS = [
    ("internal_free_kb", r"Internal free:\s*(\d+)\s*KB"),
    ("internal_min_ever_kb", r"Internal min-ever:\s*(\d+)\s*KB"),
    ("internal_largest_kb", r"Internal largest:\s*(\d+)\s*KB"),
    ("psram_free_kb", r"PSRAM free:\s*(\d+)\s*KB"),
    ("psram_largest_kb", r"PSRAM largest:\s*(\d+)\s*KB"),
]

_STACK_ROW_RE = re.compile(
    r"^\s*(\S+)\s+(\d+)\s+(\d+)\s+(\d+)\s*(.*?)\s*$")
_STACK_MISSING_RE = re.compile(r"^\s*(\S+)\s+handle not found")


class SerialCli:
    """Minimal ANSI-aware serial console driver for the S3's BugBuster CLI.

    Deliberately separate from tests/lib/p4_console.py's P4Console: that
    driver matches the P4's plain "daq> " prompt, while the S3 CLI prompt is
    wrapped in ANSI color codes ("\\x1b[1m\\x1b[96m[BugBuster]\\x1b[0m> ")
    that need stripping before any prompt match can work, and the two
    consoles' probe strategy (port discovery, aliveness check) differs
    enough that folding them into one class would just add branching.
    """

    PROMPT = "]> "

    def __init__(self, port: str, baud: int = 115200, timeout: float = 1.0,
                 _serial=None):
        self.port = port
        if _serial is not None:
            self.ser = _serial
            return
        try:
            import serial
        except ImportError as exc:
            raise SurfaceUnavailable(f"pyserial is required: {exc}") from exc
        try:
            self.ser = serial.Serial(port, baud, timeout=timeout)
        except Exception as exc:
            raise SurfaceUnavailable(f"cannot open {port}: {exc}") from exc
        time.sleep(0.3)
        self.ser.reset_input_buffer()

    def cmd(self, text: str, deadline: float = 3.0) -> str:
        self.ser.reset_input_buffer()
        self.ser.write((text + "\r\n").encode())
        buf = ""
        end = time.monotonic() + deadline
        while time.monotonic() < end:
            chunk = self.ser.read(4096)
            if chunk:
                buf += chunk.decode("utf8", "replace")
                if self.PROMPT in _ANSI_RE.sub("", buf):
                    break
            elif buf:
                break
            else:
                time.sleep(0.01)
        if not buf:
            raise TimeoutError(f"no response within {deadline:.1f}s to {text!r}")
        return _ANSI_RE.sub("", buf)

    def close(self) -> None:
        try:
            self.ser.close()
        except Exception:
            pass

    def __enter__(self) -> "SerialCli":
        return self

    def __exit__(self, *exc) -> None:
        self.close()


def parse_heap(text: str) -> dict:
    out = {}
    for key, pattern in _HEAP_FIELDS:
        m = re.search(pattern, text)
        out[key] = int(m.group(1)) if m else None
    out["unit"] = "KB"
    return out


def parse_stack_hwm(text: str) -> dict:
    tasks: dict = {}
    for line in text.splitlines():
        m = _STACK_MISSING_RE.match(line)
        if m:
            tasks[m.group(1)] = {"available": False}
            continue
        m = _STACK_ROW_RE.match(line)
        if not m:
            continue
        name, declared, unused, peak, flag = m.groups()
        tasks[name] = {
            "declared_bytes": int(declared),
            "unused_bytes": int(unused),
            "peak_used_bytes": int(peak),
            "unit": "bytes",
            "flag": flag or "",
        }
    return tasks


def bench_mem(cli: SerialCli) -> dict:
    heap_text = cli.cmd("heap")
    stack_text = cli.cmd("stack_hwm")
    return {
        "available": True,
        "heap": parse_heap(heap_text),
        "stack_hwm": parse_stack_hwm(stack_text),
    }


def run_mem(serial_port: Optional[str], baud: int) -> dict:
    if not serial_port:
        return {"available": False, "error": "no --serial-port given"}
    try:
        with SerialCli(serial_port, baud=baud) as cli:
            return bench_mem(cli)
    except SurfaceUnavailable as exc:
        return {"available": False, "error": str(exc)}
    except Exception as exc:
        return {"available": False, "error": str(exc)}


# ---------------------------------------------------------------------------
# report assembly / printing
# ---------------------------------------------------------------------------

def new_report(host: str) -> dict:
    return {
        "tool": "bb_bench",
        "schema_version": SCHEMA_VERSION,
        "generated_at": datetime.now(timezone.utc).isoformat(),
        "host": host,
        "not_measured": list(NOT_MEASURED),
        "surfaces": {},
    }


def print_surface(name: str, data: dict) -> None:
    print(f"\n== {name} ==")
    if not data.get("available", True) and "endpoints" not in data:
        note = data.get("note") or data.get("error") or "unavailable"
        print(f"  unavailable: {note}")
        return

    if name == "http":
        if data.get("cold_ms") is not None:
            print(f"  cold ({data.get('cold_endpoint')}): {data['cold_ms']:.1f} ms")
        for ep, stats in data.get("endpoints", {}).items():
            if stats["count"] == 0:
                print(f"  {ep:<16} no successful samples "
                      f"(errors={stats['errors']})")
                continue
            print(f"  {ep:<16} min={stats['min']:.1f} median={stats['median']:.1f} "
                  f"p95={stats['p95']:.1f} max={stats['max']:.1f} ms  "
                  f"n={stats['count']} errors={stats['errors']}")
    elif name == "link":
        rt = data.get("round_trip", {})
        print(f"  {rt.get('note', '')}")
        if rt.get("count"):
            print(f"  round trip: min={rt['min']:.1f} median={rt['median']:.1f} "
                  f"p95={rt['p95']:.1f} max={rt['max']:.1f} ms  n={rt['count']} "
                  f"errors={rt['errors']}")
        else:
            print(f"  no successful samples (errors={rt.get('errors')})")
    elif name == "mem":
        heap = data.get("heap", {})
        print("  heap: " + "  ".join(f"{k}={v}" for k, v in heap.items() if k != "unit"))
        for task, t in data.get("stack_hwm", {}).items():
            if not t.get("available", True):
                print(f"    {task}: handle not found")
                continue
            print(f"    {task:<10} declared={t['declared_bytes']:>6}B "
                  f"unused={t['unused_bytes']:>6}B peak={t['peak_used_bytes']:>6}B"
                  f"{'  ' + t['flag'] if t['flag'] else ''}")
    elif name == "daq_usb":
        if data.get("available"):
            print(f"  host throughput   : {data['mb_per_s']:.2f} MB/s")
            print(f"  WAVE_I end-to-end : {data['wave_i_sps']:,.0f} Sa/s")
            print(f"  WAVE_V end-to-end : {data['wave_v_sps']:,.0f} Sa/s")
            print(f"  wire frame loss   : {data['seq_lost']} frames "
                  f"({data['frame_loss_pct']:.3f}%)")
    elif name == "ota":
        s3 = data.get("s3", {})
        if s3.get("bytes_per_sec"):
            print(f"  S3 upload: {s3['bytes_per_sec']/1024:.1f} KB/s "
                  f"({s3['bytes']} bytes in {s3['seconds']:.2f}s)")
        else:
            print(f"  S3: {s3.get('note') or s3.get('error') or 'skipped'}")
        p4 = data.get("p4", {})
        if p4.get("bytes_per_sec"):
            print(f"  P4 push: {p4['bytes_per_sec']/1024:.1f} KB/s")
        else:
            print(f"  P4: {p4.get('note') or p4.get('error') or 'skipped'}")


def print_report(report: dict) -> None:
    print(f"bb_bench report -- host={report['host']}  {report['generated_at']}")
    print("\nthis tool does NOT measure:")
    for item in report["not_measured"]:
        print(f"  - {item}")
    for name, data in report["surfaces"].items():
        print_surface(name, data)


# ---------------------------------------------------------------------------
# compare / delta
# ---------------------------------------------------------------------------

_LOWER_BETTER_HINTS = (
    "error", "lost", "loss", "latency", "_ms", "drop", "overflow", "gap",
    "cold_ms", "min", "median", "p95", "max", "unused_bytes",
)
_HIGHER_BETTER_HINTS = (
    "mb_per_s", "sps", "bytes_per_sec", "throughput", "largest", "free",
    "declared_bytes", "peak_used_bytes",
)


def _direction(key_path: str) -> Optional[bool]:
    """True = higher is better, False = lower is better, None = neutral.

    A simple substring heuristic over the flattened key path. Good enough
    for a delta table meant to catch a human's eye, not for automated
    pass/fail gating (unlike the daq_usb_stream_bench tool's compare(),
    which is strict on purpose because it also returns a CI exit code).
    """
    lower = key_path.lower()
    for hint in _LOWER_BETTER_HINTS:
        if hint in lower:
            return False
    for hint in _HIGHER_BETTER_HINTS:
        if hint in lower:
            return True
    return None


def _flatten(d: Any, prefix: str = "") -> dict:
    out = {}
    if isinstance(d, dict):
        for k, v in d.items():
            path = f"{prefix}.{k}" if prefix else str(k)
            out.update(_flatten(v, path))
    elif isinstance(d, (int, float)) and not isinstance(d, bool):
        out[prefix] = d
    return out


def format_delta_row(key: str, before: float, after: float) -> str:
    """Render one before/after/delta/pct row, flagged when direction hints
    say the change is a regression. Pure formatting -- no I/O -- so it is
    directly unit-testable."""
    delta = after - before
    pct = (100.0 * delta / before) if before else (float("inf") if delta else 0.0)
    direction = _direction(key)
    flag = ""
    if abs(pct) >= 1.0 and direction is not None:
        improved = (delta > 0) if direction else (delta < 0)
        flag = "  ok" if improved else "  <-- REGRESSION"
    return (f"{key:<40} {before:>14,.3f} {after:>14,.3f} {delta:>+14,.3f} "
            f"{pct:>+8.1f}%{flag}")


def compare_reports(before: dict, after: dict) -> int:
    bf = _flatten(before.get("surfaces", before))
    af = _flatten(after.get("surfaces", after))
    common = sorted(set(bf) & set(af))
    only_before = sorted(set(bf) - set(af))
    only_after = sorted(set(af) - set(bf))

    print(f"{'metric':<40} {'before':>14} {'after':>14} {'delta':>14} {'pct':>9}")
    print("-" * 100)
    bad = 0
    for key in common:
        row = format_delta_row(key, bf[key], af[key])
        print(row)
        if "REGRESSION" in row:
            bad += 1

    if only_before:
        print(f"\nmetrics only in BEFORE (surface unavailable in AFTER?): "
              f"{', '.join(only_before[:10])}")
    if only_after:
        print(f"\nmetrics only in AFTER (surface newly available?): "
              f"{', '.join(only_after[:10])}")

    print()
    print("FAIL: regression(s) detected." if bad else "OK: no flagged regressions.")
    return 1 if bad else 0


def compare(before_path: str, after_path: str) -> int:
    with open(before_path) as f:
        before = json.load(f)
    with open(after_path) as f:
        after = json.load(f)
    print(f"\n== {before_path} -> {after_path} ==")
    return compare_reports(before, after)


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------

def _env_default(name: str, fallback: Optional[str] = None) -> Optional[str]:
    return os.environ.get(name, fallback)


def _add_common_net_args(sp: argparse.ArgumentParser) -> None:
    sp.add_argument("--host", default=_env_default("BB_HOST", "192.168.4.1"),
                     help="device IP/hostname (env BB_HOST)")
    sp.add_argument("--port", type=int, default=80)
    sp.add_argument("--token", default=_env_default("BB_TOKEN"),
                     help="admin token (env BB_TOKEN); required for "
                          "admin-gated endpoints (update/status, ota)")
    sp.add_argument("--timeout", type=float, default=5.0,
                     help="per-request timeout, seconds")
    sp.add_argument("--samples", type=int, default=20,
                     help="samples per endpoint")


def build_parser() -> argparse.ArgumentParser:
    ap = argparse.ArgumentParser(
        prog="bb_bench.py",
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    ap.add_argument("--json", help="write the combined report here")
    ap.add_argument("--compare", nargs=2, metavar=("BEFORE", "AFTER"),
                     help="compare two previously-written --json reports "
                          "and exit (ignores any subcommand)")

    sub = ap.add_subparsers(dest="cmd")

    sp_all = sub.add_parser("all", help="run every non-flashing surface")
    _add_common_net_args(sp_all)
    sp_all.add_argument("--serial-port", default=_env_default("BB_SERIAL"))
    sp_all.add_argument("--serial-baud", type=int, default=115200)
    sp_all.add_argument("--seconds", type=float, default=5.0,
                         help="daq-usb capture window")
    sp_all.add_argument("--skip", action="append", default=[],
                         choices=["http", "daq-usb", "link", "mem"],
                         help="skip one of the non-flashing surfaces")
    sp_all.add_argument("--idle-wait", type=float, default=0.0)

    sp_http = sub.add_parser("http", help="S3 HTTP/API latency")
    _add_common_net_args(sp_http)
    sp_http.add_argument("--idle-wait", type=float, default=0.0,
                          help="sleep this long before the cold sample, to "
                               "let WiFi actually enter power-save")

    sp_link = sub.add_parser(
        "link", help="HAT round-trip latency (HTTP+UART, NOT UART alone)")
    _add_common_net_args(sp_link)

    sp_daq = sub.add_parser(
        "daq-usb", help="DAQ HAT USB-HS stream (delegates to daq_usb_stream_bench.py)")
    sp_daq.add_argument("--seconds", type=float, default=5.0)
    sp_daq.add_argument("--chunk", type=int, default=262144)
    sp_daq.add_argument("--timeout-ms", type=int, default=1000)

    sp_mem = sub.add_parser("mem", help="S3 memory snapshot over the serial CLI")
    sp_mem.add_argument("--serial-port", default=_env_default("BB_SERIAL"),
                         help="e.g. /dev/cu.usbmodem1234561 (env BB_SERIAL)")
    sp_mem.add_argument("--serial-baud", type=int, default=115200)

    sp_ota = sub.add_parser(
        "ota",
        help="S3/P4 OTA upload throughput -- WARNING: WRITES FIRMWARE AND "
             "REBOOTS THE DEVICE when opted in; never runs by default",
    )
    _add_common_net_args(sp_ota)
    sp_ota.add_argument("--image", help="firmware.bin to upload to the S3")
    sp_ota.add_argument(
        "--do-flash", action="store_true",
        help="REQUIRED to actually upload/flash the S3 image. Without this "
             "flag the 'ota' subcommand only reports what it would measure "
             "and touches nothing.")
    sp_ota.add_argument(
        "--include-slow", action="store_true",
        help="also attempt a P4 push benchmark (requires --p4-image and "
             "--p4-do-flash). ALSO FLASHES.")
    sp_ota.add_argument("--p4-image")
    sp_ota.add_argument("--p4-do-flash", action="store_true")

    return ap


def _run_surface(name: str, fn) -> dict:
    """Run one surface's entry point, converting any exception that escaped
    its own try/except into {"available": False} so one bad surface never
    takes the whole combined run down with it."""
    try:
        return fn()
    except Exception as exc:  # noqa: BLE001 -- last-resort guard, see docstring
        return {"available": False, "error": f"unhandled: {exc}"}


def main(argv=None) -> int:
    ap = build_parser()
    args = ap.parse_args(argv)

    if args.compare:
        return compare(*args.compare)

    if not args.cmd:
        ap.print_help()
        return 2

    report = new_report(getattr(args, "host", "n/a"))

    if args.cmd == "all":
        if "http" not in args.skip:
            report["surfaces"]["http"] = _run_surface(
                "http", lambda: run_http(args.host, args.port, args.token,
                                          args.samples, args.timeout, args.idle_wait))
        if "link" not in args.skip:
            report["surfaces"]["link"] = _run_surface(
                "link", lambda: run_link(args.host, args.port, args.token,
                                          args.samples, args.timeout))
        if "mem" not in args.skip:
            report["surfaces"]["mem"] = _run_surface(
                "mem", lambda: run_mem(args.serial_port, args.serial_baud))
        if "daq-usb" not in args.skip:
            report["surfaces"]["daq_usb"] = _run_surface(
                "daq_usb", lambda: run_daq_usb(args.seconds, 262144, 1000))
        # ota is intentionally never included in "all" -- opt in via the
        # dedicated `ota` subcommand, which itself defaults to a no-op.
        report["surfaces"]["ota"] = {
            "available": False, "skipped": True,
            "note": "ota is never run as part of 'all' -- it writes "
                    "firmware. Use the dedicated `ota` subcommand.",
        }
    elif args.cmd == "http":
        report["surfaces"]["http"] = run_http(
            args.host, args.port, args.token, args.samples, args.timeout,
            args.idle_wait)
    elif args.cmd == "link":
        report["surfaces"]["link"] = run_link(
            args.host, args.port, args.token, args.samples, args.timeout)
    elif args.cmd == "mem":
        report["surfaces"]["mem"] = run_mem(args.serial_port, args.serial_baud)
    elif args.cmd == "daq-usb":
        report["surfaces"]["daq_usb"] = run_daq_usb(
            args.seconds, args.chunk, args.timeout_ms)
    elif args.cmd == "ota":
        report["surfaces"]["ota"] = run_ota(
            args.host, args.port, args.token, args.image, args.do_flash,
            args.include_slow, args.p4_image, args.p4_do_flash, args.timeout)

    print_report(report)

    if args.json:
        with open(args.json, "w") as f:
            json.dump(report, f, indent=2)
        print(f"\nwrote {args.json}")

    return 0


if __name__ == "__main__":
    sys.exit(main())
