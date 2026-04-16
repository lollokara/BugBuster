import usb.core
import usb.util
import time
import sys

# USB identifiers
VID = 0x2E8A
PID = 0x000C
LA_INTERFACE = 3
EP_IN = 0x87   # bulk IN (device → host)
EP_OUT = 0x06  # bulk OUT (host → device)

# Commands
STREAM_CMD_STOP  = 0x00
STREAM_CMD_START = 0x01

def test_raw_usb():
    print(f"Searching for device {VID:#06x}:{PID:#06x}...")
    dev = usb.core.find(idVendor=VID, idProduct=PID)
    if dev is None:
        print("Device not found!")
        return

    try:
        if dev.is_kernel_driver_active(LA_INTERFACE):
            print(f"Detaching kernel driver for interface {LA_INTERFACE}...")
            dev.detach_kernel_driver(LA_INTERFACE)
        
        print(f"Claiming interface {LA_INTERFACE}...")
        usb.util.claim_interface(dev, LA_INTERFACE)
    except Exception as e:
        print(f"Failed to claim interface: {e}")
        return

    try:
        # 1. Test raw read (flush any stale data)
        print("\n--- Phase 1: Initial Drain ---")
        try:
            raw = dev.read(EP_IN, 16384, timeout=500)
            print(f"  Read {len(raw)} bytes of stale data")
        except usb.core.USBTimeoutError:
            print("  No stale data on EP_IN")
        except Exception as e:
            print(f"  Read error: {e}")

        # 2. Test raw write (START command)
        print("\n--- Phase 2: Send START command ---")
        try:
            sent = dev.write(EP_OUT, [STREAM_CMD_START], timeout=1000)
            print(f"  Wrote {sent} bytes to EP_OUT (START)")
        except Exception as e:
            print(f"  Write error: {e}")

        # 3. Wait for START packet and some data
        print("\n--- Phase 3: Read Stream ---")
        t0 = time.monotonic()
        total_bytes = 0
        packets = 0
        while time.monotonic() - t0 < 1.0:
            try:
                raw = dev.read(EP_IN, 64, timeout=100)
                if len(raw) > 0:
                    data = bytes(raw)
                    if packets == 0:
                        print(f"  First packet: {data.hex()}")
                    packets += 1
                    total_bytes += len(raw)
                    if packets % 200 == 0:
                        print(f"  Received {packets} packets ({total_bytes} bytes)...")
            except usb.core.USBTimeoutError:
                if packets > 0:
                    print("  Stream paused (timeout)")
                else:
                    print("  Never received any data")
                break
            except Exception as e:
                print(f"  Read error: {e}")
                break
        
        print(f"  Total: {packets} packets, {total_bytes} bytes")

        # 4. Test STOP command
        print("\n--- Phase 4: Send STOP command ---")
        try:
            sent = dev.write(EP_OUT, [STREAM_CMD_STOP], timeout=1000)
            print(f"  Wrote {sent} bytes to EP_OUT (STOP)")
        except Exception as e:
            print(f"  Write error: {e}")

        # 5. Wait for PKT_STOP
        print("\n--- Phase 5: Drain and look for PKT_STOP ---")
        t0 = time.monotonic()
        found_stop = False
        drained_packets = 0
        while time.monotonic() - t0 < 3.0:
            try:
                raw = dev.read(EP_IN, 64, timeout=100)
                if len(raw) > 0:
                    data = bytes(raw)
                    if data[0] == 0x03: # PKT_STOP
                        print(f"  GOT PKT_STOP: {data.hex()}")
                        found_stop = True
                        break
                    drained_packets += 1
                    if drained_packets % 200 == 0:
                        print(f"  Draining... {drained_packets} packets")
            except usb.core.USBTimeoutError:
                print("  Timeout waiting for PKT_STOP")
                break
            except Exception as e:
                print(f"  Read error: {e}")
                break
        
        if not found_stop:
            print(f"  FAILED to receive PKT_STOP (drained {drained_packets} packets)")

    finally:
        print("\nReleasing interface...")
        usb.util.release_interface(dev, LA_INTERFACE)
        print("Done.")

if __name__ == "__main__":
    test_raw_usb()
