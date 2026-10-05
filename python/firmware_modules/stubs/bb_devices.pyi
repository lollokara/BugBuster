"""Drivers for common I2C/SPI/1-Wire/display parts (``bb_devices.py`` on the device).

Frozen into the firmware: ``import bb_devices``. Pass a ``bugbuster.I2C`` or
``bugbuster.SPI`` bus object; the I2C drivers talk to the part through
``writeto``, ``readfrom`` and ``writeto_then_readfrom``. Addresses are 7-bit.
Drivers read the part directly; none of them power it, so create the bus with a
``supply`` that matches the part first. Errors from the bus surface as
``OSError``; a wrong chip id on construction raises ``OSError`` too.

Example:
    import bugbuster
    import bb_devices
    bus = bugbuster.I2C(1, 2, freq=100000, supply=3.3, vlogic=3.3)
    try:
        sensor = bb_devices.BME280(bus)
        temp_c, press_pa, rh = sensor.read()
    finally:
        bus.close()
"""

class TMP102:
    """TI TMP102 I2C temperature sensor (12-bit, 0.0625 C per step).

    Example:
        t = bb_devices.TMP102(bus).read_celsius()
    """
    def __init__(self, i2c: bugbuster.I2C, addr: int = 0x48) -> None:
        """Attach to the sensor. No bus traffic until you read.

        Args:
            i2c: A ``bugbuster.I2C`` bus.
            addr: 7-bit address, 0x48-0x4B by the ADD0 pin. Default 0x48.

        Example:
            sensor = bb_devices.TMP102(bus, addr=0x48)
        """
    def read_celsius(self) -> float:
        """Read the temperature.

        Returns:
            Temperature in degrees Celsius.

        Raises:
            OSError: the bus transfer fails.

        Example:
            print("%.2f C" % sensor.read_celsius())
        """

class BMP280:
    """Bosch BMP280 pressure/temperature sensor (raw values only).

    Starts the part in normal mode. For compensated pressure, temperature and
    humidity use :class:`BME280`.

    Example:
        temp_raw, press_raw = bb_devices.BMP280(bus).read()
    """
    def __init__(self, i2c: bugbuster.I2C, addr: int = 0x76) -> None:
        """Check the chip id and start measuring.

        Args:
            i2c: A ``bugbuster.I2C`` bus.
            addr: 7-bit address, 0x76 or 0x77. Default 0x76.

        Raises:
            OSError: the chip id is not 0x60 (no BMP280 at that address).

        Example:
            sensor = bb_devices.BMP280(bus)
        """
    def read(self) -> tuple[int, int]:
        """Read the raw ADC values.

        Returns:
            ``(temperature_raw, pressure_raw)`` as uncompensated 20-bit integers.

        Raises:
            OSError: the bus transfer fails.

        Example:
            temp_raw, press_raw = sensor.read()
        """

class MCP3008:
    """Microchip MCP3008 / MCP3004 10-bit SPI ADC.

    Example:
        adc = bb_devices.MCP3008(spi)
        raw = adc.read(0)
    """
    def __init__(self, spi: bugbuster.SPI, channels: int = 8) -> None:
        """Attach to the converter.

        Args:
            spi: A ``bugbuster.SPI`` bus with MOSI, MISO and chip select.
            channels: 8 for the MCP3008, 4 for the MCP3004.

        Raises:
            ValueError: ``channels`` is not 4 or 8.

        Example:
            adc = bb_devices.MCP3008(spi, channels=8)
        """
    def read(self, channel: int, single_ended: bool = True) -> int:
        """Convert one input.

        Args:
            channel: Input number, 0 up to ``channels - 1``.
            single_ended: True (default) for single-ended, False for differential pairs.

        Returns:
            The 10-bit code, 0-1023 (multiply by Vref / 1024 for volts).

        Raises:
            ValueError: ``channel`` is out of range.
            OSError: the SPI transfer fails.

        Example:
            volts = adc.read(0) * 3.3 / 1024
        """

