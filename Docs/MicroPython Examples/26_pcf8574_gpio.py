import bb_devices
import bugbuster

i2c = bugbuster.I2C(1, 2, freq=400000, pullups='external', supply=3.3, vlogic=3.3)
gpio = bb_devices.PCF8574(i2c, addr=0x20)

# digitalWrite(0, LOW)
# digitalWrite(1, HIGH) via the PCF8574's weak pull-up behavior.
gpio.write(0b11111110)
print("P0:", gpio.read_pin(0))
print("P1:", gpio.read_pin(1))
print("GPIO state: 0x%02x" % gpio.read())
