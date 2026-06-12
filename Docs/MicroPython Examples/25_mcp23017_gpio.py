import bb_devices
import bugbuster

i2c = bugbuster.I2C(1, 2, freq=400000, pullups='external', supply=3.3, vlogic=3.3)
gpio = bb_devices.MCP23017(i2c)

# pinMode(0, OUTPUT)
# pinMode(1, INPUT_PULLUP)
# pinMode(2, INPUT_PULLUP)
# pinMode(3, INPUT_PULLUP)
gpio.pin_mode(0, output=True)
gpio.pin_mode(1, output=False, pullup=True)
gpio.pin_mode(2, output=False, pullup=True)
gpio.pin_mode(3, output=False, pullup=True)

# digitalWrite(0, HIGH)
gpio.set_pin(0, 1)
print("GPIO0:", gpio.read_pin(0))
print("GPIO1:", gpio.read_pin(1))
print("GPIO state: 0x%04x" % gpio.read_port())
