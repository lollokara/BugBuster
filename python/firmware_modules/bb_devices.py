# bb_devices.py - BugBuster common device drivers (frozen module)
#
# These are intentionally small, educational drivers for popular parts.
# Import with: import bb_devices

import bugbuster

try:
    from machine import Pin as _Pin
except ImportError:
    _Pin = None

try:
    import onewire as _onewire
except ImportError:
    _onewire = None

try:
    import ds18x20 as _ds18x20
except ImportError:
    _ds18x20 = None

try:
    import ssd1306 as _ssd1306
except ImportError:
    _ssd1306 = None


def _u16_be(buf, idx=0):
    return (buf[idx] << 8) | buf[idx + 1]


def _u16_le(buf, idx=0):
    return buf[idx] | (buf[idx + 1] << 8)


def _s16_be(buf, idx=0):
    v = _u16_be(buf, idx)
    return v - 0x10000 if v & 0x8000 else v


def _s16_le(buf, idx=0):
    v = _u16_le(buf, idx)
    return v - 0x10000 if v & 0x8000 else v


def _s8(v):
    return v - 0x100 if v & 0x80 else v


def _bcd_to_int(v):
    return ((v >> 4) * 10) + (v & 0x0F)


def _int_to_bcd(v):
    return ((v // 10) << 4) | (v % 10)


def _crc8_poly31(data, init=0xFF):
    crc = init & 0xFF
    for b in data:
        crc ^= b
        for _ in range(8):
            if crc & 0x80:
                crc = ((crc << 1) ^ 0x31) & 0xFF
            else:
                crc = (crc << 1) & 0xFF
    return crc & 0xFF


def _clamp(v, lo, hi):
    if v < lo:
        return lo
    if v > hi:
        return hi
    return v


class TMP102:
    """TI TMP102 I2C temperature sensor driver."""

    REG_TEMP = 0x00

    def __init__(self, i2c, addr=0x48):
        self._i2c = i2c
        self._addr = addr

    def read_celsius(self):
        raw = self._i2c.writeto_then_readfrom(self._addr, bytes([self.REG_TEMP]), 2)
        raw12 = _u16_be(raw) >> 4
        if raw12 & 0x800:
            raw12 -= 0x1000
        return raw12 * 0.0625


class BMP280:
    """Bosch BMP280 I2C pressure/temperature sensor driver.

    Returns raw uncompensated ADC values.
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
            raise OSError("BMP280 not found (chip_id=0x%02x)" % chip)
        self._i2c.writeto(self._addr, bytes([self.REG_CTRL_MEAS, 0x27]))

    def read(self):
        data = self._i2c.writeto_then_readfrom(self._addr, bytes([self.REG_PRESS_MSB]), 6)
        press_raw = (data[0] << 12) | (data[1] << 4) | (data[2] >> 4)
        temp_raw = (data[3] << 12) | (data[4] << 4) | (data[5] >> 4)
        return temp_raw, press_raw


class MCP3008:
    """Microchip MCP3008 SPI 10-bit ADC driver."""

    def __init__(self, spi, channels=8):
        self._spi = spi
        if channels not in (4, 8):
            raise ValueError("channels must be 4 or 8")
        self._channels = channels

    def read(self, channel, single_ended=True):
        if not (0 <= channel < self._channels):
            raise ValueError("channel out of range")
        config = (0x08 if single_ended else 0x00) | (channel & 0x07)
        rx = self._spi.transfer(bytes([0x01, config << 4, 0x00]))
        return ((rx[1] & 0x03) << 8) | rx[2]


class DS18B20:
    """Dallas/Maxim DS18B20 1-Wire temperature sensor."""

    def __init__(self, pin):
        if _Pin is None or _onewire is None or _ds18x20 is None:
            raise OSError("DS18B20 requires machine.Pin, onewire, and ds18x20")
        if isinstance(pin, int):
            pin = _Pin(pin)
        self._pin = pin
        self._ow = _onewire.OneWire(pin)
        self._ds = _ds18x20.DS18X20(self._ow)

    def scan(self):
        return self._ds.scan()

    def roms(self):
        return self.scan()

    def read_celsius(self, rom=None, wait_ms=750):
        roms = self.scan() if rom is None else [rom]
        if not roms:
            raise OSError("no DS18B20 device found")
        if rom is None:
            if len(roms) != 1:
                raise ValueError("multiple sensors present; pass rom=...")
            rom = roms[0]
        self._ds.convert_temp()
        bugbuster.sleep(wait_ms)
        return self._ds.read_temp(rom)

    def read_all(self, wait_ms=750):
        roms = self.scan()
        if not roms:
            return []
        self._ds.convert_temp()
        bugbuster.sleep(wait_ms)
        out = []
        for rom in roms:
            out.append((rom, self._ds.read_temp(rom)))
        return out


class DS3231:
    """Maxim DS3231 RTC with temperature readback."""

    REG_TIME = 0x00
    REG_CTRL = 0x0E
    REG_STATUS = 0x0F
    REG_TEMP = 0x11

    def __init__(self, i2c, addr=0x68):
        self._i2c = i2c
        self._addr = addr

    def _read(self, reg, n):
        return self._i2c.writeto_then_readfrom(self._addr, bytes([reg]), n)

    def _write(self, reg, *vals):
        self._i2c.writeto(self._addr, bytes([reg] + list(vals)))

    def status(self):
        return self._read(self.REG_STATUS, 1)[0]

    def clear_osf(self):
        self._write(self.REG_STATUS, self.status() & ~0x80)

    def temperature_c(self):
        data = self._read(self.REG_TEMP, 2)
        raw = _s16_be(data) >> 6
        return raw * 0.25

    def read_datetime(self):
        data = self._read(self.REG_TIME, 7)
        second = _bcd_to_int(data[0] & 0x7F)
        minute = _bcd_to_int(data[1] & 0x7F)
        hour = _bcd_to_int(data[2] & 0x3F)
        weekday = _bcd_to_int(data[3] & 0x07)
        day = _bcd_to_int(data[4] & 0x3F)
        month = _bcd_to_int(data[5] & 0x1F)
        year = 2000 + _bcd_to_int(data[6])
        return (year, month, day, weekday, hour, minute, second)

    def set_datetime(self, year, month, day, weekday, hour, minute, second):
        self._write(
            self.REG_TIME,
            _int_to_bcd(second),
            _int_to_bcd(minute),
            _int_to_bcd(hour),
            _int_to_bcd(weekday),
            _int_to_bcd(day),
            _int_to_bcd(month),
            _int_to_bcd(year % 100),
        )


class BH1750:
    """ROHM BH1750 ambient light sensor."""

    POWER_DOWN = 0x00
    POWER_ON = 0x01
    RESET = 0x07
    CONT_HI_RES = 0x10
    CONT_HI_RES2 = 0x11
    CONT_LOW_RES = 0x13
    ONE_HI_RES = 0x20
    ONE_HI_RES2 = 0x21
    ONE_LOW_RES = 0x23

    def __init__(self, i2c, addr=0x23):
        self._i2c = i2c
        self._addr = addr
        self.power_on()
        self.reset()

    def _cmd(self, cmd):
        self._i2c.writeto(self._addr, bytes([cmd & 0xFF]))

    def power_on(self):
        self._cmd(self.POWER_ON)

    def power_down(self):
        self._cmd(self.POWER_DOWN)

    def reset(self):
        self._cmd(self.RESET)

    def set_mtreg(self, mtreg):
        mtreg = _clamp(int(mtreg), 31, 254)
        self._cmd(0x40 | (mtreg >> 5))
        self._cmd(0x60 | (mtreg & 0x1F))
        return mtreg

    def read_lux(self, high_res=True, one_time=True, mtreg=69):
        self.power_on()
        mtreg = self.set_mtreg(mtreg)
        if one_time:
            cmd = self.ONE_HI_RES if high_res else self.ONE_LOW_RES
        else:
            cmd = self.CONT_HI_RES if high_res else self.CONT_LOW_RES
        self._cmd(cmd)
        bugbuster.sleep(180 if high_res else 24)
        raw = _u16_be(self._i2c.readfrom(self._addr, 2))
        return (raw / 1.2) * (69.0 / mtreg)


class AHT20:
    """Aosong AHT20 humidity and temperature sensor."""

    def __init__(self, i2c, addr=0x38):
        self._i2c = i2c
        self._addr = addr
        self.reset()
        bugbuster.sleep(20)
        self.initialize()

    def _write(self, *vals):
        self._i2c.writeto(self._addr, bytes(vals))

    def _status(self):
        return self._i2c.writeto_then_readfrom(self._addr, b"\x71", 1)[0]

    def reset(self):
        self._write(0xBA)

    def initialize(self):
        self._write(0xBE, 0x08, 0x00)
        bugbuster.sleep(10)

    def read(self):
        self._write(0xAC, 0x33, 0x00)
        for _ in range(20):
            if not (self._status() & 0x80):
                break
            bugbuster.sleep(5)
        data = self._i2c.readfrom(self._addr, 7)
        if _crc8_poly31(data[:6], 0xFF) != data[6]:
            raise OSError("AHT20 CRC error")
        raw_h = ((data[1] << 12) | (data[2] << 4) | (data[3] >> 4)) & 0xFFFFF
        raw_t = (((data[3] & 0x0F) << 16) | (data[4] << 8) | data[5]) & 0xFFFFF
        humidity = raw_h * 100.0 / 1048576.0
        temperature = raw_t * 200.0 / 1048576.0 - 50.0
        return temperature, humidity

    def read_temperature(self):
        return self.read()[0]

    def read_humidity(self):
        return self.read()[1]


class SHT31:
    """Sensirion SHT31 temperature and humidity sensor."""

    CMD_SOFT_RESET = 0x30A2
    CMD_STATUS = 0xF32D
    CMD_MEAS_HIGH_STRETCH = 0x2C06
    CMD_MEAS_MED_STRETCH = 0x2C0D
    CMD_MEAS_LOW_STRETCH = 0x2C10
    CMD_MEAS_HIGH = 0x2400
    CMD_MEAS_MED = 0x240B
    CMD_MEAS_LOW = 0x2416

    def __init__(self, i2c, addr=0x44):
        self._i2c = i2c
        self._addr = addr

    def _cmd(self, cmd):
        self._i2c.writeto(self._addr, bytes([(cmd >> 8) & 0xFF, cmd & 0xFF]))

    def soft_reset(self):
        self._cmd(self.CMD_SOFT_RESET)
        bugbuster.sleep(2)

    def status(self):
        data = self._i2c.writeto_then_readfrom(self._addr, bytes([0xF3, 0x2D]), 3)
        if _crc8_poly31(data[:2], 0xFF) != data[2]:
            raise OSError("SHT31 status CRC error")
        return _u16_be(data)

    def read(self, repeatability="high", clock_stretch=False):
        if repeatability == "high":
            cmd = self.CMD_MEAS_HIGH_STRETCH if clock_stretch else self.CMD_MEAS_HIGH
            delay_ms = 15
        elif repeatability == "medium":
            cmd = self.CMD_MEAS_MED_STRETCH if clock_stretch else self.CMD_MEAS_MED
            delay_ms = 8
        else:
            cmd = self.CMD_MEAS_LOW_STRETCH if clock_stretch else self.CMD_MEAS_LOW
            delay_ms = 5
        self._cmd(cmd)
        if not clock_stretch:
            bugbuster.sleep(delay_ms)
        data = self._i2c.readfrom(self._addr, 6)
        if _crc8_poly31(data[0:2], 0xFF) != data[2]:
            raise OSError("SHT31 temperature CRC error")
        if _crc8_poly31(data[3:5], 0xFF) != data[5]:
            raise OSError("SHT31 humidity CRC error")
        raw_t = _u16_be(data, 0)
        raw_h = _u16_be(data, 3)
        temperature = -45.0 + (175.0 * raw_t / 65535.0)
        humidity = 100.0 * raw_h / 65535.0
        return temperature, humidity


class BME280:
    """Bosch BME280 compensated temperature / pressure / humidity sensor."""

    REG_CHIP_ID = 0xD0
    REG_RESET = 0xE0
    REG_CTRL_HUM = 0xF2
    REG_STATUS = 0xF3
    REG_CTRL_MEAS = 0xF4
    REG_CONFIG = 0xF5
    REG_DATA = 0xF7
    CHIP_ID = 0x60

    def __init__(self, i2c, addr=0x76):
        self._i2c = i2c
        self._addr = addr
        chip = self._read_u8(self.REG_CHIP_ID)
        if chip != self.CHIP_ID:
            raise OSError("BME280 not found (chip_id=0x%02x)" % chip)
        self._read_calibration()
        self._write_u8(self.REG_RESET, 0xB6)
        bugbuster.sleep(5)
        self._write_u8(self.REG_CTRL_HUM, 0x01)
        self._write_u8(self.REG_CTRL_MEAS, 0x27)
        self._write_u8(self.REG_CONFIG, 0xA0)
        self._t_fine = 0.0

    def _read_u8(self, reg):
        return self._i2c.writeto_then_readfrom(self._addr, bytes([reg]), 1)[0]

    def _read(self, reg, n):
        return self._i2c.writeto_then_readfrom(self._addr, bytes([reg]), n)

    def _write_u8(self, reg, value):
        self._i2c.writeto(self._addr, bytes([reg, value & 0xFF]))

    def _read_calibration(self):
        calib1 = self._read(0x88, 26)
        calib2 = self._read(0xE1, 7)
        self._dig_t1 = _u16_le(calib1, 0)
        self._dig_t2 = _s16_le(calib1, 2)
        self._dig_t3 = _s16_le(calib1, 4)
        self._dig_p1 = _u16_le(calib1, 6)
        self._dig_p2 = _s16_le(calib1, 8)
        self._dig_p3 = _s16_le(calib1, 10)
        self._dig_p4 = _s16_le(calib1, 12)
        self._dig_p5 = _s16_le(calib1, 14)
        self._dig_p6 = _s16_le(calib1, 16)
        self._dig_p7 = _s16_le(calib1, 18)
        self._dig_p8 = _s16_le(calib1, 20)
        self._dig_p9 = _s16_le(calib1, 22)
        self._dig_h1 = calib1[25]
        self._dig_h2 = _s16_le(calib2, 0)
        self._dig_h3 = calib2[2]
        self._dig_h4 = (calib2[3] << 4) | (calib2[4] & 0x0F)
        if self._dig_h4 & 0x800:
            self._dig_h4 -= 0x1000
        self._dig_h5 = (calib2[5] << 4) | (calib2[4] >> 4)
        if self._dig_h5 & 0x800:
            self._dig_h5 -= 0x1000
        self._dig_h6 = _s8(calib2[6])

    def _read_raw(self):
        data = self._read(self.REG_DATA, 8)
        adc_p = ((data[0] << 12) | (data[1] << 4) | (data[2] >> 4)) & 0xFFFFF
        adc_t = ((data[3] << 12) | (data[4] << 4) | (data[5] >> 4)) & 0xFFFFF
        adc_h = ((data[6] << 8) | data[7]) & 0xFFFF
        return adc_t, adc_p, adc_h

    def read(self):
        while self._read_u8(self.REG_STATUS) & 0x08:
            bugbuster.sleep(2)
        adc_t, adc_p, adc_h = self._read_raw()
        var1 = (adc_t / 16384.0 - self._dig_t1 / 1024.0) * self._dig_t2
        diff = adc_t / 131072.0 - self._dig_t1 / 8192.0
        var2 = diff * diff * self._dig_t3
        self._t_fine = var1 + var2
        temperature = self._t_fine / 5120.0

        var1 = self._t_fine / 2.0 - 64000.0
        var2 = var1 * var1 * self._dig_p6 / 32768.0
        var2 = var2 + var1 * self._dig_p5 * 2.0
        var2 = var2 / 4.0 + self._dig_p4 * 65536.0
        var1 = (self._dig_p3 * var1 * var1 / 524288.0 + self._dig_p2 * var1) / 524288.0
        var1 = (1.0 + var1 / 32768.0) * self._dig_p1
        if var1 == 0:
            pressure = 0.0
        else:
            p = 1048576.0 - adc_p
            p = (p - var2 / 4096.0) * 6250.0 / var1
            var1 = self._dig_p9 * p * p / 2147483648.0
            var2 = p * self._dig_p8 / 32768.0
            pressure = p + (var1 + var2 + self._dig_p7) / 16.0

        h = self._t_fine - 76800.0
        h = (adc_h - (self._dig_h4 * 64.0 + self._dig_h5 / 16384.0 * h)) * (
            self._dig_h2 / 65536.0 * (1.0 + self._dig_h6 / 67108864.0 * h * (1.0 + self._dig_h3 / 67108864.0 * h))
        )
        h = h * (1.0 - self._dig_h1 * h / 524288.0)
        h = _clamp(h, 0.0, 100.0)
        return temperature, pressure, h

    def read_raw(self):
        return self._read_raw()


class INA219:
    """TI INA219 current/power monitor."""

    REG_CONFIG = 0x00
    REG_SHUNT_V = 0x01
    REG_BUS_V = 0x02
    REG_POWER = 0x03
    REG_CURRENT = 0x04
    REG_CALIB = 0x05

    def __init__(self, i2c, addr=0x40, shunt_ohms=0.1, max_expected_amps=2.0):
        self._i2c = i2c
        self._addr = addr
        self._shunt_ohms = float(shunt_ohms)
        self._current_lsb = float(max_expected_amps) / 32768.0
        if self._current_lsb <= 0.0:
            self._current_lsb = 0.0001
        self._power_lsb = self._current_lsb * 20.0
        cal = int(0.04096 / (self._current_lsb * self._shunt_ohms))
        if cal < 1:
            cal = 1
        self._cal = cal & 0xFFFF
        self._write_u16(self.REG_CALIB, self._cal)
        self._write_u16(self.REG_CONFIG, 0x399F)

    def _write_u16(self, reg, value):
        self._i2c.writeto(self._addr, bytes([reg, (value >> 8) & 0xFF, value & 0xFF]))

    def _read_u16(self, reg):
        return _u16_be(self._i2c.writeto_then_readfrom(self._addr, bytes([reg]), 2))

    def _read_s16(self, reg):
        return _s16_be(self._i2c.writeto_then_readfrom(self._addr, bytes([reg]), 2))

    def read_shunt_voltage(self):
        return self._read_s16(self.REG_SHUNT_V) * 0.00001

    def read_bus_voltage(self):
        return ((self._read_u16(self.REG_BUS_V) >> 3) * 0.004)

    def read_current(self):
        return self._read_s16(self.REG_CURRENT) * self._current_lsb

    def read_power(self):
        return self._read_u16(self.REG_POWER) * self._power_lsb

    def read(self):
        return (self.read_bus_voltage(), self.read_shunt_voltage(), self.read_current(), self.read_power())


class ADS1115:
    """TI ADS1115 I2C ADC."""

    REG_CONV = 0x00
    REG_CONFIG = 0x01
    REG_LO = 0x02
    REG_HI = 0x03
    _ADDRS = (0x48, 0x49, 0x4A, 0x4B)
    _PGA_TO_BITS = {
        6.144: 0,
        4.096: 1,
        2.048: 2,
        1.024: 3,
        0.512: 4,
        0.256: 5,
    }
    _FSR = {
        0: 6.144,
        1: 4.096,
        2: 2.048,
        3: 1.024,
        4: 0.512,
        5: 0.256,
    }
    _DR = {
        8: 0,
        16: 1,
        32: 2,
        64: 3,
        128: 4,
        250: 5,
        475: 6,
        860: 7,
    }

    def __init__(self, i2c, addr=0x48):
        self._i2c = i2c
        self._addr = addr

    def _write_u16(self, reg, value):
        self._i2c.writeto(self._addr, bytes([reg, (value >> 8) & 0xFF, value & 0xFF]))

    def _read_u16(self, reg):
        return _u16_be(self._i2c.writeto_then_readfrom(self._addr, bytes([reg]), 2))

    def _read_s16(self, reg):
        return _s16_be(self._i2c.writeto_then_readfrom(self._addr, bytes([reg]), 2))

    def _config(self, channel, pga, data_rate, continuous=False, differential=False):
        if differential:
            mux = (0x00 + channel) & 0x07
        else:
            if channel not in (0, 1, 2, 3):
                raise ValueError("channel must be 0..3")
            mux = 0x04 + channel
        pga_bits = self._PGA_TO_BITS.get(float(pga))
        if pga_bits is None:
            raise ValueError("unsupported pga")
        dr_bits = self._DR.get(int(data_rate))
        if dr_bits is None:
            raise ValueError("unsupported data_rate")
        cfg = 0x8000
        cfg |= (mux & 0x07) << 12
        cfg |= (pga_bits & 0x07) << 9
        if not continuous:
            cfg |= 0x0100
        cfg |= (dr_bits & 0x07) << 5
        cfg |= 0x0003
        return cfg

    def read_raw(self, channel=0, pga=4.096, data_rate=128, continuous=False, differential=False):
        cfg = self._config(channel, pga, data_rate, continuous=continuous, differential=differential)
        self._write_u16(self.REG_CONFIG, cfg)
        if continuous:
            bugbuster.sleep(1000 // max(1, int(data_rate)))
        else:
            for _ in range(20):
                if self._read_u16(self.REG_CONFIG) & 0x8000:
                    break
                bugbuster.sleep(2)
        return self._read_s16(self.REG_CONV)

    def read_voltage(self, channel=0, pga=4.096, data_rate=128, continuous=False, differential=False):
        raw = self.read_raw(channel=channel, pga=pga, data_rate=data_rate, continuous=continuous, differential=differential)
        fsr = self._FSR.get(self._PGA_TO_BITS[float(pga)])
        return (raw * fsr) / 32768.0


class MCP23017:
    """Microchip MCP23017 16-bit GPIO expander."""

    REG_IODIRA = 0x00
    REG_IODIRB = 0x01
    REG_IPOLA = 0x02
    REG_IPOLB = 0x03
    REG_GPINTENA = 0x04
    REG_GPINTENB = 0x05
    REG_DEFVALA = 0x06
    REG_DEFVALB = 0x07
    REG_INTCONA = 0x08
    REG_INTCONB = 0x09
    REG_IOCON = 0x0A
    REG_GPPUA = 0x0C
    REG_GPPUB = 0x0D
    REG_INTFA = 0x0E
    REG_INTFB = 0x0F
    REG_INTCAPA = 0x10
    REG_INTCAPB = 0x11
    REG_GPIOA = 0x12
    REG_GPIOB = 0x13
    REG_OLATA = 0x14
    REG_OLATB = 0x15

    def __init__(self, i2c, addr=0x20):
        self._i2c = i2c
        self._addr = addr
        self.write_reg(self.REG_IOCON, 0x20)
        self.set_direction(0xFFFF)

    def write_reg(self, reg, value):
        self._i2c.writeto(self._addr, bytes([reg, value & 0xFF]))

    def read_reg(self, reg):
        return self._i2c.writeto_then_readfrom(self._addr, bytes([reg]), 1)[0]

    def _update_bit(self, reg, pin, enabled):
        if not (0 <= pin <= 15):
            raise ValueError("pin must be 0..15")
        if pin < 8:
            value = self.read_reg(reg)
            mask = 1 << pin
            if enabled:
                value |= mask
            else:
                value &= ~mask
            self.write_reg(reg, value)
        else:
            value = self.read_reg(reg + 1)
            mask = 1 << (pin - 8)
            if enabled:
                value |= mask
            else:
                value &= ~mask
            self.write_reg(reg + 1, value)

    def set_direction(self, direction_mask):
        self.write_reg(self.REG_IODIRA, direction_mask & 0xFF)
        self.write_reg(self.REG_IODIRB, (direction_mask >> 8) & 0xFF)

    def set_dir(self, pin, output=True):
        """Set one MCP23017 pin direction.

        output=True maps to digital output, output=False maps to input.
        """
        self._update_bit(self.REG_IODIRA, pin, not bool(output))

    def set_pullups(self, pullup_mask):
        self.write_reg(self.REG_GPPUA, pullup_mask & 0xFF)
        self.write_reg(self.REG_GPPUB, (pullup_mask >> 8) & 0xFF)

    def set_pullup(self, pin, enable=True):
        """Enable or disable the internal pull-up for one pin."""
        self._update_bit(self.REG_GPPUA, pin, bool(enable))

    def write_port(self, value):
        self.write_reg(self.REG_GPIOA, value & 0xFF)
        self.write_reg(self.REG_GPIOB, (value >> 8) & 0xFF)

    def read_port(self):
        return self.read_reg(self.REG_GPIOA) | (self.read_reg(self.REG_GPIOB) << 8)

    def set_pin(self, pin, value):
        if not (0 <= pin <= 15):
            raise ValueError("pin must be 0..15")
        mask = 1 << pin
        port = self.read_port()
        if value:
            port |= mask
        else:
            port &= ~mask
        self.write_port(port)

    def read_pin(self, pin):
        if not (0 <= pin <= 15):
            raise ValueError("pin must be 0..15")
        return 1 if (self.read_port() >> pin) & 1 else 0

    def pin_mode(self, pin, output=True, pullup=False):
        """Convenience helper matching Arduino-style pin setup."""
        self.set_dir(pin, output=output)
        self.set_pullup(pin, enable=pullup)


class PCF8574:
    """NXP PCF8574 quasi-bidirectional I/O expander."""

    def __init__(self, i2c, addr=0x20, value=0xFF):
        self._i2c = i2c
        self._addr = addr
        self.write(value)

    def write(self, value):
        self._i2c.writeto(self._addr, bytes([value & 0xFF]))

    def read(self):
        return self._i2c.readfrom(self._addr, 1)[0]

    def set_pin(self, pin, value):
        if not (0 <= pin <= 7):
            raise ValueError("pin must be 0..7")
        port = self.read()
        if value:
            port |= (1 << pin)
        else:
            port &= ~(1 << pin)
        self.write(port)

    def read_pin(self, pin):
        if not (0 <= pin <= 7):
            raise ValueError("pin must be 0..7")
        return 1 if (self.read() >> pin) & 1 else 0


class PCA9685:
    """NXP PCA9685 16-channel PWM controller."""

    REG_MODE1 = 0x00
    REG_MODE2 = 0x01
    REG_SUBADR1 = 0x02
    REG_SUBADR2 = 0x03
    REG_SUBADR3 = 0x04
    REG_ALLCALLADR = 0x05
    REG_LED0_ON_L = 0x06
    REG_PRE_SCALE = 0xFE

    def __init__(self, i2c, addr=0x40, osc_hz=25000000):
        self._i2c = i2c
        self._addr = addr
        self._osc_hz = float(osc_hz)
        self.write_reg(self.REG_MODE1, 0x00)
        self.write_reg(self.REG_MODE2, 0x04)
        self.set_pwm_freq(50)

    def write_reg(self, reg, value):
        self._i2c.writeto(self._addr, bytes([reg, value & 0xFF]))

    def read_reg(self, reg):
        return self._i2c.writeto_then_readfrom(self._addr, bytes([reg]), 1)[0]

    def set_pwm_freq(self, freq_hz):
        freq_hz = float(freq_hz)
        if freq_hz <= 0.0:
            raise ValueError("freq_hz must be > 0")
        prescale = int((self._osc_hz / (4096.0 * freq_hz)) + 0.5) - 1
        prescale = _clamp(prescale, 3, 255)
        old_mode = self.read_reg(self.REG_MODE1)
        self.write_reg(self.REG_MODE1, (old_mode & 0x7F) | 0x10)
        self.write_reg(self.REG_PRE_SCALE, prescale)
        self.write_reg(self.REG_MODE1, old_mode & ~0x10)
        bugbuster.sleep(1)
        self.write_reg(self.REG_MODE1, old_mode | 0x80)
        return prescale

    def set_pwm(self, channel, on=0, off=0):
        if not (0 <= channel <= 15):
            raise ValueError("channel must be 0..15")
        base = self.REG_LED0_ON_L + 4 * channel
        data = bytes([
            base,
            on & 0xFF,
            (on >> 8) & 0x0F,
            off & 0xFF,
            (off >> 8) & 0x0F,
        ])
        self._i2c.writeto(self._addr, data)

    def set_duty(self, channel, duty):
        duty = _clamp(float(duty), 0.0, 1.0)
        off = int(duty * 4095.0 + 0.5)
        self.set_pwm(channel, 0, off)


class TCS34725:
    """ams OSRAM TCS34725 RGBC color sensor."""

    REG_ENABLE = 0x00
    REG_ATIME = 0x01
    REG_WTIME = 0x03
    REG_PERS = 0x0C
    REG_CONFIG = 0x0D
    REG_CONTROL = 0x0F
    REG_ID = 0x12
    REG_CDATAL = 0x14
    CMD = 0x80

    _GAIN_TO_BITS = {1: 0, 4: 1, 16: 2, 60: 3}

    def __init__(self, i2c, addr=0x29, integration_ms=50, gain=4):
        self._i2c = i2c
        self._addr = addr
        self._integration_ms = 50
        chip = self.read_u8(self.REG_ID)
        if chip not in (0x44, 0x4D):
            raise OSError("TCS34725 not found (id=0x%02x)" % chip)
        self.set_integration_time(integration_ms)
        self.set_gain(gain)
        self.enable()

    def write_u8(self, reg, value):
        self._i2c.writeto(self._addr, bytes([self.CMD | (reg & 0x1F), value & 0xFF]))

    def read_u8(self, reg):
        return self._i2c.writeto_then_readfrom(self._addr, bytes([self.CMD | 0xA0 | (reg & 0x1F)]), 1)[0]

    def read_words(self, reg, count=4):
        data = self._i2c.writeto_then_readfrom(self._addr, bytes([self.CMD | 0xA0 | (reg & 0x1F)]), count * 2)
        out = []
        for i in range(count):
            out.append(_u16_le(data, i * 2))
        return out

    def enable(self):
        self.write_u8(self.REG_ENABLE, 0x01)
        bugbuster.sleep(3)
        self.write_u8(self.REG_ENABLE, 0x03)

    def disable(self):
        self.write_u8(self.REG_ENABLE, 0x00)

    def set_integration_time(self, integration_ms):
        atime = int(256 - (float(integration_ms) / 2.4))
        atime = _clamp(atime, 0, 255)
        self.write_u8(self.REG_ATIME, atime)
        self._integration_ms = int(integration_ms)
        return atime

    def set_gain(self, gain):
        bits = self._GAIN_TO_BITS.get(int(gain))
        if bits is None:
            raise ValueError("gain must be 1, 4, 16, or 60")
        self.write_u8(self.REG_CONTROL, bits)
        return gain

    def read_rgbc(self):
        bugbuster.sleep(self._integration_ms)
        clear, red, green, blue = self.read_words(self.REG_CDATAL, 4)
        return red, green, blue, clear


class MPU6050:
    """InvenSense MPU6050 6-axis IMU."""

    REG_SMPLRT_DIV = 0x19
    REG_CONFIG = 0x1A
    REG_GYRO_CONFIG = 0x1B
    REG_ACCEL_CONFIG = 0x1C
    REG_FIFO_EN = 0x23
    REG_INT_ENABLE = 0x38
    REG_INT_STATUS = 0x3A
    REG_ACCEL_XOUT_H = 0x3B
    REG_TEMP_OUT_H = 0x41
    REG_GYRO_XOUT_H = 0x43
    REG_PWR_MGMT_1 = 0x6B
    REG_PWR_MGMT_2 = 0x6C
    REG_WHO_AM_I = 0x75

    _ACCEL_SCALE = {2: 16384.0, 4: 8192.0, 8: 4096.0, 16: 2048.0}
    _GYRO_SCALE = {250: 131.0, 500: 65.5, 1000: 32.8, 2000: 16.4}

    def __init__(self, i2c, addr=0x68, accel_range=2, gyro_range=250):
        self._i2c = i2c
        self._addr = addr
        who = self.read_u8(self.REG_WHO_AM_I) & 0x7F
        if who != 0x68:
            raise OSError("MPU6050 not found (whoami=0x%02x)" % who)
        self.wake()
        self.set_ranges(accel_range, gyro_range)

    def read_u8(self, reg):
        return self._i2c.writeto_then_readfrom(self._addr, bytes([reg]), 1)[0]

    def write_u8(self, reg, value):
        self._i2c.writeto(self._addr, bytes([reg, value & 0xFF]))

    def wake(self):
        self.write_u8(self.REG_PWR_MGMT_1, 0x01)
        bugbuster.sleep(10)

    def set_ranges(self, accel_range=2, gyro_range=250):
        accel_bits = {2: 0, 4: 1, 8: 2, 16: 3}.get(int(accel_range))
        gyro_bits = {250: 0, 500: 1, 1000: 2, 2000: 3}.get(int(gyro_range))
        if accel_bits is None:
            raise ValueError("accel_range must be 2, 4, 8, or 16")
        if gyro_bits is None:
            raise ValueError("gyro_range must be 250, 500, 1000, or 2000")
        self.write_u8(self.REG_ACCEL_CONFIG, accel_bits << 3)
        self.write_u8(self.REG_GYRO_CONFIG, gyro_bits << 3)
        self._accel_scale = self._ACCEL_SCALE[int(accel_range)]
        self._gyro_scale = self._GYRO_SCALE[int(gyro_range)]
        return accel_range, gyro_range

    def _read_block(self, reg, n):
        return self._i2c.writeto_then_readfrom(self._addr, bytes([reg]), n)

    def read_temperature(self):
        raw = _s16_be(self._read_block(self.REG_TEMP_OUT_H, 2))
        return raw / 340.0 + 36.53

    def read_accel(self):
        raw = self._read_block(self.REG_ACCEL_XOUT_H, 6)
        ax = _s16_be(raw, 0) / self._accel_scale
        ay = _s16_be(raw, 2) / self._accel_scale
        az = _s16_be(raw, 4) / self._accel_scale
        return ax, ay, az

    def read_gyro(self):
        raw = self._read_block(self.REG_GYRO_XOUT_H, 6)
        gx = _s16_be(raw, 0) / self._gyro_scale
        gy = _s16_be(raw, 2) / self._gyro_scale
        gz = _s16_be(raw, 4) / self._gyro_scale
        return gx, gy, gz

    def read(self):
        return self.read_accel(), self.read_gyro(), self.read_temperature()


class MCP4725:
    """Microchip MCP4725 12-bit I2C DAC."""

    def __init__(self, i2c, addr=0x60, vref=3.3):
        self._i2c = i2c
        self._addr = addr
        self._vref = float(vref)

    def set_raw(self, code):
        code = int(code) & 0x0FFF
        self._i2c.writeto(self._addr, bytes([0x40, (code >> 4) & 0xFF, (code << 4) & 0xF0]))
        return code

    def set_voltage(self, voltage, vref=None):
        if vref is None:
            vref = self._vref
        code = int(_clamp(float(voltage), 0.0, float(vref)) * 4095.0 / float(vref) + 0.5)
        return self.set_raw(code)

    def sleep(self):
        self._i2c.writeto(self._addr, bytes([0x40, 0x00, 0x00]))


if _ssd1306 is not None:
    class SSD1306_I2C(_ssd1306.SSD1306_I2C):
        pass

    class SSD1306_SPI(_ssd1306.SSD1306_SPI):
        pass

    SSD1306 = SSD1306_I2C
else:
    class SSD1306_I2C(object):
        def __init__(self, *args, **kwargs):
            raise OSError("ssd1306 module not available")

    class SSD1306_SPI(object):
        def __init__(self, *args, **kwargs):
            raise OSError("ssd1306 module not available")

    SSD1306 = SSD1306_I2C
