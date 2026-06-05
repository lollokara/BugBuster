import sys, os, time, threading
_REPO_ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
if _REPO_ROOT not in sys.path:
    sys.path.insert(0, _REPO_ROOT)
from python.bugbuster import connect_http
from python.bugbuster.discovery import discover_mdns
from Scripts.MeasureCurrent import Measurement, IO_BLOCK_MAP

host = discover_mdns(timeout=2.0)[0].ip
token = "5ca9c6b47be8e96d70b35ee28e0bb12728094b6c0a149ffb48a01d8dae336969"
bb = connect_http(host, admin_token=token)

# Try polling for 2 seconds
meas = Measurement()
t = threading.Thread(target=meas.poll, args=(bb, 3, 0.1, 12.0), daemon=True)
t.start()
time.sleep(2)
meas.running = False
t.join()

print(f"Collected {len(meas.timestamps)} samples in Poll mode over Wi-Fi.")
