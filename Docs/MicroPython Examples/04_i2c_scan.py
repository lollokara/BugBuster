# 04_i2c_scan.py — Scan external I2C bus on IO2 (SDA) / IO3 (SCL).
#
# Purpose: discover connected I2C devices.
#
# Wiring: target's SDA→IO2, SCL→IO3, GND common. Target VDD on a separate
#   3.3 V supply (or wire to BugBuster's VADJ line, which is auto-configured
#   by the firmware). External 4.7 kΩ pull-ups on SDA/SCL recommended
#   (firmware can provide internal pull-ups, but external is preferred).
#
# Run: POST /api/scripts/eval or upload + run-file. Useful transports: eval.
# Expect: list of 7-bit hex addresses (0x00–0x7F) of responding I2C slaves.
#   If no devices are wired, the list will be empty (expected).

import bugbuster

print("Scanning I2C bus on IO2 (SDA) / IO3 (SCL)...")

try:
    i2c = bugbuster.I2C(sda_io=2, scl_io=3, freq=100000, pullups='external')
    addrs = i2c.scan(start=0x08, stop=0x77, skip_reserved=True, timeout_ms=50)

    if addrs:
        print('Found %d device(s):' % len(addrs))
        for addr in addrs:
            print('  0x%02x' % addr)
    else:
        print('No devices found.')

except ValueError as e:
    print('Setup error: %s' % e)
except OSError as e:
    print('Scan error: %s' % e)
