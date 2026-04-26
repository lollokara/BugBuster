# 09_concurrent_devices.py — Demonstrate I2C and SPI in the same script.
#
# Purpose: show that multiple buses can be set up and used concurrently.
#   The firmware's bus_planner handles MUX state, e-fuse routing, and
#   level-shifter control automatically so there's no conflict.
#
# Wiring:
#   I2C: SDA→IO2, SCL→IO3 (with optional I2C device at 0x48)
#   SPI: CS→IO7, CLK→IO4, MOSI→IO5, MISO→IO6 (with optional SPI flash)
#   GND common to both buses and to BugBuster.
#
# Run: POST /api/scripts/eval or upload + run-file.
# Expect: scan I2C, attempt a SPI JEDEC read, print results from both.
#   If devices are not wired, you'll see empty I2C list and 0xFF (SPI),
#   both expected (no errors raised).

import bugbuster

print('Setting up I2C and SPI concurrently...')

# I2C on IO2/IO3
try:
    i2c = bugbuster.I2C(sda_io=2, scl_io=3, freq=100000, pullups='external')
    i2c_ok = True
except (ValueError, OSError) as e:
    print('I2C setup error: %s' % e)
    i2c_ok = False

# SPI on IO4/IO5/IO6/IO7
try:
    spi = bugbuster.SPI(sck_io=4, mosi_io=5, miso_io=6, cs_io=7, freq=1_000_000, mode=0)
    spi_ok = True
except (ValueError, OSError) as e:
    print('SPI setup error: %s' % e)
    spi_ok = False

# I2C scan
if i2c_ok:
    print('Scanning I2C...')
    try:
        addrs = i2c.scan(start=0x08, stop=0x77, skip_reserved=True, timeout_ms=50)
        if addrs:
            print('  Found devices: %s' % [hex(a) for a in addrs])
        else:
            print('  No I2C devices found (expected if nothing is wired).')
    except OSError as e:
        print('  Scan error: %s' % e)

# SPI JEDEC read
if spi_ok:
    print('Reading JEDEC ID from SPI...')
    try:
        rx = spi.transfer(b'\x9F\x00\x00\x00')
        mfg_id = rx[1]
        print('  JEDEC manufacturer: 0x%02x' % mfg_id)
        if mfg_id == 0xFF:
            print('  (0xFF suggests no SPI device or mismatch)')
    except OSError as e:
        print('  Transfer error: %s' % e)

print('Done. Both buses ran without contention.')
