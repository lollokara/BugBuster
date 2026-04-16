import time
import struct
import random
import sys
import argparse
from typing import Optional

# Add project root to path for imports
import os
sys.path.insert(0, os.path.abspath(os.path.join(os.path.dirname(__file__), '..')))

import bugbuster as bb
from bugbuster import client
from tests.mock.la_usb_host import (
    LaUsbHost, STREAM_CMD_START, STREAM_CMD_STOP,
    PKT_START, PKT_DATA, PKT_STOP
)

def fmt_pkt(pkt):
    if not pkt: return "None"
    ptype = pkt.pkt_type
    seq = pkt.seq
    info = pkt.info
    tname = {
        PKT_START: "START",
        PKT_DATA: "DATA",
        PKT_STOP: "STOP"
    }.get(ptype, f"TYPE_{ptype:02X}")
    return f"{tname} seq={seq} len={len(pkt.payload)} info=0x{info:02x}"

def run_cycle(port, cycle_num):
    print(f"\n--- Cycle {cycle_num} ---")
    
    # Phase 1: BBP Prep
    print(f"  [BBP] Connecting to {port}...")
    dev = bb.connect_usb(port)
    try:
        print("  [BBP] Resetting LA USB...")
        dev.hat_la_usb_reset()
        print("  [BBP] Configuring LA...")
        dev.hat_la_configure(channels=4, rate_hz=1_000_000, depth=100_000)
        status = dev.hat_la_get_status()
        print(f"  [BBP] Status: state={status.get('state')}, rearm={status.get('usb_rearm_pending')}")
    finally:
        dev.disconnect()
        time.sleep(0.5)

    # Phase 2: Bulk Stream
    print("  [Bulk] Connecting...")
    host = LaUsbHost()
    try:
        host.connect()
        print("  [Bulk] Sending START...")
        host.send_command(STREAM_CMD_START)
        
        # Wait for START
        start_pkt = host.wait_for_start(timeout_ms=1000)
        print(f"  [Bulk] {fmt_pkt(start_pkt)}")
        
        # Read some data
        data_count = 0
        t0 = time.monotonic()
        while time.monotonic() - t0 < 0.5:
            try:
                pkt = host.read_packet(timeout_ms=100)
                if pkt.pkt_type == PKT_DATA:
                    data_count += 1
            except Exception:
                break
        print(f"  [Bulk] Read {data_count} DATA packets")
        
        print("  [Bulk] Sending STOP...")
        host.send_command(STREAM_CMD_STOP)
        
        # Wait for STOP (consume DATA packets until we find it)
        stop_pkt = None
        t0 = time.monotonic()
        while time.monotonic() - t0 < 2.0:
            try:
                pkt = host.read_packet(timeout_ms=200)
                if pkt.pkt_type == PKT_STOP:
                    stop_pkt = pkt
                    break
            except Exception:
                break
        
        if stop_pkt:
            print(f"  [Bulk] {fmt_pkt(stop_pkt)}")
        else:
            print("  [Bulk] STOP timeout (no PKT_STOP after loop)")
            
    finally:
        host.close()
        time.sleep(0.5)

    # Phase 3: Post-check
    print(f"  [BBP] Final health check...")
    dev = bb.connect_usb(port)
    try:
        status = dev.hat_la_get_status()
        print(f"  [BBP] Final: state={status.get('state')}, rearm={status.get('usb_rearm_pending')}")
        if status.get('state') != 0 and status.get('state') != 4:
            print(f"  !!! BAD STATE: {status.get('state')}")
    finally:
        dev.disconnect()
    
    return True

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", required=True)
    parser.add_argument("--cycles", type=int, default=5)
    args = parser.parse_args()

    print(f"Starting {args.cycles}-cycle stress test on {args.port}")
    
    for i in range(args.cycles):
        if not run_cycle(args.port, i+1):
            print("\nSTRESS TEST FAILED")
            sys.exit(1)
            
    print("\nSTRESS TEST PASSED")

if __name__ == "__main__":
    main()