class DS18B20:
    """Dallas/Maxim DS18B20 1-Wire temperature sensor.

    Needs ``machine.Pin`` on a free GPIO with a 4.7 kOhm pull-up to the sensor
    supply.

    Example:
        ds = bb_devices.DS18B20(machine.Pin(4))
        print(ds.read_celsius())
    """
    def __init__(self, pin: int | machine.Pin) -> None:
        """Set up the 1-Wire bus on a pin.

        Args:
            pin: A ``machine.Pin`` or GPIO number.

        Raises:
            OSError: the 1-Wire/DS18x20 support is not available.

        Example:
            ds = bb_devices.DS18B20(machine.Pin(4))
        """
    def scan(self) -> list[bytes]:
        """Find the sensors on the bus.

        Returns:
            A list of 8-byte ROM codes.

        Example:
            print(len(ds.scan()), "sensor(s)")
        """
    def roms(self) -> list[bytes]:
        """Alias of :meth:`scan`.

        Returns:
            A list of 8-byte ROM codes.

        Example:
            roms = ds.roms()
        """
    def read_celsius(self, rom: bytes | None = None, wait_ms: int = 750) -> float:
        """Convert and read one sensor.

        Args:
            rom: ROM code of the sensor; None uses the only sensor on the bus.
            wait_ms: Conversion wait in milliseconds. Default 750 (12-bit).

        Returns:
            Temperature in degrees Celsius.

        Raises:
            OSError: no sensor found.
            ValueError: several sensors present and ``rom`` not given.

        Example:
            print(ds.read_celsius())
        """
    def read_all(self, wait_ms: int = 750) -> list[tuple[bytes, float]]:
        """Convert and read every sensor on the bus with one conversion.

        Args:
            wait_ms: Conversion wait in milliseconds. Default 750.

        Returns:
            A list of ``(rom, celsius)`` tuples; empty when no sensor is found.

        Example:
            for rom, celsius in ds.read_all():
                print(rom.hex(), celsius)
        """

class DS3231:
    """Maxim DS3231 real-time clock with temperature readback.

    Example:
        rtc = bb_devices.DS3231(bus)
        print(rtc.read_datetime())
    """
    def __init__(self, i2c: bugbuster.I2C, addr: int = 0x68) -> None:
        """Attach to the clock. No bus traffic until you read.

        Args:
            i2c: A ``bugbuster.I2C`` bus.
            addr: 7-bit address. Default 0x68.

        Example:
            rtc = bb_devices.DS3231(bus)
        """
    def status(self) -> int:
        """Read the status register.

        Returns:
            The status byte; bit 7 (OSF) set means the oscillator stopped and the time is unreliable.

        Example:
            stopped = rtc.status() & 0x80
        """
    def clear_osf(self) -> None:
        """Clear the oscillator-stop flag after setting the time.

        Example:
            rtc.clear_osf()
        """
    def temperature_c(self) -> float:
        """Read the internal temperature sensor.

        Returns:
            Temperature in degrees Celsius (0.25 C resolution).

        Example:
            print(rtc.temperature_c())
        """
    def read_datetime(self) -> tuple[int, int, int, int, int, int, int]:
        """Read the date and time.

        Returns:
            ``(year, month, day, weekday, hour, minute, second)``; year is 2000-2099.

        Example:
            year, month, day, weekday, hour, minute, second = rtc.read_datetime()
        """
    def set_datetime(self, year: int, month: int, day: int, weekday: int, hour: int,
                     minute: int, second: int) -> None:
        """Set the date and time (24-hour clock).

        Args:
            year: Four-digit year; only the last two digits are stored.
            month: 1-12.
            day: 1-31.
            weekday: 1-7 (your own convention, stored as given).
            hour: 0-23.
            minute: 0-59.
            second: 0-59.

        Example:
            rtc.set_datetime(2026, 10, 4, 7, 12, 30, 0)
            rtc.clear_osf()
        """

class BH1750:
    """ROHM BH1750 ambient light sensor.

    Example:
        lux = bb_devices.BH1750(bus).read_lux()
    """
    def __init__(self, i2c: bugbuster.I2C, addr: int = 0x23) -> None:
        """Power the sensor on and reset it.

        Args:
            i2c: A ``bugbuster.I2C`` bus.
            addr: 7-bit address, 0x23 (ADDR low) or 0x5C. Default 0x23.

        Example:
            light = bb_devices.BH1750(bus)
        """
    def power_on(self) -> None:
        """Wake the sensor.

        Example:
            light.power_on()
        """
    def power_down(self) -> None:
        """Put the sensor in its low-power state.

        Example:
            light.power_down()
        """
    def reset(self) -> None:
        """Clear the data register (the sensor must be powered on).

        Example:
            light.reset()
        """
    def set_mtreg(self, mtreg: int) -> int:
        """Set the measurement-time register (sensitivity).

        Args:
            mtreg: 31-254, clamped. Default hardware value is 69.

        Returns:
            The value actually applied.

        Example:
            applied = light.set_mtreg(138)
        """
    def read_lux(self, high_res: bool = True, one_time: bool = True, mtreg: int = 69) -> float:
        """Measure the illuminance.

        Blocks about 180 ms (high resolution) or 24 ms (low resolution).

        Args:
            high_res: True (default) for 1 lx resolution, False for 4 lx.
            one_time: True (default) for one-shot, False for continuous mode.
            mtreg: Measurement-time register, 31-254. Default 69.

        Returns:
            Illuminance in lux.

        Example:
            print("%.1f lx" % light.read_lux())
        """

