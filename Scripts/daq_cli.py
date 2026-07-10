#!/usr/bin/env python3
"""Tiny helper to drive the DAQ HAT (ESP32-P4) CLI over a serial port.

Usage:
    python daq_cli.py COM15 "adaqreg 0 scratch" "adaqspi 3" ...

Sends each command, waits for the `daq>` prompt (or a timeout), and prints
everything the device returns. Read-only: it never flashes the device.
"""
import sys
import time
import serial


def drain(ser, settle=0.35, hard_timeout=6.0):
    """Read until the prompt returns or we go quiet for `settle` seconds."""
    buf = bytearray()
    t0 = time.time()
    last = time.time()
    while True:
        chunk = ser.read(4096)
        if chunk:
            buf.extend(chunk)
            last = time.time()
            if b"daq>" in buf and (time.time() - last) >= 0.0:
                # keep reading a touch in case more trails after the prompt
                pass
        else:
            if (time.time() - last) >= settle:
                break
        if (time.time() - t0) >= hard_timeout:
            break
    return buf.decode(errors="replace")


def main():
    port = sys.argv[1]
    cmds = sys.argv[2:]
    ser = serial.Serial(port, 115200, timeout=0.1)
    time.sleep(0.2)
    # wake the prompt
    ser.write(b"\r\n")
    time.sleep(0.2)
    print(drain(ser), end="")
    for c in cmds:
        print(f"\n===> {c}")
        ser.write((c + "\r\n").encode())
        time.sleep(0.05)
        print(drain(ser), end="")
    ser.close()


if __name__ == "__main__":
    main()
