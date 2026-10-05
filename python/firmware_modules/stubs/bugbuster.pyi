"""BugBuster board control from on-device MicroPython scripts.

Analog/digital channels (``Channel``), the external I2C/SPI buses on the IO
terminals, HTTP/MQTT, IO ownership (``claim``), the VADJ rails and e-fuses, and
the SWD/LA HAT.

IO terminals are numbered 1-12 (never raw ESP32 GPIO numbers). IO1-IO6 are
powered from VADJ1, IO7-IO12 from VADJ2. The analog channels CH0-CH3 sit on
IO3, IO6, IO9 and IO12. Ownership slots number 0-11 for IO1-IO12 and 12-15 for
CH0-CH3.

Every blocking call (``sleep``, bus transfers, HTTP, ``rail_power_up``) can be
interrupted by the Stop button: a pending stop raises ``KeyboardInterrupt``.

Example:
    import bugbuster
    ch = bugbuster.Channel(0)
    ch.set_function(bugbuster.FUNC_VOUT)
    ch.set_voltage(2.5)
    bugbuster.sleep(500)
    ch.set_function(bugbuster.FUNC_HIGH_IMP)
"""

FUNC_HIGH_IMP: int = 0
"""Channel function 0: high impedance. The safe resting state; use it to release an output."""
FUNC_VOUT: int = 1
"""Channel function 1: voltage output. ``Channel.set_voltage`` drives the terminal."""
FUNC_IOUT: int = 2
"""Channel function 2: current output (externally powered)."""
FUNC_VIN: int = 3
"""Channel function 3: voltage input (ADC). Use with ``Channel.read_voltage``."""
FUNC_IIN_EXT_PWR: int = 4
"""Channel function 4: current input, externally powered loop."""
FUNC_IIN_LOOP_PWR: int = 5
"""Channel function 5: current input, loop powered by the board."""
FUNC_RES_MEAS: int = 7
"""Channel function 7: resistance measurement (RTD)."""
FUNC_DIN_LOGIC: int = 8
"""Channel function 8: digital input, logic level."""
FUNC_DIN_LOOP: int = 9
"""Channel function 9: digital input, loop powered."""
FUNC_IOUT_HART: int = 10
"""Channel function 10: current output with HART."""
FUNC_IIN_EXT_PWR_HART: int = 11
"""Channel function 11: current input, externally powered, with HART."""
FUNC_IIN_LOOP_PWR_HART: int = 12
"""Channel function 12: current input, loop powered, with HART."""
HAT_RAIL_3V3_ADJ: int = 0
"""HAT rail 0: the adjustable I/O voltage rail (its "voltage" is the HAT I/O level)."""
HAT_RAIL_VADJ3: int = 1
"""HAT rail 1: VADJ3, a buck-boost target supply."""
HAT_RAIL_VADJ4: int = 2
"""HAT rail 2: VADJ4, a buck-boost target supply."""
HAT_LED_OFF: int = 0
"""HAT LED colour 0: off."""
HAT_LED_RED: int = 1
"""HAT LED colour 1: red."""
HAT_LED_GREEN: int = 2
"""HAT LED colour 2: green."""
HAT_LED_BLUE: int = 3
"""HAT LED colour 3: blue."""
HAT_LED_YELLOW: int = 4
"""HAT LED colour 4: yellow."""
HAT_LED_CYAN: int = 5
"""HAT LED colour 5: cyan."""
HAT_LED_MAGENTA: int = 6
"""HAT LED colour 6: magenta."""
HAT_LED_WHITE: int = 7
"""HAT LED colour 7: white."""

def sleep(ms: int) -> None:
    """Pause the script for ``ms`` milliseconds; a Stop request interrupts it.

    Use this instead of ``time.sleep`` so the Stop button can end the script
    while it waits.

    Args:
        ms: Delay in milliseconds, 0 or more.

    Raises:
        ValueError: ``ms`` is negative.
        KeyboardInterrupt: the script was stopped during the wait.

    Example:
        import bugbuster
        bugbuster.sleep(250)
    """

