"""Print the DAQ HAT VDUT state and live measurement (read-only).

vdut() with no arguments only reports, and read() only measures: the output is
not switched on or changed. Without a DAQ HAT it says so.
"""
import daq

if not daq.present():
    print("no DAQ HAT connected")
else:
    state = daq.vdut()
    print("output %s, setpoint %.2f V, limit %.3f A, fault %s" % (
        "on" if state["enabled"] else "off", state["volts"], state["amps_limit"], state["fault"]))
    r = daq.read()
    print("measured %.3f V  %.1f mA  %.1f mW" % (r["v"], r["i"] * 1e3, r["p"] * 1e3))
