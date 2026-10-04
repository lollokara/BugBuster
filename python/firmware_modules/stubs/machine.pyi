"""MicroPython ``machine`` on BugBuster: ESP32-S3 GPIO pins, bit-banged I2C/SPI,
watchdog and chip control.

This is a trimmed port: there is no hardware I2C/SPI/UART/PWM/ADC/Timer/RTC.
Pin numbers are raw ESP32-S3 GPIO numbers, NOT the 1-12 IO terminals used by
``bugbuster``. Many GPIOs are wired to the board's own chips, so driving an
arbitrary pin can disturb the instrument. Prefer ``bugbuster.I2C`` and
``bugbuster.SPI`` for the front-panel IO terminals.

Example:
    import machine
    print(machine.unique_id().hex(), machine.freq() // 1000000, "MHz")
"""

def reset() -> None:
    """Hard-reboot the board (the ESP32 restarts).

    Safety:
        Stops everything, including the script and open connections. Outputs
        return to their power-on state.

    Example:
        machine.reset()
    """

def soft_reset() -> None:
    """End the script (an alias of ``sys.exit``).

    Example:
        machine.soft_reset()
    """

def reset_cause() -> int:
    """Return why the chip last reset (the ESP-IDF ``esp_reset_reason()`` code).

    Returns:
        1 power-on, 2 external pin, 3 software, 4 panic, 5 interrupt watchdog,
        6 task watchdog, 7 other watchdog, 8 deep sleep wake, 9 brown-out,
        10 SDIO; 0 when unknown.

    Example:
        if machine.reset_cause() == 9:
            print("brown-out")
    """

def unique_id() -> bytes:
    """Return the Wi-Fi station MAC address as 6 bytes, unique per board.

    Returns:
        6 bytes.

    Example:
        print(machine.unique_id().hex())
    """

def freq() -> int:
    """Return the CPU frequency in Hz. Read-only: ``freq(hz)`` raises ``NotImplementedError``.

    Returns:
        Frequency in hertz.

    Example:
        print(machine.freq() // 1000000, "MHz")
    """

def idle() -> None:
    """Yield the CPU to other tasks for one scheduler tick.

    Example:
        machine.idle()
    """

def lightsleep(time_ms: int | None = None) -> None:
    """Put the chip into light sleep.

    Args:
        time_ms: Wake after this many milliseconds. None sleeps with no wake source configured.

    Safety:
        Suspends the CPU, including Wi-Fi and the script link. Use short timeouts.

    Example:
        machine.lightsleep(100)
    """

def deepsleep(time_ms: int | None = None) -> None:
    """Put the chip into deep sleep; the board restarts on wake.

    Args:
        time_ms: Wake after this many milliseconds. None sleeps with no wake source configured.

    Safety:
        Does not return. Powers down the CPU and drops all connections; the board
        reboots when it wakes.

    Example:
        machine.deepsleep(60000)  # reboots after 60 s
    """

