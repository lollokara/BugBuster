#!/usr/bin/env python3
"""
flash_via_p4.py — Flash the ESP32-C6 via the P4 c6boot passthrough.

Workflow:
  1. Open the P4 REPL console on <port> at 115200.
  2. Send "c6boot" to enter ROM download mode + bridge UART2 to console.
  3. Wait for the "UART2 bridged" confirmation.
  4. Close the serial handle so esptool can claim the same port.
  5. Run esptool to flash the C6.

Usage:
  python flash_via_p4.py [port] [firmware.bin]

Defaults:
  port        = COM15
  firmware    = .pio/build/esp32c6/firmware.bin  (relative to this script)
"""

import sys
import time
import subprocess
import os
import serial

PORT     = sys.argv[1] if len(sys.argv) > 1 else "COM15"
FW_BIN   = sys.argv[2] if len(sys.argv) > 2 else os.path.join(
               os.path.dirname(__file__), ".pio", "build", "esp32c6", "firmware.bin")
ESPTOOL  = os.path.join(os.path.expanduser("~"), ".platformio", "penv",
                        "Scripts", "python.exe")
ESPTOOL_MOD = "-m esptool"

def log(msg):
    print(f"[flash_via_p4] {msg}", flush=True)

# ── Step 1: send c6boot ──────────────────────────────────────────────────────
log(f"Opening {PORT} at 115200 to send 'c6boot' to the P4 REPL ...")
try:
    ser = serial.Serial(PORT, baudrate=115200, timeout=2)
except serial.SerialException as e:
    sys.exit(f"Cannot open {PORT}: {e}")

# Drain any pending data, then send the command.
time.sleep(0.2)
ser.read(ser.in_waiting or 1)
ser.write(b"\r\nc6boot\r\n")
ser.flush()

# ── Step 2: wait for bridge-ready banner ────────────────────────────────────
log("Waiting for P4 to confirm passthrough (up to 10 s) ...")
deadline = time.time() + 10
buf = b""
ready = False
while time.time() < deadline:
    chunk = ser.read(ser.in_waiting or 1)
    if chunk:
        buf += chunk
        sys.stdout.buffer.write(chunk)
        sys.stdout.buffer.flush()
    if b"UART2 bridged" in buf or b"ROM download mode" in buf:
        ready = True
        break
    time.sleep(0.05)

ser.close()

if not ready:
    sys.exit("P4 did not confirm passthrough — is it running? Is c6boot available?")

# Small settle time so the P4 finishes installing the UART driver before
# esptool resets the line and starts slipping frames.
time.sleep(0.5)

# ── Step 3: run esptool ──────────────────────────────────────────────────────
log(f"Flashing {FW_BIN} -> C6 on {PORT} ...")
cmd = [
    ESPTOOL, "-m", "esptool",
    "--chip",  "esp32c6",
    "--port",  PORT,
    "--baud",  "460800",
    "write_flash", "0x0", FW_BIN,
]
log(f"Command: {' '.join(cmd)}")
result = subprocess.run(cmd)
sys.exit(result.returncode)
