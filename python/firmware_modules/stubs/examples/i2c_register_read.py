"""Read one register from an I2C device on IO1 (SDA) / IO2 (SCL).

Reads the chip-id register (0xD0) of a BME280/BMP280 at 0x76 at 3.3 V. Change
ADDR and REG for your part. The bus is closed even if the read fails.
"""
import bugbuster

ADDR = 0x76
REG = 0xD0

bus = bugbuster.I2C(1, 2, freq=100000, supply=3.3, vlogic=3.3)
try:
    if ADDR not in bus.scan():
        print("no device at 0x%02X" % ADDR)
    else:
        value = bus.writeto_then_readfrom(ADDR, bytes([REG]), 1)[0]
        print("reg 0x%02X = 0x%02X" % (REG, value))
finally:
    bus.close()
