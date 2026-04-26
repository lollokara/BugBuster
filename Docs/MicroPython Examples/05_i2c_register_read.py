# 05_i2c_register_read.py — Read temperature from I2C sensor (TMP102 pattern).
#
# Purpose: demonstrate register-read pattern (write address, then read data).
#
# Wiring: temperature sensor with I2C at 0x48 (e.g., TMP102, ADS1115)
#   wired to IO2 (SDA) / IO3 (SCL). For testing with a sensor that measures
#   room temperature, room temp should be 15–30 °C = 0x0F00–0x1E00 (12-bit).
#
# Run: POST /api/scripts/eval or upload + run-file.
# Expect: if a TMP102 is present at 0x48, read two bytes from register 0x00,
#   decode as signed 12-bit temperature, and print in Celsius. Each LSB is
#   0.0625 °C. Without hardware, you'll see a communication error (expected).

import bugbuster

print("Reading temperature from TMP102-like sensor at 0x48...")

try:
    i2c = bugbuster.I2C(sda_io=2, scl_io=3, freq=100000, pullups='external')

    # Read two bytes from register 0x00 (temperature register)
    data = i2c.writeto_then_readfrom(0x48, b'\x00', 2)

    # Unpack as big-endian 16-bit signed, extract upper 12 bits, convert to Celsius
    raw = (data[0] << 8) | data[1]
    temp_raw_12bit = (raw >> 4) & 0xFFF
    if temp_raw_12bit & 0x800:
        temp_raw_12bit = temp_raw_12bit - 0x1000  # Sign extend
    temp_c = temp_raw_12bit * 0.0625

    print('Raw bytes: 0x%02x 0x%02x' % (data[0], data[1]))
    print('Temperature: %.2f C' % temp_c)

except ValueError as e:
    print('Setup error: %s' % e)
except OSError as e:
    print('I2C error: %s' % e)
