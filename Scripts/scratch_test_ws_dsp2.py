import sys, os, time
_REPO_ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
if _REPO_ROOT not in sys.path:
    sys.path.insert(0, _REPO_ROOT)
from python.bugbuster import connect_http
from python.bugbuster.discovery import discover_mdns
from python.bugbuster.client import AdcRate, AdcMux, AdcRange
from python.bugbuster.hal import PortMode

host = discover_mdns(timeout=2.0)[0].ip
token = "5ca9c6b47be8e96d70b35ee28e0bb12728094b6c0a149ffb48a01d8dae336969"
bb = connect_http(host, admin_token=token)

print("Configuring HAL for channel D (analog_io=12, ch=3)")
hal = bb.hal
hal.begin()
hal.configure(12, PortMode.ANALOG_IN)
bb.set_adc_config(3, AdcMux.LF_TO_AGND, AdcRange.V_0_12, AdcRate.SPS_9600)
time.sleep(0.5)

events = []
def on_dsp(win):
    events.append(win)

print("Starting DSP stream")
bb.start_adc_dsp_stream(3, rate=AdcRate.SPS_9600, callback=on_dsp)

print("Started stream, waiting 3 seconds...")
time.sleep(3.0)

bb.stop_adc_dsp_stream()
hal.shutdown()
print(f"Received {len(events)} events.")
if events:
    print("First event mean_v:", events[0].mean_v)