def log(level: str, msg: str) -> None:
    """Write a line to the script log (source ``mpy``) and the console.

    A message containing line breaks is split into one log line per non-empty
    segment. ``bb_logging`` wraps this with timestamps.

    Args:
        level: One character: 'E' error, 'W' warning, 'I' info, 'D' debug.
        msg: Message text.

    Raises:
        ValueError: ``level`` is not one of 'E', 'W', 'I', 'D'.

    Example:
        bugbuster.log("W", "supply sagging")
    """

def ticks_ms() -> int:
    """Return the millisecond tick counter (time since boot).

    The counter is unsigned 32-bit and wraps after about 49.7 days, so compare
    timestamps by subtracting. It is the same time base as the script log.

    Returns:
        Milliseconds since boot.

    Example:
        t0 = bugbuster.ticks_ms()
        bugbuster.sleep(100)
        print("elapsed", bugbuster.ticks_ms() - t0, "ms")
    """

def vadj_pd_warning(rail: int, voltage: float) -> str | None:
    """Check whether the USB-C input can supply a VADJ rail voltage.

    VADJ1/VADJ2 are buck rails: they cannot regulate above the active USB-C
    input voltage (about 5 V from data USB or a 5 V source). Only requests above
    5 V are checked against the negotiated PD contract. Reads the PD controller;
    changes nothing.

    Args:
        rail: VADJ rail, 1 or 2.
        voltage: Requested rail voltage in volts.

    Returns:
        A warning sentence explaining why the voltage is not available, or
        ``None`` when the request is fine.

    Raises:
        ValueError: ``rail`` is not 1 or 2.

    Example:
        warning = bugbuster.vadj_pd_warning(1, 9.0)
        if warning:
            print(warning)
    """

def rail_power_up(rail: int, volts: float, efuse_mask: int) -> dict:
    """Bring a VADJ rail up to ``volts`` and arm its e-fuses.

    Sequence: switch the selected e-fuses off, program the voltage, enable the
    rail, wait 500 ms to settle, arm the e-fuses, wait the 120 ms e-fuse blackout,
    then sample power-good and fault. Blocks for roughly 0.7 s.

    Args:
        rail: VADJ rail, 1 (IO1-IO6) or 2 (IO7-IO12).
        volts: Target voltage, 3-12 V. The rail quantises to about 0.1 V steps.
        efuse_mask: E-fuses to arm, 1-3 (bit 0 = first e-fuse of the rail:
            EFUSE1 for rail 1, EFUSE3 for rail 2; bit 1 = second: EFUSE2 / EFUSE4;
            3 = both).

    Returns:
        A dict reporting the result.

    Keys:
        pg: True when the rail reports power-good.
        fault: True when any selected e-fuse has tripped.

    Raises:
        ValueError: ``rail`` is not 1-2, ``efuse_mask`` is not 1-3, or ``volts`` is
            outside 3-12 V.
        OSError: EIO when a hardware step fails.

    Safety:
        Powers the IO terminals of that rail (and anything wired to them). Check
        the voltage against your target first; ``vadj_pd_warning`` tells you when
        the USB-C input cannot deliver it.

    Example:
        result = bugbuster.rail_power_up(1, 3.3, 3)
        if not result["pg"] or result["fault"]:
            raise OSError("VADJ1 did not come up cleanly")
    """

def efuse_set(efuse: int, on: bool) -> None:
    """Switch one e-fuse on or off.

    The e-fuses gate the VADJ supply to groups of IO terminals: EFUSE1 IO1-IO3,
    EFUSE2 IO4-IO6, EFUSE3 IO7-IO9, EFUSE4 IO10-IO12. This does not program the
    rail voltage; use ``rail_power_up`` for the full sequence. A freshly enabled
    e-fuse has a 100 ms blackout during which a short is not reported.

    Args:
        efuse: E-fuse number, 1-4.
        on: True to supply the terminals, False to cut power.

    Raises:
        ValueError: ``efuse`` is not 1-4.
        OSError: EIO when the expander write fails (for example no mainboard I/O expander).

    Safety:
        ``on=True`` powers the terminals of that group from the current VADJ voltage.

    Example:
        bugbuster.efuse_set(1, False)
    """

