#!/usr/bin/env python3
"""
fwlab.py - firmware flash/verify fixture for the fix campaign.

Flashing is the expensive step in this repo, and every firmware fix needs the
same three things around it: a before snapshot, the flash, an after snapshot,
and a diff that tells you whether anything regressed. Doing that by hand each
time is slow and inconsistent. This wraps it.

It composes existing tooling rather than reimplementing it:
  - tests/tools/subsystem_liveness.py  active per-subsystem probes
  - tests/tools/daq_push.py            C6 merged-image push
  - python/bugbuster/ota.py            OTAClient for S3 / SPIFFS / P4 / C6

CALIBRATION SAFETY
------------------
Calibration data is irreplaceable without a bench reference. Every snapshot
records a *calibration fingerprint* (point counts and have-flags only - it
never writes). ``diff`` treats any change to that fingerprint as a hard
failure, not a warning, and ``cycle`` aborts before flashing if it cannot read
the fingerprint at all. Nothing here ever calls IDAC_CALIBRATE (0xA3),
IDAC_CAL_CLEAR (0xA5), IDAC_CAL_SAVE (0xA6) or the P4 ``cal i`` / ``cal v``
console commands.

USAGE
-----
    # one-shot: snapshot, build, flash, re-snapshot, diff, smoke
    python tests/tools/fwlab.py cycle --target p4 --smoke daq

    # or step by step
    python tests/tools/fwlab.py snapshot --tag pre
    python tests/tools/fwlab.py flash --target p4
    python tests/tools/fwlab.py snapshot --tag post
    python tests/tools/fwlab.py diff pre post

Snapshots land in tests/tools/baselines/fwlab/<tag>.json.

Exit codes: 0 OK, 1 regression or cal drift, 2 setup error.
"""
from __future__ import annotations

import argparse
import json
import subprocess
import sys
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Callable

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "python"))

import bugbuster as bb  # noqa: E402

SNAP_DIR = REPO / "tests/tools/baselines/fwlab"

# Cached admin token, fetched over USB once (GET_ADMIN_TOKEN is USB-only).
# scratch/ is gitignored, so the token never reaches the repository.
TOKEN_FILE = REPO / "scratch/.admin_token"


def _token(explicit: str | None) -> str | None:
    if explicit:
        return explicit
    if TOKEN_FILE.is_file():
        return TOKEN_FILE.read_text().strip()
    return None

# Fields that legitimately differ between two snapshots of a healthy device.
# Anything not matched here that changes is reported as a difference.
VOLATILE = (
    "uptime", "seq", "timestamp", "captured_at", "_us", "temp", "temperature",
    "die_temp", "rssi", "heap_free", "free_heap", "min_free", "largest_free",
    "psram", "live_status",
    "frames_tx", "bytes_per_sec", "wave_i_frames", "wave_v_frames",
    "wave_i_index_lo", "ring_high_water", "adc_raw", "adc_value", "din_counter",
    "current_ma", "in_voltage", "in_current", "detect_voltage",
)

# Relative change below this on a numeric field is measurement noise, not a
# regression. Keeps a real supply fault visible while ignoring ADC jitter.
NUM_TOL = 0.02


def _same_number(a: Any, b: Any) -> bool:
    if isinstance(a, bool) or isinstance(b, bool):
        return a == b
    if not (isinstance(a, (int, float)) and isinstance(b, (int, float))):
        return False
    if a == b:
        return True
    scale = max(abs(a), abs(b))
    return scale > 0 and abs(a - b) / scale <= NUM_TOL


def _volatile(path: str) -> bool:
    low = path.lower()
    return any(v in low for v in VOLATILE)


# ---------------------------------------------------------------------------
# Snapshot
# ---------------------------------------------------------------------------

@dataclass
class Probe:
    name: str
    fn: Callable[[Any], Any]
    critical: bool = False   # failure here aborts a cycle before flashing


def _probes() -> list[Probe]:
    return [
        Probe("device_info", lambda c: _asdict(c.get_device_info()), critical=True),
        Probe("ping", lambda c: _asdict(c.ping())),
        Probe("status", lambda c: c.get_status(), critical=True),
        Probe("memory", lambda c: _asdict(c.get_memory_status())),
        Probe("idac_cal", lambda c: c.idac_get_status(), critical=True),
        Probe("hat_rails", lambda c: c.hat_get_rail_status()),
        Probe("hat_cal", lambda c: c.hat_calibrate_status()),
        Probe("daq_stream", lambda c: _daq_stream_status()),
    ]


