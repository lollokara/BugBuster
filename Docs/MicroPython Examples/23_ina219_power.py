import bb_devices
import bugbuster

i2c = bugbuster.I2C(1, 2, freq=400000, pullups='external', supply=3.3, vlogic=3.3)
sensor = bb_devices.INA219(i2c, shunt_ohms=0.1, max_expected_amps=2.0)
bus_v, shunt_v, current_a, power_w = sensor.read()
print("Bus: %.3f V" % bus_v)
print("Shunt: %.3f mV" % (shunt_v * 1000.0))
print("Current: %.3f A" % current_a)
print("Power: %.3f W" % power_w)
