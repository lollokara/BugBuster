import sys, os
_REPO_ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
if _REPO_ROOT not in sys.path:
    sys.path.insert(0, _REPO_ROOT)
from python.bugbuster import connect_usb
try:
    import glob
    candidates = glob.glob("/dev/tty.usbmodem*") + glob.glob("/dev/ttyACM*")
    port = candidates[0]
    print(f"Connecting to {port}")
    bb = connect_usb(port)
    val = bb.get_adc_value(3)
    print("ADC val:", val)
    print("Test passed")
except Exception as e:
    print("Error:", e)
