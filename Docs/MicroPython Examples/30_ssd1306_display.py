import bb_devices
import bugbuster

i2c = bugbuster.I2C(1, 2, freq=400000, pullups='external', supply=3.3, vlogic=3.3)
display = bb_devices.SSD1306(128, 64, i2c)
display.fill(0)
display.text("BugBuster", 0, 0, 1)
display.text("MicroPython", 0, 12, 1)
display.text("SSD1306", 0, 24, 1)
display.show()