class AHT20:
    """Aosong AHT20 humidity and temperature sensor.

    Example:
        temp_c, rh = bb_devices.AHT20(bus).read()
    """
    def __init__(self, i2c: bugbuster.I2C, addr: int = 0x38) -> None:
        """Reset and initialise the sensor (about 30 ms).

        Args:
            i2c: A ``bugbuster.I2C`` bus.
            addr: 7-bit address. Default 0x38.

        Example:
            sensor = bb_devices.AHT20(bus)
        """
    def reset(self) -> None:
        """Soft-reset the sensor.

        Example:
            sensor.reset()
        """
    def initialize(self) -> None:
        """Send the initialisation command.

        Example:
            sensor.initialize()
        """
    def read(self) -> tuple[float, float]:
        """Trigger a measurement and read it (CRC checked).

        Returns:
            ``(temperature_celsius, relative_humidity_percent)``.

        Raises:
            OSError: CRC mismatch or bus failure.

        Example:
            temp_c, rh = sensor.read()
        """
    def read_temperature(self) -> float:
        """Measure and return only the temperature.

        Returns:
            Temperature in degrees Celsius.

        Example:
            print(sensor.read_temperature())
        """
    def read_humidity(self) -> float:
        """Measure and return only the humidity.

        Returns:
            Relative humidity in percent.

        Example:
            print(sensor.read_humidity())
        """

class SHT31:
    """Sensirion SHT31 temperature and humidity sensor.

    Example:
        temp_c, rh = bb_devices.SHT31(bus).read()
    """
    def __init__(self, i2c: bugbuster.I2C, addr: int = 0x44) -> None:
        """Attach to the sensor. No bus traffic until you use it.

        Args:
            i2c: A ``bugbuster.I2C`` bus.
            addr: 7-bit address, 0x44 or 0x45. Default 0x44.

        Example:
            sensor = bb_devices.SHT31(bus)
        """
    def soft_reset(self) -> None:
        """Reset the sensor (2 ms wait).

        Example:
            sensor.soft_reset()
        """
    def status(self) -> int:
        """Read the 16-bit status register (CRC checked).

        Returns:
            The status word.

        Raises:
            OSError: CRC mismatch.

        Example:
            print(hex(sensor.status()))
        """
    def read(self, repeatability: str = "high", clock_stretch: bool = False) -> tuple[float, float]:
        """Measure temperature and humidity (CRC checked).

        Args:
            repeatability: 'high' (15 ms), 'medium' (8 ms) or 'low' (5 ms). Default 'high'.
            clock_stretch: True lets the sensor stretch the clock instead of the driver waiting. Default False.

        Returns:
            ``(temperature_celsius, relative_humidity_percent)``.

        Raises:
            OSError: CRC mismatch or bus failure.

        Example:
            temp_c, rh = sensor.read(repeatability="medium")
        """

class BME280:
    """Bosch BME280 compensated temperature, pressure and humidity sensor.

    Example:
        temp_c, press_pa, rh = bb_devices.BME280(bus).read()
    """
    def __init__(self, i2c: bugbuster.I2C, addr: int = 0x76) -> None:
        """Check the chip id, load the calibration and start measuring (normal mode).

        Args:
            i2c: A ``bugbuster.I2C`` bus.
            addr: 7-bit address, 0x76 or 0x77. Default 0x76.

        Raises:
            OSError: the chip id is not 0x60 (no BME280 at that address).

        Example:
            sensor = bb_devices.BME280(bus, addr=0x76)
        """
    def read(self) -> tuple[float, float, float]:
        """Read compensated values (waits for any running conversion).

        Returns:
            ``(temperature_celsius, pressure_pascal, relative_humidity_percent)``.

        Example:
            temp_c, press_pa, rh = sensor.read()
        """
    def read_raw(self) -> tuple[int, int, int]:
        """Read the raw ADC values.

        Returns:
            ``(temperature_raw, pressure_raw, humidity_raw)`` uncompensated integers.

        Example:
            adc_t, adc_p, adc_h = sensor.read_raw()
        """

