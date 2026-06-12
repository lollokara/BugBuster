import bb_devices
import bugbuster

i2c = bugbuster.I2C(1, 2, freq=400000, pullups='external', supply=3.3, vlogic=3.3)
light = bb_devices.BH1750(i2c)
print("Lux: %.1f" % light.read_lux())
