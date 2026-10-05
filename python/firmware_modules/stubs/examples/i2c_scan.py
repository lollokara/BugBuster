"""Scan an I2C bus on IO1 (SDA) / IO2 (SCL) at 3.3 V and print the addresses."""
import bugbuster

bus = bugbuster.I2C(1, 2, freq=100000, pullups="internal", supply=3.3, vlogic=3.3)
try:
    print(["0x%02X" % a for a in bus.scan()])
finally:
    bus.close()