class INA219:
    """TI INA219 current and power monitor.

    Calibrated on construction from the shunt resistor and the expected maximum current.

    Example:
        ina = bb_devices.INA219(bus, shunt_ohms=0.1, max_expected_amps=2.0)
        bus_v, shunt_v, amps, watts = ina.read()
    """
    def __init__(self, i2c: bugbuster.I2C, addr: int = 0x40, shunt_ohms: float = 0.1,
                 max_expected_amps: float = 2.0) -> None:
        """Write the calibration and configuration registers.

        Args:
            i2c: A ``bugbuster.I2C`` bus.
            addr: 7-bit address, 0x40-0x4F. Default 0x40.
            shunt_ohms: Shunt resistor in ohms. Default 0.1.
            max_expected_amps: Largest current you expect, in amps; sets the resolution. Default 2.0.

        Example:
            ina = bb_devices.INA219(bus, shunt_ohms=0.1, max_expected_amps=1.0)
        """
    def read_shunt_voltage(self) -> float:
        """Read the voltage across the shunt.

        Returns:
            Volts (10 uV per step).

        Example:
            print(ina.read_shunt_voltage())
        """
    def read_bus_voltage(self) -> float:
        """Read the bus voltage.

        Returns:
            Volts (4 mV per step).

        Example:
            print(ina.read_bus_voltage())
        """
    def read_current(self) -> float:
        """Read the current.

        Returns:
            Amps, from the calibrated current register.

        Example:
            print("%.1f mA" % (ina.read_current() * 1e3))
        """
    def read_power(self) -> float:
        """Read the power.

        Returns:
            Watts, from the calibrated power register.

        Example:
            print("%.1f mW" % (ina.read_power() * 1e3))
        """
    def read(self) -> tuple[float, float, float, float]:
        """Read all four values.

        Returns:
            ``(bus_volts, shunt_volts, amps, watts)``.

        Example:
            bus_v, shunt_v, amps, watts = ina.read()
        """

class ADS1115:
    """TI ADS1115 16-bit I2C ADC.

    Example:
        adc = bb_devices.ADS1115(bus)
        volts = adc.read_voltage(channel=0, pga=4.096)
    """
    def __init__(self, i2c: bugbuster.I2C, addr: int = 0x48) -> None:
        """Attach to the converter. No bus traffic until you read.

        Args:
            i2c: A ``bugbuster.I2C`` bus.
            addr: 7-bit address, 0x48-0x4B by the ADDR pin. Default 0x48.

        Example:
            adc = bb_devices.ADS1115(bus)
        """
    def read_raw(self, channel: int = 0, pga: float = 4.096, data_rate: int = 128,
                 continuous: bool = False, differential: bool = False) -> int:
        """Convert one input and return the signed code.

        Args:
            channel: Input 0-3 single-ended. With ``differential`` it selects the pair (0: A0-A1, 1: A0-A3, 2: A1-A3, 3: A2-A3).
            pga: Full-scale range in volts: 6.144, 4.096, 2.048, 1.024, 0.512 or 0.256. Default 4.096.
            data_rate: Samples per second: 8, 16, 32, 64, 128, 250, 475 or 860. Default 128.
            continuous: True for continuous conversion mode. Default False (single-shot).
            differential: True to measure a differential pair. Default False.

        Returns:
            Signed 16-bit conversion code.

        Raises:
            ValueError: ``channel``, ``pga`` or ``data_rate`` is not supported.

        Example:
            code = adc.read_raw(channel=1, pga=2.048)
        """
    def read_voltage(self, channel: int = 0, pga: float = 4.096, data_rate: int = 128,
                     continuous: bool = False, differential: bool = False) -> float:
        """Convert one input and return volts (the arguments are as for :meth:`read_raw`).

        Args:
            channel: Input 0-3.
            pga: Full-scale range in volts. Default 4.096.
            data_rate: Samples per second. Default 128.
            continuous: Continuous mode. Default False.
            differential: Differential pair. Default False.

        Returns:
            Voltage in volts.

        Example:
            volts = adc.read_voltage(channel=0, pga=4.096, data_rate=250)
        """

