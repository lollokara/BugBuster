#!/usr/bin/env python3
# 13_on_device_decorator.py — Host-side script: manual @on_device-style pattern.
#
# Purpose: show how to ship a locally-defined function to the device, execute
#   it there, and recover its return value on the host via the print(repr(...))
#   convention that @on_device relies on.
#
#   The real @on_device decorator (bugbuster.script.on_device) works only when
#   a bugbuster_mcp session singleton is active. This example uses the identical
#   mechanics manually so it works in any environment (plain HTTP or USB).
#
# Run (HTTP):
#   python 13_on_device_decorator.py --host 192.168.1.42 --token TOKEN
#
# Run (USB):
#   python 13_on_device_decorator.py --usb /dev/ttyACM0
#
# Expect:
#   temperature: 23.12109 C
#   (Exact value depends on the sensor; with no device wired you may see 0.0
#   or an OSError from the I2C bus — both expected.)
#
# Prerequisites:
#   An I2C temperature sensor (e.g. LM75, TMP102) at 7-bit address 0x48 on
#   IO2 (SDA) / IO3 (SCL). Adjust addr if your sensor uses a different address.
#   pip install bugbuster  (or: PYTHONPATH=python)
#
# How the pattern works:
#   1. Define the function in a real .py file (inspect.getsource requires it).
#   2. Strip the decorator line(s) and dedent the source.
#   3. POST the source via script_eval(persist=True).
#   4. Call the function by name in a second eval; the last print(repr(...))
#      line carries the return value back to the host.
#   5. Drain logs and apply ast.literal_eval to the last non-empty line.

from __future__ import annotations

import argparse
import ast
import inspect
import sys
import textwrap


# ---------------------------------------------------------------------------
# Function to run on-device
# ---------------------------------------------------------------------------
# This function is defined here so inspect.getsource() can extract it.
# The body must be valid MicroPython and must end with print(repr(value)).

def read_temp_c(addr):
    # Runs on-device; imports and hardware access happen there.
    i2c = bugbuster.I2C(sda_io=2, scl_io=3, freq=400000)
    i2c.writeto(addr, b'\x00')
    raw = i2c.readfrom(addr, 2)
    temp = ((raw[0] << 8) | raw[1]) / 256.0
    print(repr(temp))


# ---------------------------------------------------------------------------
# Host-side plumbing
# ---------------------------------------------------------------------------

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


def _drain_logs(bb) -> str:
    chunks = []
    while True:
        chunk = bb.script_logs()
        if not chunk:
            break
        chunks.append(chunk)
    return "".join(chunks)


def _extract_source(func) -> str:
    """
    Extract function source, strip leading decorator lines, and dedent.
    Mirrors what the real @on_device decorator does at decoration time.
    """
    raw_lines = inspect.getsource(func).splitlines(keepends=True)
    stripped = []
    skip_decorator = True
    for line in raw_lines:
        if skip_decorator and line.lstrip().startswith("@"):
            continue
        skip_decorator = False
        stripped.append(line)
    return textwrap.dedent("".join(stripped))


def run_on_device(bb, func, *args):
    """
    Manual @on_device-style call:
    1. Upload the function definition to the persistent VM.
    2. Call it with *args* in a second eval.
    3. Drain logs and parse the last repr()-encoded line as the return value.
    """
    func_src = _extract_source(func)
    func_name = func.__name__

    # Upload the function definition (persist=True so it survives the next eval)
    bb.script_eval("import bugbuster\n" + func_src, persist=True)
    _drain_logs(bb)  # discard definition-phase output

    # Build the call expression
    arg_reprs = ", ".join(repr(a) for a in args)
    call_src = "%s(%s)" % (func_name, arg_reprs)
    bb.script_eval(call_src, persist=True)

    logs = _drain_logs(bb)
    lines = [ln for ln in logs.splitlines() if ln.strip()]
    if not lines:
        return None
    try:
        return ast.literal_eval(lines[-1])
    except (ValueError, SyntaxError):
        return None


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(
        description="Manual @on_device-style pattern: read I2C temperature",
    )
    conn = parser.add_mutually_exclusive_group(required=True)
    conn.add_argument("--host", metavar="IP",   help="Device IP address (HTTP)")
    conn.add_argument("--usb",  metavar="PORT",  help="USB serial port path")
    parser.add_argument("--token", metavar="TOKEN", help="Admin token (HTTP only)")
    parser.add_argument("--addr", type=lambda x: int(x, 0), default=0x48,
                        help="I2C sensor address (default: 0x48)")
    args = parser.parse_args(argv)

    bb = _make_client(args)
    try:
        temp = run_on_device(bb, read_temp_c, args.addr)
        if temp is None:
            print("no value returned (check sensor wiring or address)")
            return 1
        print("temperature: %.5f C" % temp)
    except OSError as e:
        print("transport error: %s" % e)
        return 1
    finally:
        bb.script_reset()
        bb.disconnect()

    return 0


if __name__ == "__main__":
    sys.exit(main())