def _asdict(obj: Any) -> Any:
    if hasattr(obj, "__dict__"):
        return {k: v for k, v in vars(obj).items() if not k.startswith("_")}
    return obj


def _daq_stream_status() -> Any:
    """Poll the P4 data plane for a STATUS record. Read-only: start/stop only."""
    from bugbuster.daq_stream import DaqStream
    stream = DaqStream()
    try:
        stream.open()
        stream.start()
        deadline = time.monotonic() + 2.0
        while time.monotonic() < deadline:
            for rec in stream.read_records(timeout_ms=200):
                raw = getattr(rec, "raw", None)
                if isinstance(raw, dict) and "sample_rate" in raw:
                    stream.stop()
                    return raw
        stream.stop()
        raise RuntimeError("no STATUS record within 2 s")
    finally:
        stream.close()


def snapshot(client, tag: str) -> dict:
    """Run every probe and persist the result. Never writes device state."""
    out: dict[str, Any] = {
        "tag": tag,
        "captured_at": time.time(),
        "probes": {},
    }
    for p in _probes():
        try:
            out["probes"][p.name] = {"ok": True, "data": p.fn(client)}
        except Exception as exc:  # probe failure is data, not a crash
            out["probes"][p.name] = {
                "ok": False,
                "error": f"{type(exc).__name__}: {exc}",
                "critical": p.critical,
            }
    out["cal_fingerprint"] = _cal_fingerprint(out)
    SNAP_DIR.mkdir(parents=True, exist_ok=True)
    (SNAP_DIR / f"{tag}.json").write_text(json.dumps(out, indent=2, default=str))
    return out


# Substrings identifying a calibration-bearing field. Matched case-insensitively
# against key names, so this survives the naming drift between surfaces
# (calibrated / polyValid / calPoly / points / have / count).
CAL_KEYS = ("calibrat", "calpoly", "polyvalid", "point", "have", "count", "valid")


def _cal_fingerprint(snap: dict) -> dict:
    """Point counts, have-flags and cal polynomials - what a stray write changes."""
    fp: dict[str, Any] = {}
    for probe in ("idac_cal", "hat_cal", "daq_stream"):
        p = snap["probes"].get(probe, {})
        if p.get("ok"):
            got = _extract(p["data"], CAL_KEYS)
            if got:
                fp[probe] = got
    return fp


def _extract(obj: Any, keys: tuple[str, ...]) -> Any:
    """Recursively pull out any key whose name contains one of *keys*."""
    if isinstance(obj, dict):
        got = {k: v for k, v in obj.items()
               if any(s in k.lower() for s in keys)}
        nested = {k: _extract(v, keys) for k, v in obj.items()
                  if isinstance(v, (dict, list))}
        got.update({k: v for k, v in nested.items() if v})
        return got
    if isinstance(obj, list):
        vals = [_extract(v, keys) for v in obj]
        return [v for v in vals if v]
    return None


# ---------------------------------------------------------------------------
# Diff
# ---------------------------------------------------------------------------

def _flatten(obj: Any, prefix: str = "") -> dict[str, Any]:
    flat: dict[str, Any] = {}
    if isinstance(obj, dict):
        for k, v in obj.items():
            flat.update(_flatten(v, f"{prefix}.{k}" if prefix else str(k)))
    elif isinstance(obj, list):
        for i, v in enumerate(obj):
            flat.update(_flatten(v, f"{prefix}[{i}]"))
    else:
        flat[prefix] = obj
    return flat


def diff(before: dict, after: dict) -> tuple[list[str], list[str], list[str]]:
    """Return (cal_violations, regressions, changes)."""
    cal_violations: list[str] = []
    if before.get("cal_fingerprint") != after.get("cal_fingerprint"):
        fb = _flatten(before.get("cal_fingerprint", {}))
        fa = _flatten(after.get("cal_fingerprint", {}))
        for k in sorted(set(fb) | set(fa)):
            vb, va = fb.get(k), fa.get(k)
            if vb == va:
                continue
            # A field that did not exist before is new firmware reporting more,
            # not calibration being altered. A field that VANISHES still counts,
            # since that hides cal state from the guard.
            if k not in fb:
                continue
            cal_violations.append(f"{k}: {vb!r} -> {va!r}")

    regressions: list[str] = []
    for name, pb in before["probes"].items():
        pa = after["probes"].get(name)
        if pa is None:
            regressions.append(f"probe {name} missing after")
        elif pb.get("ok") and not pa.get("ok"):
            regressions.append(f"probe {name} was OK, now fails: {pa.get('error')}")

    changes: list[str] = []
    fb, fa = _flatten(before["probes"]), _flatten(after["probes"])
    for k in sorted(set(fb) | set(fa)):
        if _volatile(k):
            continue
        vb, va = fb.get(k), fa.get(k)
        if vb != va and not _same_number(vb, va):
            changes.append(f"{k}: {vb!r} -> {va!r}")
    return cal_violations, regressions, changes


