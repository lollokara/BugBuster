#!/usr/bin/env python3
# 10_persistent_session.py — Host-side script: persistent VM session across multiple evals.
#
# Purpose: demonstrate that globals survive across eval calls when using
#   ScriptSession (persist=True mode). Opens one session, sets up a Channel,
#   sweeps voltage in 0.5 V steps, reads back each value, and prints a table.
#
# Run (HTTP):
#   python 10_persistent_session.py --host 192.168.1.42 --token TOKEN
#
# Run (USB):
#   python 10_persistent_session.py --usb /dev/ttyACM0
#
# Expect: a two-column table of (set_v, read_v) pairs from 0.0 V to 5.0 V in
#   0.5 V steps. On calibrated hardware, read_v should track within ±50 mV.
#   On exit, the persistent VM is automatically reset via script_reset().
#
# Prerequisites:
#   pip install bugbuster  (or run from the repo: PYTHONPATH=python)
#   Device must be on the same network (HTTP) or connected via USB.
#   Admin token required for eval (pass --token or let USB derive it).
#
# Dependencies: bugbuster (host-side library), requests, argparse (stdlib)

from __future__ import annotations

import argparse
import ast
import sys


def _make_client(args):
    from bugbuster.client import BugBuster
    from bugbuster.transport.http import HTTPTransport
    from bugbuster.transport.usb import USBTransport

    if args.host:
        t = HTTPTransport(args.host)
        bb = BugBuster(t)
        bb.connect()
        if args.token:
            bb._admin_token = args.token
        return bb
    elif args.usb:
        t = USBTransport(args.usb)
        bb = BugBuster(t)
        bb.connect()
        return bb
    else:
        raise SystemExit("error: specify --host <ip> or --usb <port>")


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(
        description="Persistent VM session: DAC sweep + ADC readback table",
    )
    conn = parser.add_mutually_exclusive_group(required=True)
    conn.add_argument("--host", metavar="IP",   help="Device IP address (HTTP)")
    conn.add_argument("--usb",  metavar="PORT",  help="USB serial port path")
    parser.add_argument("--token", metavar="TOKEN", help="Admin token (HTTP only)")
    args = parser.parse_args(argv)

    bb = _make_client(args)
    try:
        with bb.script_session() as s:
            # Set up channel once; globals persist for the lifetime of the session.
            s.eval("import bugbuster")
            s.eval("ch = bugbuster.Channel(0)")
            s.eval("ch.set_function(bugbuster.FUNC_VOUT)")

            steps = [round(v * 0.5, 1) for v in range(11)]  # 0.0 .. 5.0

            rows = []
            for v in steps:
                out = s.eval(
                    "ch.set_voltage(%(v)s); bugbuster.sleep(80); print(ch.read_voltage())" % {"v": v}
                )
                line = out.strip().splitlines()[-1] if out.strip() else ""
                try:
                    readback = float(ast.literal_eval(line))
                except (ValueError, SyntaxError):
                    readback = float("nan")
                rows.append((v, readback))

            # Return channel to high-impedance before exiting.
            s.eval("ch.set_function(bugbuster.FUNC_HIGH_IMP)", log_drain=False)
    finally:
        bb.disconnect()

    # Print table
    print("set_v    read_v   delta_v")
    print("-" * 28)
    for set_v, read_v in rows:
        delta = read_v - set_v
        print("%.2f V   %.5f V  %+.5f V" % (set_v, read_v, delta))

    return 0


if __name__ == "__main__":
    sys.exit(main())
