import bb_devices
import bugbuster

i2c = bugbuster.I2C(1, 2, freq=100000, pullups='external', supply=3.3, vlogic=3.3)
dac = bb_devices.MCP4725(i2c, addr=0x62, vref=3.3)

for volts in (0.0, 0.5, 1.0, 1.5, 2.0, 2.5, 3.0, 3.3):
    dac.set_voltage(volts)
    print("Set to %.2f V" % volts)
    bugbuster.sleep(250)
