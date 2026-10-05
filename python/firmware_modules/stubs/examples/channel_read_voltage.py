"""Read channel 0 as a voltage input ten times and print the average.

The channel is switched to voltage-input (high impedance, nothing is driven)
and back to high impedance at the end.
"""
import bugbuster

ch = bugbuster.Channel(0)
ch.set_function(bugbuster.FUNC_VIN)
try:
    total = 0.0
    for _ in range(10):
        bugbuster.sleep(100)
        volts = ch.read_voltage()
        total += volts
        print("%.4f V" % volts)
    print("average: %.4f V" % (total / 10))
finally:
    ch.set_function(bugbuster.FUNC_HIGH_IMP)
