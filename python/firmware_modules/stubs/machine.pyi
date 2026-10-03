"""MicroPython ``machine`` on BugBuster: ESP32-S3 GPIO pins, software I2C/SPI
and chip control. Prefer ``bugbuster.I2C``/``SPI`` for the IO terminals."""

def reset() -> None:
    """Reboot the board."""

def reset_cause() -> int:
    """``esp_reset_reason()`` code."""

def unique_id() -> bytes:
    """Wi-Fi STA MAC (6 bytes)."""

def freq() -> int:
    """CPU frequency in Hz (read-only)."""

def idle() -> None:
    """Yield the CPU."""

class Pin:
    """ESP32-S3 GPIO."""
    IN: int
    OUT: int
    OPEN_DRAIN: int
    PULL_UP: int
    PULL_DOWN: int
    def __init__(self, id: int, mode: int | None = None, pull: int | None = None, *,
                 value: int | None = None) -> None: ...
    def init(self, mode: int | None = None, pull: int | None = None, *, value: int | None = None) -> None:
        """Reconfigure the pin."""
    def value(self, x: int | None = None) -> int | None:
        """Read (no argument) or drive the pin."""
    def on(self) -> None:
        """Drive high."""
    def off(self) -> None:
        """Drive low."""

class SoftI2C:
    """Bit-banged I2C on ESP GPIOs."""
    def __init__(self, scl: Pin, sda: Pin, *, freq: int = 400000) -> None: ...
    def scan(self) -> list[int]: ...
    def readfrom(self, addr: int, nbytes: int) -> bytes: ...
    def writeto(self, addr: int, buf: bytes) -> int: ...

class I2C(SoftI2C):
    """Alias of :class:`SoftI2C` on this port."""

class SoftSPI:
    """Bit-banged SPI on ESP GPIOs."""
    def __init__(self, *, baudrate: int = 500000, polarity: int = 0, phase: int = 0,
                 sck: Pin | None = None, mosi: Pin | None = None, miso: Pin | None = None) -> None: ...
    def read(self, nbytes: int) -> bytes: ...
    def write(self, buf: bytes) -> None: ...
    def write_readinto(self, write_buf: bytes, read_buf: bytearray) -> None: ...

class SPI(SoftSPI):
    """Alias of :class:`SoftSPI` on this port."""