class Channel:
    """One analog channel of the AD74416H front-end (CH0-CH3).

    CH0-CH3 map to IO3, IO6, IO9 and IO12 and to ownership slots 12-15. The
    state-changing methods (``set_function``, ``set_voltage``, ``set_do``) take a
    one-shot 5 s claim on the channel unless the script already holds one with
    ``claim()``, and raise ``OSError`` EEXIST when another owner (app, HTTP, CLI)
    holds it.

    Example:
        ch = bugbuster.Channel(2)
        ch.set_function(bugbuster.FUNC_VIN)
        print(ch.read_voltage())
    """
    def __init__(self, channel: int) -> None:
        """Create a handle for one channel. Does not change the hardware.

        Args:
            channel: Channel index, 0-3.

        Raises:
            ValueError: ``channel`` is outside 0-3.

        Example:
            ch = bugbuster.Channel(0)
        """
    def set_function(self, func: int) -> None:
        """Select the channel function (one of the ``FUNC_*`` constants).

        Reconfigures the analog front-end for that channel. Switch to
        ``FUNC_HIGH_IMP`` when you are done driving it.

        Args:
            func: A ``FUNC_*`` constant (0-5 or 7-12).

        Raises:
            ValueError: ``func`` is not a valid function code.
            OSError: EEXIST when another owner holds the channel; EIO when the
                hardware refuses.

        Safety:
            ``FUNC_VOUT``, ``FUNC_IOUT`` and the HART variants drive the terminal.

        Example:
            ch.set_function(bugbuster.FUNC_VIN)
        """
    def set_voltage(self, voltage: float, bipolar: bool = False) -> None:
        """Set the DAC output voltage (the channel must be in ``FUNC_VOUT``).

        Out-of-range values are clamped, not rejected. Changing ``bipolar``
        between calls re-parks the output while the range switches.

        Args:
            voltage: Output in volts. Unipolar 0 to 12 V; bipolar -12 to +12 V.
            bipolar: True selects the bipolar range. Default False.

        Raises:
            OSError: EEXIST when another owner holds the channel; EIO when the
                DAC write fails.

        Safety:
            Drives the IO terminal immediately.

        Example:
            ch.set_function(bugbuster.FUNC_VOUT)
            ch.set_voltage(3.3)
        """
    def read_voltage(self) -> float:
        """Return the latest ADC reading of the channel in volts.

        The value comes from the firmware's continuous ADC poll, so it is the
        most recent sample, not a fresh conversion. Use it with ``FUNC_VIN`` (or
        to read back a ``FUNC_VOUT`` output).

        Returns:
            Voltage in volts.

        Raises:
            OSError: ETIMEDOUT when the measurement state could not be read within 50 ms.

        Example:
            volts = ch.read_voltage()
        """
    def set_do(self, value: bool) -> None:
        """Set the channel's digital output on or off.

        Args:
            value: True for logic high, False for low.

        Raises:
            OSError: EEXIST when another owner holds the channel; EBUSY when the
                control task's command queue is full.

        Safety:
            Drives the IO terminal.

        Example:
            ch.set_do(True)
        """

