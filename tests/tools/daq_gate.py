#!/usr/bin/env python3
"""
daq_gate.py — run the DAQ regression gate and print a summary.

This is the command you actually type. It shells pytest with the right markers
and flags, then prints a per-group pass/fail table, because a 40-line pytest
tail is not a report.

Usage:
    python3 tests/tools/daq_gate.py                    # fast tier (skips slow)
    python3 tests/tools/daq_gate.py --full             # everything except WiFi
    python3 tests/tools/daq_gate.py --full --wifi      # everything
    python3 tests/tools/daq_gate.py --load-ohms 470
"""
from __future__ import annotations

import argparse
import os
import subprocess
import sys

REPO = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
TIER_FILES = [
    ("Tier 1 — P4 USB-HS stream", "tests/device/test_16_daq_stream.py"),
    ("Tier 2 — BBP control plane", "tests/device/test_17_daq_control.py"),
    ("Tier 3 — api_core HTTP routes", "tests/device/test_18_daq_api.py"),
]


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--full", action="store_true",
                    help="include tests marked slow")
    ap.add_argument("--wifi", action="store_true",
                    help="include the WiFi tier (changes this host's network)")
    ap.add_argument("--load-ohms", type=float, default=None,
                    help="nominal DUT load, reporting only")
    ap.add_argument("--p4-serial", default=None, help="P4 console port")
    ap.add_argument("--device-usb", default=None,
                    help="S3 mainboard CDC0 port, required for tier 2")
    ap.add_argument("--device-http", default=None,
                    help="S3 mainboard HTTP IP, required for tier 3")
    args = ap.parse_args()

    failed = []
    for label, path in TIER_FILES:
        if not os.path.exists(os.path.join(REPO, path)):
            print("  SKIP  %-32s (not implemented yet)" % label)
            continue

        cmd = [sys.executable, "-m", "pytest", path, "-q", "--daq"]
        if not args.full:
            cmd += ["-m", "not slow"]
        if args.wifi:
            cmd += ["--daq-wifi"]
        if args.load_ohms is not None:
            cmd += ["--daq-load-ohms", str(args.load_ohms)]
        if args.p4_serial:
            cmd += ["--daq-p4-serial", args.p4_serial]
        if args.device_usb:
            cmd += ["--device-usb", args.device_usb]
        if args.device_http:
            cmd += ["--device-http", args.device_http]

        print("\n=== %s ===" % label)
        env = dict(os.environ, PYTHONPATH="python")
        rc = subprocess.run(cmd, cwd=REPO, env=env).returncode
        if rc != 0:
            failed.append(label)

    print("\n" + "=" * 60)
    if failed:
        print("DAQ GATE: FAIL — %s" % ", ".join(failed))
        return 1
    print("DAQ GATE: PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