# ---------------------------------------------------------------------------
# Build + flash
# ---------------------------------------------------------------------------

BUILD = {
    "s3": ["pio", "run", "-e", "esp32s3"],
    "p4": ["pio", "run", "-e", "esp32p4"],
    "c6": ["pio", "run", "-e", "esp32c6"],
}
BUILD_DIR = {
    "s3": REPO / "Firmware/ESP32",
    "p4": REPO / "Firmware/DAQ_HAT/ESP32P4",
    "c6": REPO / "Firmware/DAQ_HAT/ESP32C6",
}


def build(target: str) -> None:
    if target == "rp2040":
        d = REPO / "Firmware/RP2040/build"
        _run(["cmake", ".."], d)
        _run(["ninja"], d)
        return
    _run(BUILD[target], BUILD_DIR[target])


def _run(cmd: list[str], cwd: Path) -> None:
    print(f"  $ {' '.join(cmd)}   (in {cwd})")
    r = subprocess.run(cmd, cwd=cwd)
    if r.returncode != 0:
        raise SystemExit(f"build failed: {' '.join(cmd)}")


def flash(target: str, *, host: str | None, token: str | None,
          upload_port: str | None = None) -> None:
    """Flash *target*. P4 goes over its own USB port via PIO, per project choice."""
    if target == "p4":
        # platformio.ini deliberately sets no upload_port. Several ESP devices
        # enumerate at once here (mainboard COM5/COM6, P4 console COM15), and
        # auto-detect could put the P4 image on the mainboard - which that same
        # ini documents as having bricked a board once. Refuse to guess.
        if not upload_port:
            raise SystemExit(
                "p4 flash requires --upload-port (the P4 console, e.g. COM15). "
                "Refusing to let PlatformIO auto-detect with several ESP "
                "devices enumerated.")
        _run(["pio", "run", "-e", "esp32p4", "-t", "upload",
              "--upload-port", upload_port], BUILD_DIR["p4"])
        return
    if target == "c6":
        _run([sys.executable, str(REPO / "tests/tools/daq_push.py"), "c6",
              "--host", host or "", "--token", token or ""], REPO)
        return
    if target in ("s3", "rp2040", "spiffs"):
        from bugbuster.ota import OTAClient
        from bugbuster.transport import HTTPTransport
        t = HTTPTransport(host)
        t.set_admin_token(token)
        ota = OTAClient(t)
        if target == "s3":
            img = BUILD_DIR["s3"] / ".pio/build/esp32s3/firmware.bin"
            print(f"    uploading {img} ({img.stat().st_size} bytes)")
            print(f"    {ota.upload_firmware(str(img))}")
        elif target == "spiffs":
            img = BUILD_DIR["s3"] / ".pio/build/esp32s3/spiffs.bin"
            print(f"    {ota.upload_spiffs(str(img))}")
        else:
            raise SystemExit("rp2040 OTA: use ota.apply_update(rp2040=True)")
        return
    raise SystemExit(f"unknown target {target}")


# ---------------------------------------------------------------------------
# Smoke suites - deliberately small; the 30 min full S3 suite is opt-in
# ---------------------------------------------------------------------------

SMOKE = {
    "core": ["tests/device/test_01_core.py"],
    "daq": ["tests/device/test_16_daq_stream.py", "tests/device/test_17_daq_control.py"],
    "hat": ["tests/device/test_11_hat.py"],
    "la": ["tests/device/test_la_usb_bulk.py"],
    "power": ["tests/device/test_05_power.py"],
    "unit": ["tests/unit"],
}


def smoke(name: str, extra: list[str]) -> int:
    paths = SMOKE.get(name, [name])
    cmd = [sys.executable, "-m", "pytest", "-q", *paths, *extra]
    print(f"  $ {' '.join(cmd)}")
    return subprocess.run(cmd, cwd=REPO).returncode


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------