class I2C:
    """External I2C master on two IO terminals (not raw ESP32 GPIOs).

    Creating the bus routes the pins, programs the VADJ supply and enables the
    e-fuse of the IO block, so the target is powered once the constructor
    returns. Only one external bus is active at a time; call ``close()`` before
    wiring a different pin group. For bit-banged GPIO buses use ``machine.SoftI2C``.

    Example:
        bus = bugbuster.I2C(1, 2, freq=100000, supply=3.3, vlogic=3.3)
        try:
            print(bus.scan())
        finally:
            bus.close()
    """
    def __init__(self, sda_io: int, scl_io: int, *, freq: int = 400000, pullups: str = "external",
                 supply: float | None = None, vlogic: float | None = None,
                 allow_split_supplies: bool = False) -> None:
        """Open the bus and power the IO block.

        Args:
            sda_io: IO terminal for SDA, 1-12.
            scl_io: IO terminal for SCL, 1-12; must differ from ``sda_io``.
            freq: Clock in Hz. Default 400000. Use external pull-ups from 400 kHz up.
            pullups: 'external' (default), 'internal' or 'off'. 'off' behaves like
                'external': only 'internal' enables the board's pull-ups.
            supply: VADJ rail voltage for the IO block in volts, 3.0-15.0. Default 3.3.
            vlogic: Logic level in volts. Default 3.3.
            allow_split_supplies: True permits SDA and SCL on different rails
                (IO1-6 vs IO7-12). Default False.

        Raises:
            ValueError: a pin is outside 1-12 or SDA equals SCL, ``pullups`` is
                unknown, ``supply`` is outside 3.0-15.0 V, or the bus planner
                rejects the route (the message says why).

        Safety:
            Powers the target at ``supply``. A "Warning:" line is printed when the
            USB-C input cannot deliver that voltage.

        Example:
            bus = bugbuster.I2C(1, 2, freq=100000, supply=3.3, vlogic=3.3)
        """
    def close(self) -> None:
        """Release the bus controller and its IO terminals.

        Raises:
            OSError: EIO when the release fails.

        Example:
            bus.close()
        """
    def scan(self, start: int = 0x08, stop: int = 0x77, skip_reserved: bool = True,
             timeout_ms: int = 50) -> list[int]:
        """Probe the address range and return the 7-bit addresses that ACK.

        Args:
            start: First address to probe. Default 0x08.
            stop: Last address to probe. Default 0x77.
            skip_reserved: Skip reserved address blocks. Default True.
            timeout_ms: Per-transaction timeout in milliseconds. Default 50.

        Returns:
            Sorted list of responding 7-bit addresses.

        Raises:
            OSError: EIO when the scan fails.

        Example:
            print(["0x%02X" % a for a in bus.scan()])
        """
    def writeto(self, addr: int, buf: bytes, timeout_ms: int = 50) -> None:
        """Write ``buf`` to the device at ``addr``.

        Args:
            addr: 7-bit device address.
            buf: bytes, bytearray or other buffer to send.
            timeout_ms: Timeout in milliseconds. Default 50.

        Raises:
            OSError: EIO when the device does not ACK or the transfer fails.

        Example:
            bus.writeto(0x48, bytes([0x01, 0x60]))
        """
    def readfrom(self, addr: int, n: int, timeout_ms: int = 50) -> bytes:
        """Read ``n`` bytes from the device at ``addr``.

        Args:
            addr: 7-bit device address.
            n: Byte count, 1-512.
            timeout_ms: Timeout in milliseconds. Default 50.

        Returns:
            The bytes read.

        Raises:
            ValueError: ``n`` is outside 1-512.
            OSError: ETIMEDOUT when the read fails.

        Example:
            data = bus.readfrom(0x48, 2)
        """
    def writeto_then_readfrom(self, addr: int, buf: bytes, rd_n: int, timeout_ms: int = 50) -> bytes:
        """Write ``buf`` then read ``rd_n`` bytes with a repeated start.

        The usual way to read a register: write the register number, read the value.

        Args:
            addr: 7-bit device address.
            buf: Bytes to write first (typically the register address).
            rd_n: Byte count to read, 1-512.
            timeout_ms: Timeout in milliseconds. Default 50.

        Returns:
            The bytes read.

        Raises:
            ValueError: ``rd_n`` is outside 1-512.
            OSError: EIO when the transfer fails.

        Example:
            chip_id = bus.writeto_then_readfrom(0x76, b"\\xd0", 1)[0]
        """

