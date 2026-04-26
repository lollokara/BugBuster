# bb_devices.py — BugBuster common device drivers (frozen module)
#
# Minimal drivers for popular I2C/SPI sensors using the bugbuster module.
# Import with: import bb_devices
#
# These run on-device without VFS — compiled as frozen .mpy bytecode.
#
# I2C API (bugbuster.I2C):
#   .scan(start, stop, skip_reserved, timeout_ms)
#   .writeto(addr, buf, timeout_ms)
#   .readfrom(addr, n, timeout_ms)
#   .writeto_then_readfrom(addr, buf, rd_n, timeout_ms)
#
# SPI API (bugbuster.SPI):
#   .transfer(buf)

import bugbuster


class TMP102:
    """TI TMP102 I2C temperature sensor driver.

    Usage:
        i2c = bugbuster.I2C(sda_io=2, scl_io=3, freq=400000, pullups='external')
        sensor = bb_devices.TMP102(i2c, addr=0x48)
        print('Temp: %.2f C' % sensor.read_celsius())
    """

    REG_TEMP = 0x00

    def __init__(self, i2c, addr=0x48):
        self._i2c = i2c
        self._addr = addr

    def read_celsius(self):
        """Read temperature in degrees Celsius."""
        raw = self._i2c.writeto_then_readfrom(self._addr, bytes([self.REG_TEMP]), 2)
        msb = raw[0]
        lsb = raw[1]
        raw16 = (msb << 8) | lsb
        # TMP102: 12-bit, left-aligned in 16 bits, 0.0625 C/LSB
        raw12 = raw16 >> 4
        if raw12 & 0x800:
            raw12 = raw12 - 4096
        return raw12 * 0.0625


class BMP280:
    """Bosch BMP280 I2C pressure/temperature sensor driver.

    Usage:
        i2c = bugbuster.I2C(sda_io=2, scl_io=3, freq=400000, pullups='external')
        bmp = bb_devices.BMP280(i2c, addr=0x76)
        t_raw, p_raw = bmp.read()
        print('Temp raw: %d  Pressure raw: %d' % (t_raw, p_raw))

    Note: Returns raw uncompensated ADC values. Full compensation requires
    reading trimming parameters from the sensor. This minimal driver returns
    raw ADC values to demonstrate the I2C pattern.
    """

    REG_CHIP_ID = 0xD0
    REG_CTRL_MEAS = 0xF4
    REG_PRESS_MSB = 0xF7

    CHIP_ID = 0x60

    def __init__(self, i2c, addr=0x76):
        self._i2c = i2c
        self._addr = addr
        chip = self._i2c.writeto_then_readfrom(self._addr, bytes([self.REG_CHIP_ID]), 1)[0]
        if chip != self.CHIP_ID:
            raise OSError('BMP280 not found (chip_id=0x%02x)' % chip)
        # Normal mode, oversampling x1 for temp and pressure
        self._i2c.writeto(self._addr, bytes([self.REG_CTRL_MEAS, 0x27]))

    def read(self):
        """Return (temperature_raw, pressure_raw) as uncompensated integer ADC values."""
        data = self._i2c.writeto_then_readfrom(self._addr, bytes([self.REG_PRESS_MSB]), 6)
        press_raw = (data[0] << 12) | (data[1] << 4) | (data[2] >> 4)
        temp_raw = (data[3] << 12) | (data[4] << 4) | (data[5] >> 4)
        return temp_raw, press_raw


class MCP3008:
    """Microchip MCP3008 SPI 10-bit ADC driver (8 channels).

    Usage:
        spi = bugbuster.SPI(sck_io=4, mosi_io=5, miso_io=6, cs_io=7,
                            freq=1_000_000, mode=0)
        adc = bb_devices.MCP3008(spi)
        raw = adc.read(channel=0)          # 0-1023
        volts = raw * 3.3 / 1023.0
        print('CH0: %.3f V' % volts)
    """

    def __init__(self, spi, channels=8):
        self._spi = spi
        if channels not in (4, 8):
            raise ValueError('channels must be 4 or 8')
        self._channels = channels

    def read(self, channel, single_ended=True):
        """Read a channel, return raw 10-bit value (0-1023).

        Args:
            channel:      Channel index (0 to channels-1).
            single_ended: True for single-ended, False for differential.
        """
        if not (0 <= channel < self._channels):
            raise ValueError('channel out of range')
        start = 0x01
        config = (0x08 if single_ended else 0x00) | (channel & 0x07)
        cmd = bytes([start, config << 4, 0x00])
        rx = self._spi.transfer(cmd)
        return ((rx[1] & 0x03) << 8) | rx[2]
