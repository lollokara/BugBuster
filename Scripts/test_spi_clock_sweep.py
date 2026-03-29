#!/usr/bin/env python3
"""
SPI Clock Sweep Test — reads ADC at increasing SPI clock speeds.
For each speed: set clock, verify, take 20 ADC readings, then next speed.
Starts at 1 MHz, increments by 1 MHz up to 20 MHz.
"""

import serial
import time
import sys
import re
import csv
from datetime import datetime

# ---------- Config ----------
PORT = "/dev/cu.usbmodem1234561"
BAUD = 115200
STEPS_MHZ = range(1, 21)          # 1 MHz to 20 MHz
READS_PER_STEP = 20
SETTLE_AFTER_CLOCK = 0.05         # 50ms settle after clock change
CMD_TIMEOUT = 0.6                 # max wait per command response
# ----------------------------

def open_serial(port, baud):
    ser = serial.Serial(port, baud, timeout=0.1)
    time.sleep(0.3)
    ser.reset_input_buffer()
    return ser

def is_noise(line):
    """Filter out ESP log spam (WiFi, system logs, ANSI, prompts)."""
    noise = ["wifi:", "wifi ", "STA disc", "ap channel", "new:<", "old:<",
             "prof:", "snd_ch", "I (", "W (", "E (", "esp_netif",
             ">>>", "BugBuster>"]
    return any(p in line for p in noise)

def send_cmd(ser, cmd):
    """Send a CLI command and return filtered response lines."""
    ser.reset_input_buffer()
    ser.write((cmd + "\r\n").encode())
    time.sleep(0.05)
    lines = []
    deadline = time.time() + CMD_TIMEOUT
    empty_count = 0
    while time.time() < deadline:
        raw = ser.readline()
        if not raw:
            empty_count += 1
            if empty_count >= 2:
                break
            continue
        empty_count = 0
        line = re.sub(r"\x1b\[[0-9;]*m", "", raw.decode(errors="replace")).strip()
        if line and not is_noise(line):
            lines.append(line)
    return lines

def parse_spiclock_verify(lines):
    for line in lines:
        if "Verify:" in line:
            ok = "OK" in line
            m = re.search(r"read 0x([0-9A-Fa-f]+)", line)
            return ok, m.group(1) if m else "????"
    return False, "NO_RESPONSE"

def parse_adc_readings(lines):
    readings = []
    for line in lines:
        m = re.match(r"\s*(\d)\s*\|.*\|\s*\d\s*\|\s*(\d+)\s*\|\s*([+-]?\d+\.\d+)\s*V", line)
        if m:
            readings.append((int(m.group(1)), int(m.group(2)), float(m.group(3))))
    return readings

def main():
    port = sys.argv[1] if len(sys.argv) > 1 else PORT

    print(f"=== SPI Clock Sweep Test ===")
    print(f"Port: {port}  |  1-20 MHz  |  {READS_PER_STEP} reads/step\n")

    ser = open_serial(port, BAUD)

    ts = datetime.now().strftime("%Y%m%d_%H%M%S")
    csv_path = f"spi_sweep_{ts}.csv"
    csvfile = open(csv_path, "w", newline="")
    writer = csv.writer(csvfile)
    writer.writerow(["clock_mhz", "clock_ok", "sample", "ch", "raw", "voltage"])

    results = []

    for mhz in STEPS_MHZ:
        hz = mhz * 1_000_000
        t0 = time.time()

        # Set clock
        resp = send_cmd(ser, f"spiclock {hz}")
        clock_ok, rb = parse_spiclock_verify(resp)

        if not clock_ok:
            print(f"{mhz:2d} MHz: FAIL (readback=0x{rb}) — stopping")
            writer.writerow([mhz, "FAIL", "", "", "", ""])
            results.append((mhz, "FAIL", 0, {}))
            send_cmd(ser, "spiclock 1000000")
            break

        time.sleep(SETTLE_AFTER_CLOCK)

        # Take ADC readings
        ch_v = {0: [], 1: [], 2: [], 3: []}
        errs = 0
        for i in range(READS_PER_STEP):
            readings = parse_adc_readings(send_cmd(ser, "adc"))
            if not readings:
                errs += 1
                continue
            for ch, raw, v in readings:
                ch_v[ch].append(v)
                writer.writerow([mhz, "OK", i + 1, ch, raw, f"{v:.6f}"])

        elapsed = time.time() - t0
        # One-line summary per step
        parts = []
        for ch in range(4):
            vals = ch_v[ch]
            if vals:
                avg = sum(vals) / len(vals)
                spread = max(vals) - min(vals)
                parts.append(f"CH{ch}:{avg:+.3f}V(±{spread:.4f})")
            else:
                parts.append(f"CH{ch}:---")
        err_str = f" [{errs} errs]" if errs else ""
        print(f"{mhz:2d} MHz: {' | '.join(parts)}{err_str}  ({elapsed:.1f}s)")

        results.append((mhz, "OK", sum(len(v) for v in ch_v.values()), ch_v))

    csvfile.close()

    # Restore
    send_cmd(ser, "spiclock 1000000")
    ser.close()

    # Summary
    print(f"\n{'='*80}")
    print(f"{'MHz':>4} | {'CH0 avg':>12} | {'CH1 avg':>12} | {'CH2 avg':>12} | {'CH3 avg':>12}")
    print(f"{'-'*4}-+-{'-'*12}-+-{'-'*12}-+-{'-'*12}-+-{'-'*12}")
    for mhz, status, total, ch_v in results:
        avgs = []
        for ch in range(4):
            vals = ch_v.get(ch, []) if isinstance(ch_v, dict) else []
            avgs.append(f"{sum(vals)/len(vals):+.5f}V" if vals else "     ---    ")
        print(f"{mhz:>4} | {' | '.join(avgs)}")

    print(f"\nCSV: {csv_path}")

if __name__ == "__main__":
    main()