class MCP23017:
    """Microchip MCP23017 16-bit I2C GPIO expander (pins 0-7 = port A, 8-15 = port B).

    All pins start as inputs.

    Example:
        gpio = bb_devices.MCP23017(bus)
        gpio.pin_mode(0, output=True)
        gpio.set_pin(0, 1)
    """
    def __init__(self, i2c: bugbuster.I2C, addr: int = 0x20) -> None:
        """Configure the part and set every pin to input.

        Args:
            i2c: A ``bugbuster.I2C`` bus.
            addr: 7-bit address, 0x20-0x27. Default 0x20.

        Example:
            gpio = bb_devices.MCP23017(bus)
        """
    def write_reg(self, reg: int, value: int) -> None:
        """Write one register.

        Args:
            reg: Register address.
            value: Byte value, 0-255.

        Example:
            gpio.write_reg(gpio.REG_OLATA, 0x01)
        """
    def read_reg(self, reg: int) -> int:
        """Read one register.

        Args:
            reg: Register address.

        Returns:
            The byte read.

        Example:
            value = gpio.read_reg(gpio.REG_GPIOA)
        """
    def set_direction(self, direction_mask: int) -> None:
        """Set all 16 pin directions.

        Args:
            direction_mask: 16-bit mask, bit n = pin n; 1 = input, 0 = output.

        Example:
            gpio.set_direction(0xFF00)  # port A outputs, port B inputs
        """
    def set_dir(self, pin: int, output: bool = True) -> None:
        """Set one pin's direction.

        Args:
            pin: Pin 0-15.
            output: True for output (default), False for input.

        Raises:
            ValueError: ``pin`` is outside 0-15.

        Example:
            gpio.set_dir(3, output=True)
        """
    def set_pullups(self, pullup_mask: int) -> None:
        """Set all 16 pull-ups.

        Args:
            pullup_mask: 16-bit mask, bit n = pin n; 1 enables the 100 kOhm pull-up.

        Example:
            gpio.set_pullups(0xFF00)
        """
    def set_pullup(self, pin: int, enable: bool = True) -> None:
        """Enable or disable one pin's pull-up.

        Args:
            pin: Pin 0-15.
            enable: True to enable (default).

        Raises:
            ValueError: ``pin`` is outside 0-15.

        Example:
            gpio.set_pullup(9, True)
        """
    def write_port(self, value: int) -> None:
        """Write all 16 output levels.

        Args:
            value: 16-bit mask, bit n = pin n.

        Example:
            gpio.write_port(0x00FF)
        """
    def read_port(self) -> int:
        """Read all 16 pin levels.

        Returns:
            A 16-bit mask, bit n = pin n.

        Example:
            levels = gpio.read_port()
        """
    def set_pin(self, pin: int, value: int) -> None:
        """Set one output pin, leaving the others unchanged.

        Args:
            pin: Pin 0-15.
            value: 0 or 1.

        Raises:
            ValueError: ``pin`` is outside 0-15.

        Example:
            gpio.set_pin(0, 1)
        """
    def read_pin(self, pin: int) -> int:
        """Read one pin.

        Args:
            pin: Pin 0-15.

        Returns:
            0 or 1.

        Raises:
            ValueError: ``pin`` is outside 0-15.

        Example:
            pressed = gpio.read_pin(8) == 0
        """
    def pin_mode(self, pin: int, output: bool = True, pullup: bool = False) -> None:
        """Set direction and pull-up of one pin in one call.

        Args:
            pin: Pin 0-15.
            output: True for output (default), False for input.
            pullup: True enables the pull-up. Default False.

        Raises:
            ValueError: ``pin`` is outside 0-15.

        Example:
            gpio.pin_mode(8, output=False, pullup=True)
        """

class PCF8574:
    """NXP PCF8574 8-bit quasi-bidirectional I2C I/O expander.

    A pin reads high unless pulled low externally; write 1 to a pin to use it as an input.

    Example:
        io = bb_devices.PCF8574(bus)
        io.set_pin(0, 0)
    """
    def __init__(self, i2c: bugbuster.I2C, addr: int = 0x20, value: int = 0xFF) -> None:
        """Attach and write the initial output byte.

        Args:
            i2c: A ``bugbuster.I2C`` bus.
            addr: 7-bit address, 0x20-0x27 (0x38-0x3F for the PCF8574A). Default 0x20.
            value: Initial output byte; 0xFF (default) leaves every pin high/input.

        Example:
            io = bb_devices.PCF8574(bus, addr=0x20)
        """
    def write(self, value: int) -> None:
        """Write all 8 pins.

        Args:
            value: Byte, bit n = pin n.

        Example:
            io.write(0xFE)  # pin 0 low, the rest high
        """
    def read(self) -> int:
        """Read all 8 pins.

        Returns:
            A byte, bit n = pin n.

        Example:
            levels = io.read()
        """
    def set_pin(self, pin: int, value: int) -> None:
        """Set one pin, leaving the others unchanged (reads the port first).

        Args:
            pin: Pin 0-7.
            value: 0 or 1.

        Raises:
            ValueError: ``pin`` is outside 0-7.

        Example:
            io.set_pin(2, 0)
        """
    def read_pin(self, pin: int) -> int:
        """Read one pin.

        Args:
            pin: Pin 0-7.

        Returns:
            0 or 1.

        Raises:
            ValueError: ``pin`` is outside 0-7.

        Example:
            closed = io.read_pin(7) == 0
        """