class SPI:
    """External SPI master on IO terminals (not raw ESP32 GPIOs).

    Like ``I2C``, creating the bus routes the pins and powers the IO block from a
    VADJ rail. Use ``close()`` to release it. For bit-banged GPIO buses use
    ``machine.SoftSPI``.

    Example:
        spi = bugbuster.SPI(1, mosi_io=2, miso_io=3, cs_io=4, freq=1000000, supply=3.3)
        try:
            print(spi.transfer(bytes([0x9F, 0, 0, 0])))
        finally:
            spi.close()
    """
    def __init__(self, sck_io: int, *, mosi_io: int | None = None, miso_io: int | None = None,
                 cs_io: int | None = None, freq: int = 1000000, mode: int = 0,
                 supply: float | None = None, vlogic: float | None = None,
                 allow_split_supplies: bool = False) -> None:
        """Open the bus and power the IO block.

        Args:
            sck_io: IO terminal for the clock, 1-12.
            mosi_io: IO terminal for MOSI, 1-12, or None to omit.
            miso_io: IO terminal for MISO, 1-12, or None to omit.
            cs_io: IO terminal for chip select, 1-12, or None to omit.
            freq: Clock in Hz. Default 1000000.
            mode: SPI mode 0-3 (clock polarity and phase). Default 0.
            supply: VADJ rail voltage for the IO block in volts, 3.0-15.0. Default 3.3.
            vlogic: Logic level in volts. Default 3.3.
            allow_split_supplies: True permits pins on different rails. Default False.

        Raises:
            ValueError: a pin is outside 1-12, ``mode`` is outside 0-3, ``supply``
                is outside 3.0-15.0 V, or the bus planner rejects the route.

        Safety:
            Powers the target at ``supply``. A "Warning:" line is printed when the
            USB-C input cannot deliver that voltage.

        Example:
            spi = bugbuster.SPI(1, mosi_io=2, miso_io=3, cs_io=4, supply=3.3)
        """
    def transfer(self, buf: bytes) -> bytes:
        """Clock ``buf`` out and return the bytes clocked in (full duplex).

        Args:
            buf: Bytes to send, 1-512. The reply has the same length.

        Returns:
            The bytes received while sending.

        Raises:
            ValueError: ``buf`` is empty or longer than 512 bytes.
            OSError: EIO when the transfer fails (50 ms timeout).

        Example:
            jedec = spi.transfer(bytes([0x9F, 0x00, 0x00, 0x00]))[1:]
        """
    def close(self) -> None:
        """Release the bus controller and its IO terminals.

        Raises:
            OSError: EIO when the release fails.

        Example:
            spi.close()
        """

class Response:
    """HTTP result returned by :func:`http_get` and :func:`http_post`.

    A named tuple: unpack it as ``status, body = r`` or read the attributes.
    A non-2xx status is returned, not raised.

    Example:
        r = bugbuster.http_get("http://192.168.1.10/status")
        status, body = r
        print(r.status, len(r.body))
    """
    status: int
    """HTTP status code, for example 200."""
    body: bytes
    """Response body as bytes. Use ``body.decode()`` for text."""

class Claim:
    """Context manager returned by :func:`claim`; acquires on entry, releases on exit.

    Exceptions inside the ``with`` block are not suppressed.

    Example:
        with bugbuster.claim([12]):
            ch = bugbuster.Channel(0)
            ch.set_function(bugbuster.FUNC_VOUT)
    """
    def __enter__(self) -> "Claim":
        """Acquire the slots.

        Raises:
            OSError: EEXIST when another owner holds a slot.

        Example:
            with bugbuster.claim([0, 1]) as held:
                print("holding IO1 and IO2")
        """
    def __exit__(self, exc_type, exc, tb) -> None:
        """Release the slots, whether or not the block raised.

        Args:
            exc_type: Exception class if the block raised, else None.
            exc: Exception instance if the block raised, else None.
            tb: Traceback if the block raised, else None.

        Example:
            with bugbuster.claim([0]):
                pass  # released here, even after an exception
        """

def http_get(url: str, *, headers: dict | None = None, timeout_ms: int = 10000) -> Response:
    """HTTP or HTTPS GET.

    HTTPS validates against the built-in CA bundle. Blocks until the response is
    complete or the timeout expires (a Stop request is honoured on return).

    Args:
        url: Full URL, ``http://`` or ``https://``.
        headers: Extra request headers as a dict of str to str, at most 16.
        timeout_ms: Timeout in milliseconds, greater than 0. Default 10000.

    Returns:
        A :class:`Response` ``(status, body)``.

    Raises:
        ValueError: ``timeout_ms`` is not positive or there are more than 16 headers.
        TypeError: ``headers`` is not a dict or None.
        OSError: ECONNABORTED when the request fails (no Wi-Fi, DNS, TLS, timeout).
        KeyboardInterrupt: the script was stopped.

    Example:
        r = bugbuster.http_get("http://192.168.1.10/status", timeout_ms=3000)
        print(r.status, r.body.decode())
    """

