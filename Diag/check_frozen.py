import sys
import time
import bugbuster.client as bbc

def check_port(port):
    print("Connecting to BBP...")
    try:
        with bbc.BugBusterClient(port) as dev:
            print("Connected! Pinging RP2040 status...")
            t0 = time.time()
            st = dev.hat_la_get_status(timeout_ms=500)
            print(f"Success in {time.time() - t0:.3f}s: {st}")
    except Exception as e:
        print(f"FAILED: {e}")

if __name__ == "__main__":
    check_port(sys.argv[1] if len(sys.argv)>1 else "/dev/cu.usbmodem1234561")
