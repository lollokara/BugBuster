import usb.core
import usb.util
import time
import sys

# Add project root to path for imports
import os
sys.path.insert(0, os.path.abspath(os.path.join(os.path.dirname(__file__), '..')))

import bugbuster as bb

# USB identifiers
VID = 0x2E8A
PID = 0x000C
LA_INTERFACE = 3
EP_IN = 0x87   # bulk IN (device → host)
EP_OUT = 0x06  # bulk OUT (host → device)

# Commands
STREAM_CMD_STOP  = 0x00
STREAM_CMD_START = 0x01

def test_full_flow(port):
    print(f"--- Phase 1: BBP Configuration on {port} ---")
    dev = bb.connect_usb(port)
    try:
        dev.hat_la_usb_reset()
        rate_hz = 100_000 
        print(f"  Configuring LA at {rate_hz} Hz...")
        dev.hat_la_configure(channels=4, rate_hz=rate_hz, depth=100_000)
        status = dev.hat_la_get_status()
        print(f"  Status: {status}")
    finally:
        dev.disconnect()
        time.sleep(0.5)

    print("\n--- Phase 2: Claim Bulk Interface ---")
    udev = usb.core.find(idVendor=VID, idProduct=PID)
    if udev is None:
        print("Device not found!")
        return

    try:
        if udev.is_kernel_driver_active(LA_INTERFACE):
            udev.detach_kernel_driver(LA_INTERFACE)
        usb.util.claim_interface(udev, LA_INTERFACE)
    except Exception as e:
        print(f"Failed to claim interface: {e}")
        return

    try:
        # 1. Start Stream
        print("\n--- Phase 3: Start Stream ---")
        udev.write(EP_OUT, [STREAM_CMD_START], timeout=1000)
        
        print("  Waiting for data...")
        packets = 0
        t0 = time.monotonic()
        while packets < 10 and time.monotonic() - t0 < 2.0:
            try:
                raw = udev.read(EP_IN, 64, timeout=200)
                if len(raw) > 0:
                    data = bytes(raw)
                    print(f"  [{packets}] {data.hex()[:16]}...")
                    packets += 1
            except Exception as e:
                print(f"  Read error: {e}")
                break

        if packets == 0:
            print("  FAILED to get any data!")
            return

        # NEW: Stop reading and wait for buffers to settle
        print(f"  Received {packets} packets. Pausing reads for 0.1s...")
        time.sleep(0.1)

        # 2. Stop Stream
        print("\n--- Phase 4: Send STOP command ---")
        udev.write(EP_OUT, [STREAM_CMD_STOP], timeout=1000)
        print("  STOP sent. Resuming reads to drain...")

        # 3. Drain and find PKT_STOP
        t0 = time.monotonic()
        found_stop = False
        drained_packets = 0
        while time.monotonic() - t0 < 3.0:
            try:
                raw = udev.read(EP_IN, 64, timeout=500)
                if len(raw) > 0:
                    data = bytes(raw)
                    if data[0] == 0x03: # PKT_STOP
                        print(f"  GOT PKT_STOP: {data.hex()} (after {drained_packets} data packets)")
                        found_stop = True
                        break
                    drained_packets += 1
                    if drained_packets % 100 == 0:
                        print(f"  Drained {drained_packets} packets...")
            except usb.core.USBTimeoutError:
                print("  Read timeout during drain.")
                break
            except Exception as e:
                print(f"  Read error: {e}")
                break
        
        if not found_stop:
            print(f"  FAILED to receive PKT_STOP (drained {drained_packets} packets)")

        # Post-drain: check RP2040 status via BBP to see if deferred stop fired
        print("\n--- Phase 5: Post-drain status check ---")
        try:
            usb.util.release_interface(udev, LA_INTERFACE)
            time.sleep(1.0)  # let deferred stop complete
            dev2 = bb.connect_usb(port)
            status = dev2.hat_la_get_status()
            print(f"  state={status.get('state')} ({status.get('state_name')})")
            print(f"  stop_reason={status.get('stream_stop_reason')} ({status.get('stream_stop_reason_name')})")
            print(f"  rearm_pending={status.get('usb_rearm_pending')}")
            print(f"  rearm_req={status.get('usb_rearm_request_count')} complete={status.get('usb_rearm_complete_count')}")
            print(f"  overrun={status.get('stream_overrun_count')} short_write={status.get('stream_short_write_count')}")
            dev2.disconnect()
        except Exception as e:
            print(f"  Status check FAILED: {e}")
        return

    finally:
        try:
            usb.util.release_interface(udev, LA_INTERFACE)
        except Exception:
            pass
        print("Done.")

if __name__ == "__main__":
    import argparse
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", required=True)
    args = parser.parse_args()
    test_full_flow(args.port)
