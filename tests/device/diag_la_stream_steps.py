#!/usr/bin/env python3
"""
Diagnostic: trace LA stream lifecycle step-by-step to find where error 0x11 appears.
Run: .venv/bin/python3 tests/device/diag_la_stream_steps.py /dev/cu.usbmodem1234561
"""
import sys
import time
import struct

sys.path.insert(0, "python")
sys.path.insert(0, "tests")

import bugbuster as bb
from mock.la_usb_host import LaUsbHost, STREAM_CMD_START, STREAM_CMD_STOP, PKT_START, PKT_STOP, PKT_DATA, PKT_ERROR

PORT = sys.argv[1] if len(sys.argv) > 1 else "/dev/cu.usbmodem1234561"
step = 0

def check(label):
    global step
    step += 1
    try:
        dev = bb.connect_usb(PORT)
        status = dev.hat_la_get_status()
        dev.disconnect()
        rearm = f"req={status.get('usb_rearm_request_count')}, comp={status.get('usb_rearm_complete_count')}, pending={status.get('usb_rearm_pending')}"
        print(f"  [{step}] {label}: OK — state={status['state_name']}, {rearm}, stop_reason={status.get('stream_stop_reason_name')}")
        return True
    except Exception as e:
        print(f"  [{step}] {label}: *** FAIL *** — {type(e).__name__}: {e}")
        return False

def bbp_cmd(label, func, *args, **kwargs):
    global step
    step += 1
    try:
        dev = bb.connect_usb(PORT)
        result = func(dev, *args, **kwargs)
        dev.disconnect()
        print(f"  [{step}] {label}: OK — {result}")
        return True
    except Exception as e:
        print(f"  [{step}] {label}: *** FAIL *** — {type(e).__name__}: {e}")
        return False

print(f"\n=== LA Stream Diagnostic (port={PORT}) ===\n")

# Phase 1: Basic connectivity
print("--- Phase 1: Basic connectivity ---")
check("Initial status")

# Phase 2: Configure
print("\n--- Phase 2: Configure ---")
bbp_cmd("hat_la_stop()", lambda d: d.hat_la_stop())
check("After hat_la_stop")
bbp_cmd("hat_la_configure(4ch, 1MHz)", lambda d: d.hat_la_configure(channels=4, rate_hz=1_000_000, depth=100_000))
check("After configure")

# Phase 3: Stream cycle via vendor bulk
print("\n--- Phase 3: First stream cycle (vendor bulk) ---")
host = LaUsbHost()
host.connect()

step += 1
print(f"  [{step}] LaUsbHost connected")

host.reset_stream_buffer()
step += 1
print(f"  [{step}] reset_stream_buffer done")

host.send_command(STREAM_CMD_START)
step += 1
print(f"  [{step}] START command sent")

try:
    start_pkt = host.wait_for_start(timeout_ms=2000, max_packets=512)
    step += 1
    print(f"  [{step}] Got START packet: type={start_pkt.pkt_type}, seq={start_pkt.seq}")
except Exception as e:
    step += 1
    print(f"  [{step}] wait_for_start FAILED: {e}")
    host.close()
    sys.exit(1)

# Read a few data packets
data_count = 0
for _ in range(20):
    try:
        pkt = host.read_packet(timeout_ms=200)
        if pkt.pkt_type == PKT_DATA:
            data_count += 1
        elif pkt.pkt_type in (PKT_STOP, PKT_ERROR):
            step += 1
            print(f"  [{step}] Got terminal during read: type={pkt.pkt_type:#x}, info={pkt.info:#x}")
            break
    except Exception:
        break

step += 1
print(f"  [{step}] Read {data_count} DATA packets")

check("Status during stream")

# Send STOP via vendor bulk
host.send_command(STREAM_CMD_STOP)
step += 1
print(f"  [{step}] STOP command sent (vendor bulk)")

# Read until terminal
terminal = None
extra_data = 0
for _ in range(500):
    try:
        pkt = host.read_packet(timeout_ms=500)
        if pkt.pkt_type == PKT_STOP:
            terminal = pkt
            step += 1
            print(f"  [{step}] Got PKT_STOP: seq={pkt.seq}, info={pkt.info:#x}")
            break
        elif pkt.pkt_type == PKT_ERROR:
            terminal = pkt
            step += 1
            print(f"  [{step}] Got PKT_ERROR: seq={pkt.seq}, info={pkt.info:#x}")
            break
        elif pkt.pkt_type == PKT_DATA:
            extra_data += 1
    except Exception as e:
        step += 1
        print(f"  [{step}] Read timeout waiting for terminal: {e}")
        break

if terminal is None:
    step += 1
    print(f"  [{step}] *** NO TERMINAL PACKET *** (read {extra_data} extra DATA packets)")

check("After vendor-bulk STOP")

# Phase 4: BBP stop (the preflight path)
print("\n--- Phase 4: BBP STOP (preflight) ---")
ok = bbp_cmd("hat_la_stop() #1", lambda d: d.hat_la_stop())
if ok:
    check("After BBP stop #1")
else:
    print("  *** BBP STOP #1 failed — this is where 0x11 appears ***")
    check("Status after BBP stop #1 failure")

# Phase 5: Second cycle
print("\n--- Phase 5: Second configure + stream ---")
ok = bbp_cmd("hat_la_configure(4ch, 1MHz) #2", lambda d: d.hat_la_configure(channels=4, rate_hz=1_000_000, depth=100_000))
if not ok:
    check("After configure #2 failure")
    host.close()
    sys.exit(1)

check("After configure #2")

host.reset_stream_buffer()
host.send_command(STREAM_CMD_START)
step += 1
print(f"  [{step}] START #2 sent")

try:
    start2 = host.wait_for_start(timeout_ms=2000, max_packets=512)
    step += 1
    print(f"  [{step}] Got START #2: type={start2.pkt_type}, seq={start2.seq}")
except Exception as e:
    step += 1
    print(f"  [{step}] wait_for_start #2 FAILED: {e}")

# Quick stop
host.send_command(STREAM_CMD_STOP)
for _ in range(100):
    try:
        pkt = host.read_packet(timeout_ms=500)
        if pkt.pkt_type in (PKT_STOP, PKT_ERROR):
            step += 1
            print(f"  [{step}] Terminal #2: type={pkt.pkt_type:#x}, info={pkt.info:#x}")
            break
    except:
        break

check("After second cycle")

host.close()
print(f"\n=== Done ({step} steps) ===")
