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
PORT = "/dev/cu.usbmodem1101"
BAUD = 115200
STEPS_MHZ = range(1, 21)          # 1 MHz to 20 MHz
READS_PER_STEP = 20
SETTLE_AFTER_CLOCK = 0.3          # seconds to wait after clock change
TIMEOUT = 2.0                     # serial read timeout
# ----------------------------

def open_serial(port, baud):
    ser = serial.Serial(port, baud, timeout=TIMEOUT)
    time.sleep(0.5)
    ser.reset_input_buffer()
    return ser

def send_cmd(ser, cmd):
    """Send a CLI command and return all response lines."""
    ser.reset_input_buffer()
    ser.write((cmd + "\r\n").encode())
    time.sleep(0.05)
    lines = []
    deadline = time.time() + TIMEOUT
    while time.time() < deadline:
        raw = ser.readline()
        if not raw:
            break
        line = raw.decode(errors="replace").strip()
        if line:
            lines.append(line)
    return lines

def parse_spiclock_verify(lines):
    """Return (clock_ok: bool, read_back_hex: str) from spiclock response."""
    for line in lines:
        if "Verify:" in line:
            ok = "OK" in line
            m = re.search(r"read 0x([0-9A-Fa-f]+)", line)
            rb = m.group(1) if m else "????"
            return ok, rb
    return False, "NO_RESPONSE"

def parse_adc_readings(lines):
    """Extract per-channel (raw, voltage) from adc command output."""
    readings = []
    for line in lines:
        m = re.match(
            r"\s*(\d)\s*\|.*\|\s*\d\s*\|\s*(\d+)\s*\|\s*([+-]?\d+\.\d+)\s*V",
            line,
        )
        if m:
            ch = int(m.group(1))
            raw = int(m.group(2))
            voltage = float(m.group(3))
            readings.append((ch, raw, voltage))
    return readings

def main():
    port = sys.argv[1] if len(sys.argv) > 1 else PORT

    print(f"=== SPI Clock Sweep Test ===")
    print(f"Port: {port}  |  Steps: {STEPS_MHZ[0]}-{STEPS_MHZ[-1]} MHz  |  Reads/step: {READS_PER_STEP}")
    print()

    ser = open_serial(port, BAUD)

    # CSV log
    ts = datetime.now().strftime("%Y%m%d_%H%M%S")
    csv_path = f"spi_sweep_{ts}.csv"
    csvfile = open(csv_path, "w", newline="")
    writer = csv.writer(csvfile)
    writer.writerow(["clock_mhz", "clock_ok", "sample", "ch", "raw", "voltage"])

    results_summary = []

    for mhz in STEPS_MHZ:
        hz = mhz * 1_000_000
        print(f"--- {mhz} MHz ({hz} Hz) ---")

        # Set clock speed
        resp = send_cmd(ser, f"spiclock {hz}")
        clock_ok, rb = parse_spiclock_verify(resp)
        status = "OK" if clock_ok else f"FAIL (readback=0x{rb})"
        print(f"  Clock set: {status}")

        if not clock_ok:
            print(f"  ** SPI verify FAILED at {mhz} MHz — stopping sweep **")
            writer.writerow([mhz, "FAIL", "", "", "", ""])
            results_summary.append((mhz, "FAIL", 0, 0, []))
            # Try to recover to 1 MHz before exiting
            send_cmd(ser, "spiclock 1000000")
            break

        time.sleep(SETTLE_AFTER_CLOCK)

        # Take ADC readings
        ch_voltages = {0: [], 1: [], 2: [], 3: []}
        errors = 0
        for i in range(READS_PER_STEP):
            resp = send_cmd(ser, "adc")
            readings = parse_adc_readings(resp)
            if not readings:
                errors += 1
                continue
            for ch, raw, voltage in readings:
                ch_voltages[ch].append(voltage)
                writer.writerow([mhz, "OK", i + 1, ch, raw, f"{voltage:.6f}"])

        # Per-channel stats
        all_ok = True
        ch_stats = []
        for ch in range(4):
            vals = ch_voltages[ch]
            if not vals:
                ch_stats.append(f"CH{ch}: no data")
                all_ok = False
                continue
            avg = sum(vals) / len(vals)
            vmin = min(vals)
            vmax = max(vals)
            spread = vmax - vmin
            ch_stats.append(f"CH{ch}: avg={avg:+.4f}V  spread={spread:.4f}V")

        for s in ch_stats:
            print(f"  {s}")
        if errors:
            print(f"  ({errors}/{READS_PER_STEP} reads returned no data)")

        total_vals = sum(len(v) for v in ch_voltages.values())
        results_summary.append((mhz, "OK", total_vals, errors, ch_voltages))

    csvfile.close()

    # Restore to safe speed
    print(f"\nRestoring SPI clock to 1 MHz...")
    send_cmd(ser, "spiclock 1000000")
    ser.close()

    # Print summary table
    print(f"\n{'='*70}")
    print(f"{'MHz':>4} | {'Status':>6} | {'Samples':>7} | {'CH0 avg':>10} | {'CH1 avg':>10} | {'CH2 avg':>10} | {'CH3 avg':>10}")
    print(f"{'-'*4}-+-{'-'*6}-+-{'-'*7}-+-{'-'*10}-+-{'-'*10}-+-{'-'*10}-+-{'-'*10}")
    for mhz, status, total, errors, ch_v in results_summary:
        avgs = []
        for ch in range(4):
            vals = ch_v.get(ch, []) if isinstance(ch_v, dict) else []
            if vals:
                avgs.append(f"{sum(vals)/len(vals):+.4f}V")
            else:
                avgs.append("   ---   ")
        print(f"{mhz:>4} | {status:>6} | {total:>7} | {' | '.join(avgs)}")

    print(f"\nCSV saved to: {csv_path}")
    print("Done.")

if __name__ == "__main__":
    main()