def http_post(url: str, body: str | bytes | None = None, *, headers: dict | None = None,
              timeout_ms: int = 10000) -> Response:
    """HTTP or HTTPS POST.

    Args:
        url: Full URL, ``http://`` or ``https://``.
        body: Request body as bytes or bytearray (``str`` must be encoded first:
            ``"x".encode()``); None sends an empty body.
        headers: Extra request headers as a dict of str to str, at most 16.
        timeout_ms: Timeout in milliseconds, greater than 0. Default 10000.

    Returns:
        A :class:`Response` ``(status, body)``.

    Raises:
        ValueError: ``timeout_ms`` is not positive or there are more than 16 headers.
        TypeError: ``headers`` is not a dict or None.
        OSError: ECONNABORTED when the request fails.
        KeyboardInterrupt: the script was stopped.

    Example:
        r = bugbuster.http_post("http://192.168.1.10/log", b'{"v": 3.3}',
                                headers={"Content-Type": "application/json"})
    """

def mqtt_publish(topic: str, payload: str | bytes, host: str, *, port: int = 1883,
                 username: str | None = None, password: str | None = None) -> None:
    """Publish one MQTT message (QoS 0, not retained) and disconnect.

    Opens a plain TCP connection (no TLS) to the broker for each call and waits up
    to 5 s for it to come up, so use it for occasional messages, not high-rate
    streaming.

    Args:
        topic: Topic string.
        payload: Message bytes or text.
        host: Broker host name or IP address.
        port: Broker TCP port, 1-65535. Default 1883.
        username: Optional user name.
        password: Optional password.

    Raises:
        ValueError: ``port`` is outside 1-65535.
        OSError: ECONNABORTED when the broker could not be reached.
        KeyboardInterrupt: the script was stopped.

    Example:
        bugbuster.mqtt_publish("bench/ch0", "%.3f" % ch.read_voltage(), "192.168.1.10")
    """

def claim(slots: list[int], *, purpose: str | None = None, lease_ms: int = 0) -> Claim:
    """Create a claim on IO slots; use it as ``with claim([...]):``.

    Nothing is acquired until the ``with`` block is entered. While held, other
    owners (app, HTTP, CLI) cannot write those slots, and ``Channel`` calls
    inside the block do not take their own one-shot claim.

    Args:
        slots: 1-16 slot numbers: 0-11 for IO1-IO12, 12-15 for CH0-CH3.
        purpose: Free-text label recorded with the claim.
        lease_ms: Lease in milliseconds; 0 (default) holds until released.

    Returns:
        A :class:`Claim` context manager.

    Raises:
        ValueError: ``slots`` is empty, longer than 16 or holds a number outside 0-15.

    Example:
        with bugbuster.claim([12, 13], purpose="sweep"):
            ...
    """

def release(*, slots: list[int] | None = None) -> None:
    """Release slots held by this script immediately.

    Prefer the ``with claim(...)`` form, which releases automatically.

    Args:
        slots: Slot numbers to release; None (default) releases every slot this
            script holds.

    Raises:
        ValueError: a slot number is outside 0-15.

    Example:
        bugbuster.release()
    """

def owner_status() -> list[dict]:
    """Return the ownership table of all 16 slots.

    Returns:
        A list of 16 dicts, index = slot number.

    Keys:
        slot: Slot number 0-15.
        kind: Owner kind: 0 none, 1 USB, 2 HTTP, 3 script, 4 CLI, 5 internal.
        session_id: Owner session id.
        token_fp32: 32-bit fingerprint of the owner's token (HTTP owners).
        lease_until_ms: Low 32 bits of the lease expiry in firmware milliseconds; 0 means no expiry.

    Raises:
        OSError: EIO when the table cannot be read.

    Example:
        busy = [s["slot"] for s in bugbuster.owner_status() if s["kind"]]
    """

