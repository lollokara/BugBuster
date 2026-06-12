import bb_devices
import bugbuster

i2c = bugbuster.I2C(1, 2, freq=400000, pullups='external', supply=3.3, vlogic=3.3)
sensor = bb_devices.TCS34725(i2c)
red, green, blue, clear = sensor.read_rgbc()
print("R=%d G=%d B=%d C=%d" % (red, green, blue, clear))