def _connect(args) -> Any:
    if args.host:
        return bb.connect_http(args.host, admin_token=_token(args.token))
    port = args.port
    if not port:
        from bugbuster.discovery import find_usb_port
        port = find_usb_port()
        if not port:
            raise SystemExit("no BugBuster USB port found; pass --port or --host")
    return bb.connect_usb(port)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--host"); ap.add_argument("--token"); ap.add_argument("--port")
    ap.add_argument("--upload-port",
                    help="Target console port for PIO uploads (P4: e.g. COM15)")
    sub = ap.add_subparsers(dest="cmd", required=True)

    s = sub.add_parser("snapshot"); s.add_argument("--tag", required=True)
    d = sub.add_parser("diff"); d.add_argument("before"); d.add_argument("after")
    f = sub.add_parser("flash"); f.add_argument("--target", required=True)
    b = sub.add_parser("build"); b.add_argument("--target", required=True)
    c = sub.add_parser("cycle")
    c.add_argument("--target", required=True)
    c.add_argument("--smoke", default=None)
    c.add_argument("--no-build", action="store_true")
    c.add_argument("--settle", type=float, default=8.0,
                   help="seconds to wait for re-enumeration after flashing")
    c.add_argument("pytest_args", nargs="*")

    args = ap.parse_args()

    if args.cmd == "build":
        build(args.target); return 0

    if args.cmd == "diff":
        before = json.loads((SNAP_DIR / f"{args.before}.json").read_text())
        after = json.loads((SNAP_DIR / f"{args.after}.json").read_text())
        return _report(*diff(before, after))

    if args.cmd == "snapshot":
        with _connect(args) as c_:
            snap = snapshot(c_, args.tag)
        bad = [n for n, p in snap["probes"].items()
               if not p["ok"] and p.get("critical")]
        print(f"snapshot '{args.tag}' written; "
              f"{sum(p['ok'] for p in snap['probes'].values())}/"
              f"{len(snap['probes'])} probes OK")
        if bad:
            print(f"  CRITICAL probes failed: {bad}")
            return 2
        return 0

    if args.cmd == "flash":
        flash(args.target, host=args.host, token=_token(args.token),
              upload_port=args.upload_port); return 0

    # cycle
    print("[1/6] pre-flash snapshot")
    with _connect(args) as c_:
        pre = snapshot(c_, f"{args.target}-pre")
    if not pre["cal_fingerprint"]:
        print("  ABORT: no calibration fingerprint could be read. Refusing to "
              "flash - a cal-clobbering regression would be undetectable.")
        return 2
    crit = [n for n, p in pre["probes"].items() if not p["ok"] and p.get("critical")]
    if crit:
        print(f"  ABORT: critical probes already failing before flash: {crit}")
        return 2

    if not args.no_build:
        print(f"[2/6] build {args.target}")
        build(args.target)
    print(f"[3/6] flash {args.target}")
    flash(args.target, host=args.host, token=_token(args.token),
          upload_port=args.upload_port)

    print(f"[4/6] settle {args.settle}s")
    time.sleep(args.settle)

    print("[5/6] post-flash snapshot")
    with _connect(args) as c_:
        post = snapshot(c_, f"{args.target}-post")

    print("[6/6] diff")
    rc = _report(*diff(pre, post))
    if rc != 0:
        print("\nSTOPPING: verification regressed. Not running smoke tests.")
        return rc

    if args.smoke:
        return smoke(args.smoke, args.pytest_args)
    return 0


def _report(cal_violations: list[str], regressions: list[str],
            changes: list[str]) -> int:
    if cal_violations:
        print("\n  *** CALIBRATION FINGERPRINT CHANGED - THIS IS A HARD FAILURE ***")
        for c in cal_violations:
            print(f"    {c}")
    if regressions:
        print("\n  REGRESSIONS:")
        for r in regressions:
            print(f"    {r}")
    if changes:
        print(f"\n  changes ({len(changes)}, review for intent):")
        for c in changes[:40]:
            print(f"    {c}")
        if len(changes) > 40:
            print(f"    ... {len(changes) - 40} more")
    if not (cal_violations or regressions or changes):
        print("  identical (modulo volatile fields)")
    return 1 if (cal_violations or regressions) else 0


if __name__ == "__main__":
    sys.exit(main())