class PCA9685:
    """NXP PCA9685 16-channel 12-bit PWM controller.

    Starts at 50 Hz (servo timing).

    Example:
        pwm = bb_devices.PCA9685(bus)
        pwm.set_pwm_freq(1000)
        pwm.set_duty(0, 0.25)
    """
    def __init__(self, i2c: bugbuster.I2C, addr: int = 0x40, osc_hz: int = 25000000) -> None:
        """Initialise the part and set 50 Hz.

        Args:
            i2c: A ``bugbuster.I2C`` bus.
            addr: 7-bit address. Default 0x40.
            osc_hz: Oscillator frequency in Hz. Default 25000000.

        Example:
            pwm = bb_devices.PCA9685(bus)
        """
    def write_reg(self, reg: int, value: int) -> None:
        """Write one register.

        Args:
            reg: Register address.
            value: Byte value, 0-255.

        Example:
            pwm.write_reg(pwm.REG_MODE2, 0x04)
        """
    def read_reg(self, reg: int) -> int:
        """Read one register.

        Args:
            reg: Register address.

        Returns:
            The byte read.

        Example:
            mode = pwm.read_reg(pwm.REG_MODE1)
        """
    def set_pwm_freq(self, freq_hz: float) -> int:
        """Set the PWM frequency for all channels.

        Args:
            freq_hz: Frequency in hertz, greater than 0 (about 24-1526 Hz is reachable).

        Returns:
            The prescale value written.

        Raises:
            ValueError: ``freq_hz`` is not positive.

        Example:
            prescale = pwm.set_pwm_freq(1000)
        """
    def set_pwm(self, channel: int, on: int = 0, off: int = 0) -> None:
        """Set a channel's raw on/off counts.

        Args:
            channel: Channel 0-15.
            on: Count (0-4095) at which the output turns on. Default 0.
            off: Count (0-4095) at which it turns off. Default 0.

        Raises:
            ValueError: ``channel`` is outside 0-15.

        Example:
            pwm.set_pwm(0, 0, 2048)  # 50 % duty
        """
    def set_duty(self, channel: int, duty: float) -> None:
        """Set a channel's duty cycle.

        Args:
            channel: Channel 0-15.
            duty: Duty cycle 0.0-1.0 (clamped).

        Raises:
            ValueError: ``channel`` is outside 0-15.

        Example:
            pwm.set_duty(0, 0.25)
        """

class TCS34725:
    """ams OSRAM TCS34725 RGBC colour sensor.

    Example:
        color = bb_devices.TCS34725(bus, integration_ms=50, gain=4)
        red, green, blue, clear = color.read_rgbc()
    """
    def __init__(self, i2c: bugbuster.I2C, addr: int = 0x29, integration_ms: int = 50, gain: int = 4) -> None:
        """Check the id, set integration time and gain, and enable the sensor.

        Args:
            i2c: A ``bugbuster.I2C`` bus.
            addr: 7-bit address. Default 0x29.
            integration_ms: Integration time in milliseconds, about 2.4-614. Default 50.
            gain: 1, 4, 16 or 60. Default 4.

        Raises:
            OSError: the chip id is not 0x44 or 0x4D (no TCS34725).
            ValueError: ``gain`` is not 1, 4, 16 or 60.

        Example:
            color = bb_devices.TCS34725(bus, integration_ms=100, gain=16)
        """
    def write_u8(self, reg: int, value: int) -> None:
        """Write one register.

        Args:
            reg: Register address.
            value: Byte value, 0-255.

        Example:
            color.write_u8(color.REG_CONTROL, 0x01)
        """
    def read_u8(self, reg: int) -> int:
        """Read one register.

        Args:
            reg: Register address.

        Returns:
            The byte read.

        Example:
            chip_id = color.read_u8(color.REG_ID)
        """
    def read_words(self, reg: int, count: int = 4) -> list[int]:
        """Read consecutive 16-bit little-endian registers.

        Args:
            reg: First register address.
            count: Number of words. Default 4.

        Returns:
            A list of unsigned 16-bit integers.

        Example:
            clear, red, green, blue = color.read_words(color.REG_CDATAL, 4)
        """
    def enable(self) -> None:
        """Power on the sensor and start the RGBC converter.

        Example:
            color.enable()
        """
    def disable(self) -> None:
        """Power the sensor down.

        Example:
            color.disable()
        """
    def set_integration_time(self, integration_ms: int) -> int:
        """Set the integration time.

        Args:
            integration_ms: Milliseconds, about 2.4-614.

        Returns:
            The ATIME register value written.

        Example:
            color.set_integration_time(100)
        """
    def set_gain(self, gain: int) -> int:
        """Set the analog gain.

        Args:
            gain: 1, 4, 16 or 60.

        Returns:
            The gain.

        Raises:
            ValueError: ``gain`` is not 1, 4, 16 or 60.

        Example:
            color.set_gain(16)
        """
    def read_rgbc(self) -> tuple[int, int, int, int]:
        """Wait one integration time and read the four channels.

        Returns:
            ``(red, green, blue, clear)`` raw 16-bit counts.

        Example:
            red, green, blue, clear = color.read_rgbc()
        """