def hat_status() -> dict:
    """Return the HAT detection, link and firmware state. Works without a HAT.

    The ``hat_*`` calls below drive the SWD/GPIO HAT; they raise ``OSError`` EIO
    when no such HAT is connected (the DAQ HAT uses the ``daq`` module instead).

    Returns:
        A dict describing the HAT.

    Keys:
        detected: HAT strap detected on the connector.
        connected: Link to the HAT established.
        type: 0 none, 1 SWD/GPIO HAT, 16 DAQ HAT, 255 unknown.
        detect_voltage: Raw detect-pin voltage in volts.
        fw_major: HAT firmware major version.
        fw_minor: HAT firmware minor version.
        config_confirmed: HAT acknowledged the last pin configuration.
        io_voltage_mv: HAT I/O voltage in millivolts.
        caps_valid: ``hat_caps()`` data is valid.
        la_route: Logic-analyser route: 0 low speed, 1 high speed.
        dap_connected: A USB CMSIS-DAP host is connected.
        target_detected: An SWD target answers.
        target_dpidr: DPIDR of the SWD target.
        last_ok_ms: Time of the last good HAT reply, ms.
        last_timeout_ms: Time of the last HAT timeout, ms.
        consecutive_timeouts: Timeouts in a row.
        degraded: Link is flagged degraded.
        pin_config: List of 4 function codes for the HAT expansion pins.

    Example:
        st = bugbuster.hat_status()
        print(st["connected"], st["type"])
    """

def hat_caps() -> dict:
    """Return the HAT capabilities.

    Returns:
        A dict describing what the HAT hardware offers.

    Keys:
        hw_revision: Hardware revision.
        flags: Bit mask: 1 rails, 2 LEDs, 4 LA low-speed, 8 LA high-speed, 16 shifted I/O.
        rail_count: Number of rails.
        led_count: Number of LEDs.
        shifted_io_count: Number of level-shifted I/O pins.
        la_routes: Number of logic-analyser routes.
        fw_major: HAT firmware major version.
        fw_minor: HAT firmware minor version.

    Raises:
        OSError: EIO when capabilities are not available (no HAT).

    Example:
        caps = bugbuster.hat_caps()
        print(caps["rail_count"], "rails")
    """

def hat_rails() -> list[dict]:
    """Return the state of the HAT rails.

    Returns:
        One dict per rail, in ``HAT_RAIL_*`` order.

    Keys:
        rail_id: Rail number (see ``HAT_RAIL_*``).
        enabled: Rail output is on.
        voltage_mv: Measured rail voltage in millivolts.
        current_ma: Measured current in milliamps.
        status: HAT-reported rail status code.

    Raises:
        OSError: EIO when the HAT does not answer.

    Example:
        for rail in bugbuster.hat_rails():
            print(rail["rail_id"], rail["voltage_mv"], "mV")
    """

def hat_set_rail_enable(rail: int, enable: bool) -> None:
    """Switch a HAT rail on or off.

    Args:
        rail: A ``HAT_RAIL_*`` value, 0-2.
        enable: True to enable the rail.

    Raises:
        ValueError: ``rail`` is outside 0-2.
        OSError: EIO when the HAT refuses or is absent.

    Safety:
        Enabling VADJ3/VADJ4 powers whatever is wired to the rail.

    Example:
        bugbuster.hat_set_rail_enable(bugbuster.HAT_RAIL_VADJ3, False)
    """

def hat_set_rail_voltage(rail: int, mv: int) -> None:
    """Set a HAT rail voltage.

    For ``HAT_RAIL_3V3_ADJ`` this sets the HAT I/O voltage; for VADJ3/VADJ4 it
    sets the regulator target.

    Args:
        rail: A ``HAT_RAIL_*`` value, 0-2.
        mv: Voltage in millivolts, 0-36000.

    Raises:
        ValueError: ``rail`` is outside 0-2 or ``mv`` outside 0-36000.
        OSError: EIO when the HAT refuses or is absent.

    Safety:
        Changes the voltage on a live rail. Disable the rail first when the target
        is connected.

    Example:
        bugbuster.hat_set_rail_voltage(bugbuster.HAT_RAIL_VADJ3, 3300)
    """

def hat_led(led: int, color: int) -> None:
    """Set a HAT LED colour.

    Args:
        led: LED number, 1-8.
        color: A ``HAT_LED_*`` code (0-7), or any code 0-255 the HAT accepts.

    Raises:
        ValueError: ``led`` is outside 1-8 or ``color`` outside 0-255.
        OSError: EIO when the HAT refuses or is absent.

    Example:
        bugbuster.hat_led(1, bugbuster.HAT_LED_GREEN)
    """

