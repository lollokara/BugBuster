# 06_spi_flash_jedec_id.py — Read JEDEC ID from SPI flash chip.
#
# Purpose: demonstrate SPI full-duplex transfer and manufacturer ID lookup.
#
# Wiring: SPI flash (e.g., Winbond W25Q32) with:
#   CS→IO7, CLK→IO4, MOSI→IO5, MISO→IO6, GND common, VCC to 3.3 V supply.
#
# Run: POST /api/scripts/eval or upload + run-file.
# Expect: send JEDEC ID opcode 0x9F, receive 3-byte manufacturer+memory-type+capacity,
#   lookup manufacturer in a table, and print human-readable ID. Without hardware,
#   you'll see 0xFF or error (expected).

import bugbuster

# Known JEDEC manufacturer codes
JEDEC_VENDORS = {
    0xEF: 'Winbond',
    0xC2: 'Macronix',
    0x01: 'Spansion',
    0xAD: 'SK Hynix',
    0x20: 'Micron',
}

print("Reading JEDEC ID from SPI flash on IO4/IO5/IO6/IO7...")

try:
    spi = bugbuster.SPI(sck_io=4, mosi_io=5, miso_io=6, cs_io=7,
                        freq=1_000_000, mode=0)

    # JEDEC ID command: 0x9F followed by 3 dummy bytes
    rx = spi.transfer(b'\x9F\x00\x00\x00')

    mfg_id = rx[1]
    mem_type = rx[2]
    capacity = rx[3]

    vendor_name = JEDEC_VENDORS.get(mfg_id, 'Unknown')

    print('Raw bytes: %s' % rx.hex())
    print('Manufacturer: 0x%02x (%s)' % (mfg_id, vendor_name))
    print('Memory type: 0x%02x' % mem_type)
    print('Capacity: 0x%02x' % capacity)

except ValueError as e:
    print('Setup error: %s' % e)
except OSError as e:
    print('SPI error: %s' % e)