class MPU6050:
    """InvenSense MPU6050 6-axis accelerometer and gyroscope.

    Example:
        imu = bb_devices.MPU6050(bus)
        ax, ay, az = imu.read_accel()
    """
    def __init__(self, i2c: bugbuster.I2C, addr: int = 0x68, accel_range: int = 2, gyro_range: int = 250) -> None:
        """Check the id, wake the part and set the ranges.

        Args:
            i2c: A ``bugbuster.I2C`` bus.
            addr: 7-bit address, 0x68 or 0x69. Default 0x68.
            accel_range: Full scale in g: 2, 4, 8 or 16. Default 2.
            gyro_range: Full scale in degrees per second: 250, 500, 1000 or 2000. Default 250.

        Raises:
            OSError: WHO_AM_I does not match (no MPU6050).
            ValueError: a range is not one of the listed values.

        Example:
            imu = bb_devices.MPU6050(bus, accel_range=4, gyro_range=500)
        """
    def read_u8(self, reg: int) -> int:
        """Read one register.

        Args:
            reg: Register address.

        Returns:
            The byte read.

        Example:
            who_am_i = imu.read_u8(imu.REG_WHO_AM_I)
        """
    def write_u8(self, reg: int, value: int) -> None:
        """Write one register.

        Args:
            reg: Register address.
            value: Byte value, 0-255.

        Example:
            imu.write_u8(imu.REG_SMPLRT_DIV, 7)
        """
    def wake(self) -> None:
        """Take the part out of sleep (10 ms wait).

        Example:
            imu.wake()
        """
    def set_ranges(self, accel_range: int = 2, gyro_range: int = 250) -> tuple[int, int]:
        """Set the full-scale ranges.

        Args:
            accel_range: g: 2, 4, 8 or 16. Default 2.
            gyro_range: deg/s: 250, 500, 1000 or 2000. Default 250.

        Returns:
            ``(accel_range, gyro_range)``.

        Raises:
            ValueError: a range is not one of the listed values.

        Example:
            imu.set_ranges(accel_range=8, gyro_range=1000)
        """
    def read_temperature(self) -> float:
        """Read the die temperature.

        Returns:
            Degrees Celsius.

        Example:
            print("%.1f C" % imu.read_temperature())
        """
    def read_accel(self) -> tuple[float, float, float]:
        """Read acceleration.

        Returns:
            ``(ax, ay, az)`` in g.

        Example:
            ax, ay, az = imu.read_accel()
        """
    def read_gyro(self) -> tuple[float, float, float]:
        """Read angular rate.

        Returns:
            ``(gx, gy, gz)`` in degrees per second.

        Example:
            gx, gy, gz = imu.read_gyro()
        """
    def read(self) -> tuple[tuple[float, float, float], tuple[float, float, float], float]:
        """Read everything.

        Returns:
            ``((ax, ay, az), (gx, gy, gz), temperature_celsius)``.

        Example:
            (ax, ay, az), (gx, gy, gz), temp_c = imu.read()
        """

class MCP4725:
    """Microchip MCP4725 12-bit I2C DAC.

    Example:
        dac = bb_devices.MCP4725(bus, vref=3.3)
        dac.set_voltage(1.65)
    """
    def __init__(self, i2c: bugbuster.I2C, addr: int = 0x60, vref: float = 3.3) -> None:
        """Attach to the DAC. No bus traffic until you set an output.

        Args:
            i2c: A ``bugbuster.I2C`` bus.
            addr: 7-bit address, 0x60-0x67. Default 0x60.
            vref: Supply/reference voltage in volts. Default 3.3.

        Example:
            dac = bb_devices.MCP4725(bus, vref=3.3)
        """
    def set_raw(self, code: int) -> int:
        """Write a raw code to the output register.

        Args:
            code: 0-4095 (masked to 12 bits).

        Returns:
            The code written.

        Example:
            dac.set_raw(2048)  # half scale
        """
    def set_voltage(self, voltage: float, vref: float | None = None) -> int:
        """Output a voltage.

        Args:
            voltage: Volts, clamped to 0 up to ``vref``.
            vref: Reference in volts; None uses the constructor value.

        Returns:
            The code written.

        Example:
            dac.set_voltage(1.2)
        """
    def sleep(self) -> None:
        """Put the DAC into power-down (output pulled down).

        Example:
            dac.sleep()
        """

