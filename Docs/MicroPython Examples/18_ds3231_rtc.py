import bb_devices
import bugbuster

i2c = bugbuster.I2C(1, 2, freq=400000, pullups='external', supply=3.3, vlogic=3.3)
rtc = bb_devices.DS3231(i2c)

print("Status: 0x%02x" % rtc.status())
print("Datetime:", rtc.read_datetime())
print("Temperature: %.2f C" % rtc.temperature_c())
