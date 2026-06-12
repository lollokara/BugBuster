import bb_devices
import bugbuster

i2c = bugbuster.I2C(1, 2, freq=400000, pullups='external', supply=3.3, vlogic=3.3)
sensor = bb_devices.AHT20(i2c)
temp_c, humidity = sensor.read()
print("Temp: %.2f C" % temp_c)
print("Humidity: %.1f %%" % humidity)