class Pin:
    """One ESP32-S3 GPIO (raw GPIO number, 0-48).

    Calling the object reads or sets the level: ``pin()`` and ``pin(1)``. There is
    no interrupt, toggle or alternate-function support in this port.

    Example:
        button = machine.Pin(39, machine.Pin.IN, machine.Pin.PULL_UP)
        print(button.value())
    """
    IN: int = 1
    """Input mode (ESP-IDF GPIO_MODE_INPUT)."""
    OUT: int = 3
    """Output mode that can also be read back (GPIO_MODE_INPUT_OUTPUT)."""
    OPEN_DRAIN: int = 7
    """Open-drain output, readable (GPIO_MODE_INPUT_OUTPUT_OD)."""
    PULL_UP: int = 1
    """Enable the internal pull-up (disables the pull-down)."""
    PULL_DOWN: int = 2
    """Enable the internal pull-down (disables the pull-up)."""
    def __init__(self, id: int, mode: int | None = None, pull: int | None = None, *,
                 value: int | None = None) -> None:
        """Select a GPIO and optionally configure it.

        Args:
            id: GPIO number, 0-48.
            mode: ``Pin.IN``, ``Pin.OUT`` or ``Pin.OPEN_DRAIN``; None leaves the direction unchanged.
            pull: ``Pin.PULL_UP`` or ``Pin.PULL_DOWN``; 0 disables both; None leaves it unchanged.
            value: Initial output level, applied before the mode is set.

        Raises:
            ValueError: ``id`` is outside 0-48.

        Safety:
            Configuring a pin as an output drives it. GPIOs used by the board's own
            peripherals must not be reconfigured.

        Example:
            pin = machine.Pin(39, machine.Pin.IN, machine.Pin.PULL_UP)
        """
    def init(self, mode: int | None = None, pull: int | None = None, *, value: int | None = None) -> None:
        """Reconfigure the pin; the arguments are as for the constructor.

        Args:
            mode: ``Pin.IN``, ``Pin.OUT`` or ``Pin.OPEN_DRAIN``; None leaves it unchanged.
            pull: ``Pin.PULL_UP``, ``Pin.PULL_DOWN``, 0 for none; None leaves it unchanged.
            value: Output level to set first.

        Example:
            pin.init(machine.Pin.IN, machine.Pin.PULL_UP)
        """
    def value(self, x: int | None = None) -> int | None:
        """Read the level, or drive it when ``x`` is given.

        Args:
            x: 0 or 1 to set the output level; omit to read.

        Returns:
            The level 0 or 1 when reading; None when setting.

        Example:
            if button.value() == 0:
                print("pressed")
        """
    def on(self) -> None:
        """Drive the pin high.

        Example:
            pin.on()
        """
    def off(self) -> None:
        """Drive the pin low.

        Example:
            pin.off()
        """

class SoftI2C:
    """Bit-banged I2C master on two ESP GPIOs.

    For the front-panel IO terminals use ``bugbuster.I2C`` instead.

    Example:
        # Pin numbers are examples: use GPIOs that are free on your wiring.
        i2c = machine.SoftI2C(scl=machine.Pin(9), sda=machine.Pin(8), freq=100000)
        print(i2c.scan())
    """
    def __init__(self, scl: Pin, sda: Pin, *, freq: int = 400000, timeout: int = 255) -> None:
        """Create the bus on two pins.

        Args:
            scl: Clock pin.
            sda: Data pin.
            freq: Clock in Hz. Default 400000.
            timeout: Clock-stretch timeout in microseconds. Default 255.

        Example:
            i2c = machine.SoftI2C(machine.Pin(9), machine.Pin(8), freq=100000)
        """
    def scan(self) -> list[int]:
        """Scan 0x08-0x77 and return the addresses that ACK.

        Returns:
            List of 7-bit addresses.

        Example:
            print(i2c.scan())
        """
    def readfrom(self, addr: int, nbytes: int, stop: bool = True) -> bytes:
        """Read ``nbytes`` from the device at ``addr``.

        Args:
            addr: 7-bit address.
            nbytes: Byte count.
            stop: Send a stop condition afterwards. Default True.

        Returns:
            The bytes read.

        Raises:
            OSError: the device does not answer.

        Example:
            data = i2c.readfrom(0x48, 2)
        """
    def readfrom_into(self, addr: int, buf: bytearray, stop: bool = True) -> None:
        """Read into ``buf`` (its length sets the byte count).

        Args:
            addr: 7-bit address.
            buf: Destination buffer.
            stop: Send a stop condition afterwards. Default True.

        Raises:
            OSError: the device does not answer.

        Example:
            buf = bytearray(2)
            i2c.readfrom_into(0x48, buf)
        """
    def writeto(self, addr: int, buf: bytes, stop: bool = True) -> int:
        """Write ``buf`` to the device at ``addr``.

        Args:
            addr: 7-bit address.
            buf: Bytes to send.
            stop: Send a stop condition afterwards. Default True.

        Returns:
            The number of ACKs received.

        Raises:
            OSError: the device does not answer.

        Example:
            i2c.writeto(0x48, b"\x01")
        """
    def readfrom_mem(self, addr: int, memaddr: int, nbytes: int, *, addrsize: int = 8) -> bytes:
        """Read ``nbytes`` starting at register ``memaddr``.

        Args:
            addr: 7-bit address.
            memaddr: Register or memory address.
            nbytes: Byte count.
            addrsize: Width of ``memaddr`` in bits. Default 8.

        Returns:
            The bytes read.

        Example:
            chip_id = i2c.readfrom_mem(0x76, 0xD0, 1)
        """
    def writeto_mem(self, addr: int, memaddr: int, buf: bytes, *, addrsize: int = 8) -> None:
        """Write ``buf`` starting at register ``memaddr``.

        Args:
            addr: 7-bit address.
            memaddr: Register or memory address.
            buf: Bytes to write.
            addrsize: Width of ``memaddr`` in bits. Default 8.

        Example:
            i2c.writeto_mem(0x48, 0x01, b"\x60")
        """