class SSD1306_I2C:
    """SSD1306 OLED display over I2C (the MicroPython ``ssd1306`` driver, frozen into the firmware).

    A ``framebuf`` display: draw with ``fill``, ``text``, ``pixel``, ``hline`` and
    friends, then call ``show()``. Without the ``ssd1306`` module the constructor raises ``OSError``.

    Example:
        oled = bb_devices.SSD1306_I2C(128, 64, bus)
        oled.fill(0)
        oled.text("BugBuster", 0, 0)
        oled.show()
    """
    def __init__(self, width: int, height: int, i2c: bugbuster.I2C, addr: int = 0x3C,
                 external_vcc: bool = False) -> None:
        """Initialise the display.

        Args:
            width: Pixels, usually 128.
            height: Pixels, 32 or 64.
            i2c: A bus object with the I2C ``writeto`` method.
            addr: 7-bit address. Default 0x3C.
            external_vcc: True when the panel has its own supply. Default False.

        Example:
            oled = bb_devices.SSD1306_I2C(128, 64, bus)
        """
    def fill(self, col: int) -> None:
        """Fill the frame buffer.

        Args:
            col: 0 for black, 1 for white.

        Example:
            oled.fill(0)
            oled.show()
        """
    def text(self, s: str, x: int, y: int, col: int = 1) -> None:
        """Draw 8x8 text into the frame buffer.

        Args:
            s: Text to draw.
            x: Left pixel.
            y: Top pixel.
            col: 0 or 1. Default 1.

        Example:
            oled.text("Hello", 0, 0)
            oled.show()
        """
    def pixel(self, x: int, y: int, col: int = 1) -> None:
        """Set one pixel in the frame buffer.

        Args:
            x: Column.
            y: Row.
            col: 0 or 1. Default 1.

        Example:
            oled.pixel(10, 10, 1)
            oled.show()
        """
    def show(self) -> None:
        """Send the frame buffer to the display.

        Example:
            oled.show()
        """

class SSD1306_SPI:
    """SSD1306 OLED display over SPI (the MicroPython ``ssd1306`` driver).

    Takes ``(width, height, spi, dc, res, cs)``; the pin arguments are
    ``machine.Pin`` objects. It has the same drawing methods as :class:`SSD1306_I2C`.
    Without the ``ssd1306`` module the constructor raises ``OSError``.

    Example:
        oled = bb_devices.SSD1306_SPI(128, 64, spi, dc, res, cs)
        oled.text("Hello", 0, 0)
        oled.show()
    """
    def __init__(self, width: int, height: int, spi, dc, res, cs, external_vcc: bool = False) -> None:
        """Initialise the display.

        Args:
            width: Pixels, usually 128.
            height: Pixels, 32 or 64.
            spi: SPI bus object (``machine.SoftSPI``).
            dc: Data/command ``machine.Pin``.
            res: Reset ``machine.Pin``.
            cs: Chip-select ``machine.Pin``.
            external_vcc: True when the panel has its own supply. Default False.

        Example:
            oled = bb_devices.SSD1306_SPI(128, 64, spi, dc, res, cs)
        """
    def fill(self, col: int) -> None:
        """Fill the frame buffer.

        Args:
            col: 0 for black, 1 for white.

        Example:
            oled.fill(0)
            oled.show()
        """
    def text(self, s: str, x: int, y: int, col: int = 1) -> None:
        """Draw 8x8 text into the frame buffer.

        Args:
            s: Text to draw.
            x: Left pixel.
            y: Top pixel.
            col: 0 or 1. Default 1.

        Example:
            oled.text("Hello", 0, 0)
            oled.show()
        """
    def show(self) -> None:
        """Send the frame buffer to the display.

        Example:
            oled.show()
        """

class SSD1306(SSD1306_I2C):
    """Alias of :class:`SSD1306_I2C`.

    Example:
        oled = bb_devices.SSD1306(128, 64, bus)
    """
