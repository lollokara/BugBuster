import sys
import os
import time
import logging

# Ensure bugbuster is in the path
sys.path.append(os.path.join(os.getcwd(), "python"))

from bugbuster import connect_usb
from bugbuster.transport.usb import DeviceError

logging.basicConfig(level=logging.DEBUG)
logger = logging.getLogger("hat_verify")

def verify_hat_control():
    port = "/dev/cu.usbmodem1234561"
    print(f"Connecting to {port}...")
    try:
        dev = connect_usb(port=port)
    except Exception as e:
        print(f"Connection failed: {e}")
        return
    
    try:
        # 1. Ping
        print("\n--- Test 1: BBP Ping ---")
        dev.ping()
        print("BBP Ping OK")
        
        # 2. Get HAT Status (BBP 0xC5)
        print("\n--- Test 2: HAT Get Status ---")
        try:
            status = dev.hat_get_status()
            print(f"HAT Status: detected={status['detected']}, connected={status['connected']}, type={status['type']}")
        except Exception as e:
            print(f"HAT Status failed: {e}")

        # 3. LA Stop (BBP 0xD9) - ensure we can talk to the LA
        print("\n--- Test 3: LA Stop (Preflight) ---")
        try:
            dev.hat_la_stop()
            print("LA Stop OK")
        except Exception as e:
            print(f"LA Stop failed: {e}")

        # 4. LA Configure (BBP 0xD3) - 500ms timeout expected
        print("\n--- Test 4: LA Configure ---")
        try:
            dev.hat_la_configure(channels=1, rate_hz=1000000, depth=1024)
            print("LA Configure OK")
        except Exception as e:
            print(f"LA Configure failed: {e}")

        # 5. LA Get Status (BBP 0xD7) - Full 28-byte parsing check
        print("\n--- Test 5: LA Get Status (Full Parsing) ---")
        try:
            status = dev.hat_la_get_status()
            print(f"LA Status: state={status['state']}, samples={status['samples_captured']}/{status['total_samples']}")
            print(f"Stream Diagnostics: stop_reason={status.get('stream_stop_reason', 'N/A')}, overrun={status.get('stream_overrun_count', 'N/A')}")
            print(f"Rearm Counters: req={status.get('usb_rearm_request_count', 'N/A')}, complete={status.get('usb_rearm_complete_count', 'N/A')}")
            
            if status['total_samples'] == 1024:
                print("AC3 Pass: 28-byte parsing appears correct (captured total_samples field)")
            else:
                print(f"AC3 Fail: total_samples mismatch (expected 1024, got {status['total_samples']})")
                
        except Exception as e:
            print(f"LA Get Status failed: {e}")

        # 6. Stress test (AC5) - 10Hz for 10 seconds
        print("\n--- Test 6: Stress test 10Hz status polling ---")
        success_count = 0
        iterations = 100
        start_time = time.time()
        for i in range(iterations):
            try:
                dev.hat_la_get_status()
                success_count += 1
            except Exception:
                pass
            time.sleep(0.1)
        
        elapsed = time.time() - start_time
        print(f"Stress test complete: {success_count}/{iterations} successful in {elapsed:.2f}s")
        if success_count == iterations:
            print("AC5 Pass: 100% success rate")
        else:
            print(f"AC5 Fail: {iterations - success_count} timeouts observed")

    finally:
        dev.disconnect()

if __name__ == "__main__":
    verify_hat_control()
