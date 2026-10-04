"""BugBuster board control from on-device MicroPython scripts: analog/digital
channels, the external I2C/SPI buses, HTTP/MQTT, IO ownership and the HAT.

    import bugbuster
    ch = bugbuster.Channel(0)
    ch.set_function(bugbuster.FUNC_VOUT)
    ch.set_voltage(2.5)
"""

FUNC_HIGH_IMP: int
FUNC_VOUT: int
FUNC_IOUT: int
FUNC_VIN: int
FUNC_IIN_EXT_PWR: int
FUNC_IIN_LOOP_PWR: int
FUNC_RES_MEAS: int
FUNC_DIN_LOGIC: int
FUNC_DIN_LOOP: int
FUNC_IOUT_HART: int
FUNC_IIN_EXT_PWR_HART: int
FUNC_IIN_LOOP_PWR_HART: int
HAT_RAIL_3V3_ADJ: int
HAT_RAIL_VADJ3: int
HAT_RAIL_VADJ4: int
HAT_LED_OFF: int
HAT_LED_RED: int
HAT_LED_GREEN: int
HAT_LED_BLUE: int
HAT_LED_YELLOW: int
HAT_LED_CYAN: int
HAT_LED_MAGENTA: int
HAT_LED_WHITE: int

def sleep(ms: int) -> None:
    """Sleep ``ms`` milliseconds; a stop request interrupts it."""

def log(level: str, msg: str) -> None:
    """Write one structured line to the script log ring with ``level`` ('E', 'W', 'I', 'D') and src 'mpy'."""

def ticks_ms() -> int:
    """Return milliseconds since boot (matching the script log timestamps)."""

def vadj_pd_warning(rail: int, voltage: float) -> str | None:
    """USB-PD headroom warning for VADJ ``rail`` (1-2) at ``voltage``, or None."""

def rail_power_up(rail: int, volts: float, efuse_mask: int) -> dict:
    """Bring VADJ ``rail`` (1-2) to ``volts`` (3-12 V) behind e-fuses
    ``efuse_mask`` (1-3). Returns ``{"pg": bool, "fault": bool}``."""

def efuse_set(efuse: int, on: bool) -> None:
    """Switch e-fuse 1-4."""

class Channel:
    """One AD74416H channel (0-3)."""
    def __init__(self, channel: int) -> None: ...
    def set_function(self, func: int) -> None:
        """Select a ``FUNC_*`` channel function."""
    def set_voltage(self, voltage: float, bipolar: bool = False) -> None:
        """Drive the DAC output (VOUT)."""
    def read_voltage(self) -> float:
        """Read the channel ADC in volts."""
    def set_do(self, value: bool) -> None:
        """Set the channel's digital output."""

class I2C:
    """External I2C master on two IO terminals."""
    def __init__(self, sda_io: int, scl_io: int, *, freq: int = 400000, pullups: str = "external",
                 supply: float | None = None, vlogic: float | None = None,
                 allow_split_supplies: bool = False) -> None: ...
    def close(self) -> None:
        """Release the bus and its IO terminals."""
    def scan(self, start: int = 0x08, stop: int = 0x77, skip_reserved: bool = True,
             timeout_ms: int = 50) -> list[int]:
        """Addresses that ACK."""
    def writeto(self, addr: int, buf: bytes, timeout_ms: int = 50) -> None:
        """Write ``buf`` to ``addr``."""
    def readfrom(self, addr: int, n: int, timeout_ms: int = 50) -> bytes:
        """Read ``n`` bytes from ``addr``."""
    def writeto_then_readfrom(self, addr: int, buf: bytes, rd_n: int, timeout_ms: int = 50) -> bytes:
        """Write then repeated-start read ``rd_n`` bytes."""

class SPI:
    """External SPI master on IO terminals."""
    def __init__(self, sck_io: int, *, mosi_io: int | None = None, miso_io: int | None = None,
                 cs_io: int | None = None, freq: int = 1000000, mode: int = 0,
                 supply: float | None = None, vlogic: float | None = None,
                 allow_split_supplies: bool = False) -> None: ...
    def transfer(self, buf: bytes) -> bytes:
        """Full-duplex transfer; returns the bytes clocked in."""
    def close(self) -> None:
        """Release the bus and its IO terminals."""

class Claim:
    """Context manager returned by :func:`claim`; releases on exit."""
    def __enter__(self) -> "Claim": ...
    def __exit__(self, exc_type, exc, tb) -> None: ...

def http_get(url: str, *, headers: dict | None = None, timeout_ms: int = 10000) -> dict:
    """HTTP(S) GET. Returns ``{"status", "body", ...}``."""

def http_post(url: str, body: str | bytes | None = None, *, headers: dict | None = None,
              timeout_ms: int = 10000) -> dict:
    """HTTP(S) POST."""

def mqtt_publish(topic: str, payload: str | bytes, host: str, *, port: int = 1883,
                 username: str | None = None, password: str | None = None) -> None:
    """Publish one MQTT message."""

def claim(slots: list[int], *, purpose: str | None = None, lease_ms: int = 0) -> Claim:
    """Claim IO slots for this script (other owners are refused)."""

def release(*, slots: list[int] | None = None) -> None:
    """Release claimed IO slots (all when ``slots`` is None)."""

def owner_status() -> list[dict]:
    """Per-slot IO ownership."""

def hat_status() -> dict:
    """HAT detection, link and firmware state."""

def hat_caps() -> dict:
    """HAT capabilities."""

def hat_rails() -> list[dict]:
    """HAT rail states."""

def hat_set_rail_enable(rail: int, enable: bool) -> None:
    """Enable/disable a ``HAT_RAIL_*``."""

def hat_set_rail_voltage(rail: int, mv: int) -> None:
    """Set a ``HAT_RAIL_*`` voltage in mV."""

def hat_led(led: int, color: int) -> None:
    """Set a HAT LED to a ``HAT_LED_*`` colour."""

def hat_io_bank(dirs: int, *, ups: int = 0, dns: int = 0, vals: int = 0) -> dict:
    """Configure the HAT IO bank (bit masks)."""

def hat_level_shift(oe: bool, dir: int) -> None:
    """HAT level-shifter output enable and direction."""

def hat_calibrate_start(rail: int) -> None:
    """Start calibrating a HAT rail."""

def hat_calibrate_status() -> dict:
    """Calibration progress/result."""

def hat_calibrate_import(rail: int, points: list) -> None:
    """Import calibration points for a rail."""

def hat_setup_swd(target_voltage_mv: int = 3300, *, connector: int = 0) -> dict:
    """Power and route the SWD target connector."""
