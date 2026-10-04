"""Sweep the DAQ VDUT supply and log V/I at each step.

Steps 1.8 V to 5.0 V in 0.2 V increments with a 200 mA limit, waits 300 ms
per step, prints one line per point and switches the output off at the end.
"""
import bugbuster
import daq

if not daq.present():
    raise OSError("no DAQ HAT")

try:
    v = 1.8
    while v <= 5.0001:
        daq.vdut(True, volts=v, amps_limit=0.2)
        bugbuster.sleep(300)
        r = daq.read()
        print("set=%.2f V  v=%.3f V  i=%.1f mA  p=%.1f mW" % (v, r["v"], r["i"] * 1e3, r["p"] * 1e3))
        v += 0.2
finally:
    daq.vdut(False)
