"""Read a SPI flash's JEDEC id (command 0x9F).

Pins: SCK IO1, MOSI IO2, MISO IO3, CS IO4, mode 0, 3.3 V. Expect three bytes:
manufacturer, memory type, capacity.
"""
import bugbuster

spi = bugbuster.SPI(1, mosi_io=2, miso_io=3, cs_io=4, freq=1000000, mode=0, supply=3.3, vlogic=3.3)
try:
    reply = spi.transfer(bytes([0x9F, 0x00, 0x00, 0x00]))
    print("JEDEC id: %02X %02X %02X" % (reply[1], reply[2], reply[3]))
finally:
    spi.close()