class I2C(SoftI2C):
    """Alias of :class:`SoftI2C` on this port (there is no hardware I2C).

    Example:
        i2c = machine.I2C(machine.Pin(9), machine.Pin(8))
    """

class SoftSPI:
    """Bit-banged SPI master on ESP GPIOs.

    For the front-panel IO terminals use ``bugbuster.SPI`` instead.

    Example:
        spi = machine.SoftSPI(baudrate=1000000, sck=machine.Pin(12),
                              mosi=machine.Pin(11), miso=machine.Pin(13))
    """
    MSB: int = 0
    """Most significant bit first."""
    LSB: int = 1
    """Least significant bit first."""
    def __init__(self, *, baudrate: int = 500000, polarity: int = 0, phase: int = 0,
                 sck: Pin | None = None, mosi: Pin | None = None, miso: Pin | None = None) -> None:
        """Create the bus.

        Args:
            baudrate: Clock in Hz. Default 500000.
            polarity: Idle clock level, 0 or 1. Default 0.
            phase: Sampling edge, 0 (first) or 1 (second). Default 0.
            sck: Clock pin.
            mosi: Data-out pin.
            miso: Data-in pin.

        Example:
            spi = machine.SoftSPI(baudrate=1000000, sck=machine.Pin(12), mosi=machine.Pin(11), miso=machine.Pin(13))
        """
    def read(self, nbytes: int, write: int = 0x00) -> bytes:
        """Read ``nbytes`` while sending the byte ``write`` repeatedly.

        Args:
            nbytes: Byte count.
            write: Filler byte to send. Default 0.

        Returns:
            The bytes read.

        Example:
            data = spi.read(4)
        """
    def write(self, buf: bytes) -> None:
        """Send ``buf`` and ignore the reply.

        Args:
            buf: Bytes to send.

        Example:
            spi.write(b"\x9f")
        """
    def write_readinto(self, write_buf: bytes, read_buf: bytearray) -> None:
        """Send ``write_buf`` while reading into ``read_buf`` (same length).

        Args:
            write_buf: Bytes to send.
            read_buf: Buffer that receives the reply.

        Example:
            rx = bytearray(4)
            spi.write_readinto(b"\x9f\x00\x00\x00", rx)
        """

class SPI(SoftSPI):
    """Alias of :class:`SoftSPI` on this port (there is no hardware SPI).

    Example:
        spi = machine.SPI(baudrate=1000000, sck=machine.Pin(12), mosi=machine.Pin(11))
    """

class WDT:
    """Watchdog timer: reboots the board when it is not fed in time.

    Example:
        wdt = machine.WDT(timeout=5000)
        while True:
            wdt.feed()
            bugbuster.sleep(1000)
    """
    def __init__(self, id: int = 0, timeout: int = 5000) -> None:
        """Start the watchdog.

        Args:
            id: Watchdog number, 0.
            timeout: Timeout in milliseconds. Default 5000.

        Safety:
            A script that stops feeding the watchdog reboots the board.

        Example:
            wdt = machine.WDT(timeout=8000)
        """
    def feed(self) -> None:
        """Restart the timeout. Call it more often than ``timeout``.

        Example:
            wdt.feed()
        """
