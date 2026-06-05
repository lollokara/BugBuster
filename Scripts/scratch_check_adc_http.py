import sys, os, time
_REPO_ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
if _REPO_ROOT not in sys.path:
    sys.path.insert(0, _REPO_ROOT)
from python.bugbuster import connect_http
from python.bugbuster.discovery import discover_mdns

try:
    devices = discover_mdns(timeout=2.0)
    host = devices[0].ip if devices else "192.168.4.1"
    print(f"Connecting to {host}")
    token = "5ca9c6b47be8e96d70b35ee28e0bb12728094b6c0a149ffb48a01d8dae336969"
    bb = connect_http(host, admin_token=token)
    
    # Try getting ADC value for channel 3 (D)
    for i in range(5):
        val = bb.get_adc_value(3)
        print(f"ADC D (ch3) raw: {val.raw}, value: {val.value}")
        time.sleep(0.5)
        
    print("Test passed")
except Exception as e:
    import traceback
    traceback.print_exc()