def hat_io_bank(dirs: int, *, ups: int = 0, dns: int = 0, vals: int = 0) -> None:
    """Configure the HAT's 8-bit I/O bank; each argument is a bit mask (bit n = pin n).

    Args:
        dirs: Direction mask, 1 = output.
        ups: Pull-up mask, 1 = pull-up enabled. Default 0.
        dns: Pull-down mask, 1 = pull-down enabled. Default 0.
        vals: Output level mask for the pins set to output. Default 0.

    Raises:
        ValueError: any mask is outside 0-255.
        OSError: EIO when the HAT refuses or is absent.

    Safety:
        Pins set to output drive the target.

    Example:
        bugbuster.hat_io_bank(0x0F, vals=0x05)
    """

def hat_level_shift(oe: bool, dir: int) -> dict:
    """Set the HAT level-shifter output enable and direction.

    Args:
        oe: True enables the shifter outputs.
        dir: Direction, any truthy value selects one way, 0 the other.

    Returns:
        A dict with the state the HAT applied.

    Keys:
        oe: Output enable now in effect.
        dir: Direction now in effect.

    Raises:
        OSError: EIO when the HAT refuses or is absent.

    Example:
        state = bugbuster.hat_level_shift(True, 1)
    """

def hat_calibrate_start(rail: int) -> int:
    """Start the calibration of a HAT rail.

    Poll :func:`hat_calibrate_status` for progress.

    Args:
        rail: A ``HAT_RAIL_*`` value, 0-2.

    Returns:
        The status byte the HAT returned for the start request.

    Raises:
        ValueError: ``rail`` is outside 0-2.
        OSError: EIO when the HAT refuses or is absent.

    Safety:
        Calibration steps the rail through test voltages; disconnect sensitive targets first.

    Example:
        bugbuster.hat_calibrate_start(bugbuster.HAT_RAIL_VADJ3)
    """

def hat_calibrate_status() -> dict:
    """Return the calibration progress and result.

    Returns:
        A dict with the calibration state.

    Keys:
        state: Calibration state code.
        progress: Progress value reported by the HAT.
        rail_id: Rail being calibrated.
        last_error: Last error code, 0 = none.
        persist_state: Persistence (flash save) state code.
        stage: Current stage number.
        point: Current calibration point.
        code: DAC code under test (-128..127).
        measured_mv: Last measured voltage in millivolts.
        min_mv: Lowest reachable voltage in millivolts, -1 if unknown.
        max_mv: Highest reachable voltage in millivolts, -1 if unknown.
        max_gap_mv: Largest gap between points in millivolts, -1 if unknown.
        max_error_mv: Largest validation error in millivolts, -1 if unknown.
        validation_flags: Bit mask of validation findings.

    Raises:
        OSError: EIO when the HAT does not answer.

    Example:
        st = bugbuster.hat_calibrate_status()
        print(st["state"], st["progress"])
    """

def hat_calibrate_import(rail: int, points: list) -> None:
    """Load calibration points into a HAT rail.

    Args:
        rail: A ``HAT_RAIL_*`` value, 0-2.
        points: 2-6 ``(dac_code, measured_v)`` pairs; ``dac_code`` is an integer
            -128..127, ``measured_v`` the voltage you measured in volts.

    Raises:
        ValueError: ``rail`` is outside 0-2, there are not 2-6 points, a point is
            not a pair, or a code is outside -128..127.
        OSError: EIO when the HAT refuses or is absent.

    Example:
        bugbuster.hat_calibrate_import(1, [(-100, 1.52), (0, 3.31), (100, 5.07)])
    """

def hat_setup_swd(target_voltage_mv: int = 3300, *, connector: int = 0) -> None:
    """Power the SWD target connector and set the HAT I/O voltage.

    Sets the HAT I/O voltage, then enables power on the connector. Blocks about
    250 ms. The SWD pins themselves are wired directly to the debug probe; connect
    your debug tool over USB CMSIS-DAP afterwards and check ``hat_status()``.

    Args:
        target_voltage_mv: Target voltage in millivolts, 1200-5500. Default 3300.
        connector: 0 for connector A (VADJ1), 1 for connector B (VADJ2). Default 0.

    Raises:
        ValueError: ``target_voltage_mv`` is outside 1200-5500 or ``connector`` is not 0/1.
        OSError: EIO when the HAT refuses, is absent or the connector cannot be powered.

    Safety:
        Powers the target connector at the given voltage.

    Example:
        bugbuster.hat_setup_swd(3300, connector=0)
    """
