"""Watch a load with an INA219 power monitor for ten seconds.

INA219 on IO1 (SDA) / IO2 (SCL) at 3.3 V, address 0x40, 0.1 ohm shunt, up to
1 A. Prints bus voltage, current and power each second.
"""
import bugbuster
import bb_devices

bus = bugbuster.I2C(1, 2, freq=100000, supply=3.3, vlogic=3.3)
try:
    ina = bb_devices.INA219(bus, addr=0x40, shunt_ohms=0.1, max_expected_amps=1.0)
    for _ in range(10):
        bus_v, shunt_v, amps, watts = ina.read()
        print("%.3f V  %.1f mA  %.1f mW" % (bus_v, amps * 1e3, watts * 1e3))
        bugbuster.sleep(1000)
finally:
    bus.close()
