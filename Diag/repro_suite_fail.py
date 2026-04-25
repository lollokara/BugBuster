#!/usr/bin/env python3
import sys
import time
import argparse
import usb.core
import usb.util

# Add project paths
sys.path.insert(0, "python")
sys.path.insert(0, "tests")

import bugbuster as bb
from tests.mock.la_usb_host import (
    LaUsbHost,
    STREAM_CMD_START,
    PKT_DATA,
    LA_INTERFACE,
)

def repro(port, cycles=5):
    print(f"Starting reproduction on {port} for {cycles} cycles...")
    
    for i in range(cycles):
        print(f"\n--- Cycle {i+1} ---")
        
        # Phase 1: BBP Prep (mimic _stop_preflight_and_configure)
        print("  [BBP] Connecting...")
        dev = bb.connect_usb(port)
        try:
            print("  [BBP] hat_la_usb_reset()...")
            dev.hat_la_usb_reset()
            time.sleep(0.1)
            
            print("  [BBP] hat_la_configure()...")
            dev.hat_la_configure(channels=4, rate_hz=1_000_000, depth=100_000)
            
            status = dev.hat_la_get_status()
            print(f"  [BBP] Status: {status}")
        except Exception as e:
            print(f"  [BBP] ERROR: {e}")
            sys.exit(1)
        finally:
            dev.disconnect()
            print("  [BBP] Disconnected.")

        # Phase 2: USB Start (mimic la_host fixture + test)
        host = LaUsbHost()
        try:
            print("  [USB] Connecting...")
            host.connect()
            
            print("  [USB] Sending START...")
            host.send_command(STREAM_CMD_START)
            
            print("  [USB] Waiting for START pkt...")
            start_pkt = host.wait_for_start(timeout_ms=2000)
            print(f"  [USB] Got START: seq={start_pkt.seq}")
            
            print("  [USB] Reading some DATA...")
            for _ in range(10):
                pkt = host.read_packet(timeout_ms=500)
                if pkt.pkt_type != PKT_DATA:
                    print(f"  [USB] Unexpected pkt: {pkt.pkt_type}")
            
            # Phase 3: STOP (mimic _stop_stream_via_bbp)
            print("  [STOP] Releasing USB interface...")
            usb.util.release_interface(host._dev, LA_INTERFACE)
            host._claimed = False
            
            print("  [STOP] hat_la_stop() via BBP...")
            dev = bb.connect_usb(port)
            try:
                dev.hat_la_stop()
                print("  [STOP] hat_la_stop() OK")
            finally:
                dev.disconnect()
            
            print("  [STOP] Waiting 0.5s rearm...")
            time.sleep(0.5)
            
        except Exception as e:
            print(f"  [USB] ERROR: {e}")
            sys.exit(1)
        finally:
            host.close()
            print("  [USB] Closed.")

    print("\nSUCCESS: All cycles completed.")

if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", required=True)
    parser.add_argument("--cycles", type=int, default=5)
    args = parser.parse_args()
    repro(args.port, args.cycles)
