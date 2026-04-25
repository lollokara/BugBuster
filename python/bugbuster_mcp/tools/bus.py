"""
BugBuster MCP — External I2C/SPI bus planning tools.

Tools: plan_i2c_bus, plan_spi_bus, scan_i2c_bus, spi_transfer, spi_jedec_id,
defer_i2c_read, defer_i2c_write_read, defer_spi_transfer, get_deferred_bus_result
"""

from __future__ import annotations

from bugbuster.bus import BugBusterBusManager

from .. import session
from ..safety import require_valid_io, validate_vadj_voltage


def _planner() -> BugBusterBusManager:
    # Bus planning is side-effect free and does not require a connected device.
    return BugBusterBusManager(client=None)


def register(mcp) -> None:

    @mcp.tool()
    def plan_i2c_bus(
        sda_io: int,
        scl_io: int,
        supply_voltage: float,
        frequency_hz: int = 400_000,
        pullups: str = "external",
        allow_split_supplies: bool = False,
        confirm: bool = False,
    ) -> dict:
        """
        Preview the complete BugBuster route needed for an external I2C bus.

        This is a dry-run planner: it does not configure GPIOs, power rails,
        e-fuses, VLOGIC, or MUX switches. Use it before wiring or before a
        future bus setup/apply tool to verify that the requested SDA/SCL pins
        resolve to the expected ESP32 GPIOs and power domain.

        Parameters:
        - sda_io: Physical BugBuster IO number (1-12) for I2C SDA.
        - scl_io: Physical BugBuster IO number (1-12) for I2C SCL.
        - supply_voltage: Target VADJ rail voltage in volts (3.0-15.0 V).
        - frequency_hz: I2C clock in Hz. Common values: 100000, 400000.
        - pullups: "external", "internal", or "off". External is recommended.
        - allow_split_supplies: Normally false; reject pins spanning VADJ1/VADJ2.
        - confirm: Must be true for supply voltages above the safe threshold.

        Returns: route plan including ESP32 GPIOs, MUX bytes, supplies, e-fuses,
        scan reserved addresses skipped, side effects, warnings, and success.
        """
        require_valid_io(sda_io)
        require_valid_io(scl_io)
        validate_vadj_voltage(supply_voltage, confirm=confirm)

        plan = _planner().plan_i2c(
            sda=sda_io,
            scl=scl_io,
            io_voltage=session.get_vlogic(),
            supply_voltage=supply_voltage,
            frequency_hz=frequency_hz,
            pullups=pullups,
            allow_split_supplies=allow_split_supplies,
        ).as_dict()
        plan["success"] = True
        plan["dry_run"] = True
        plan["note"] = "Route preview only; no hardware state was changed."
        return plan

    @mcp.tool()
    def plan_spi_bus(
        sck_io: int,
        supply_voltage: float,
        mosi_io: int | None = None,
        miso_io: int | None = None,
        cs_io: int | None = None,
        frequency_hz: int = 1_000_000,
        allow_split_supplies: bool = False,
        confirm: bool = False,
    ) -> dict:
        """
        Preview the complete BugBuster route needed for an external SPI bus.

        This is a dry-run planner: it does not configure GPIOs, power rails,
        e-fuses, VLOGIC, or MUX switches. Use it before wiring or before a
        future bus setup/apply tool to verify SCK/MOSI/MISO/CS mapping.

        Parameters:
        - sck_io: Physical BugBuster IO number (1-12) for SPI SCK.
        - supply_voltage: Target VADJ rail voltage in volts (3.0-15.0 V).
        - mosi_io: Optional physical IO for SPI MOSI.
        - miso_io: Optional physical IO for SPI MISO.
        - cs_io: Optional physical IO for SPI chip-select.
        - frequency_hz: SPI clock in Hz.
        - allow_split_supplies: Normally false; reject pins spanning VADJ1/VADJ2.
        - confirm: Must be true for supply voltages above the safe threshold.

        Returns: route plan including ESP32 GPIOs, MUX bytes, supplies, e-fuses,
        side effects, warnings, and success.
        """
        require_valid_io(sck_io)
        for io in (mosi_io, miso_io, cs_io):
            if io is not None:
                require_valid_io(io)
        validate_vadj_voltage(supply_voltage, confirm=confirm)

        plan = _planner().plan_spi(
            sck=sck_io,
            mosi=mosi_io,
            miso=miso_io,
            cs=cs_io,
            io_voltage=session.get_vlogic(),
            supply_voltage=supply_voltage,
            frequency_hz=frequency_hz,
            allow_split_supplies=allow_split_supplies,
        ).as_dict()
        plan["success"] = True
        plan["dry_run"] = True
        plan["note"] = "Route preview only; no hardware state was changed."
        return plan

    @mcp.tool()
    def scan_i2c_bus(
        sda_io: int,
        scl_io: int,
        supply_voltage: float,
        frequency_hz: int = 400_000,
        pullups: str = "external",
        start_addr: int = 0x08,
        stop_addr: int = 0x77,
        allow_split_supplies: bool = False,
        confirm: bool = False,
    ) -> dict:
        """
        Configure a routed external I2C bus and scan for attached devices.

        This changes hardware state: it enables the needed VADJ rail/e-fuse,
        sets VLOGIC to the MCP server startup voltage, enables level-shifter OE,
        closes the planned MUX paths, binds the ESP32 external I2C peripheral,
        and scans addresses. Reserved I2C addresses are skipped.

        Parameters:
        - sda_io: Physical BugBuster IO number (1-12) for I2C SDA.
        - scl_io: Physical BugBuster IO number (1-12) for I2C SCL.
        - supply_voltage: Target VADJ rail voltage in volts (3.0-15.0 V).
        - frequency_hz: I2C clock in Hz. Common values: 100000, 400000.
        - pullups: "external", "internal", or "off". External is recommended.
        - start_addr: First 7-bit address to probe, default 0x08.
        - stop_addr: Last 7-bit address to probe, default 0x77.
        - allow_split_supplies: Normally false; reject pins spanning VADJ1/VADJ2.
        - confirm: Must be true for supply voltages above the safe threshold.

        Returns: detected addresses and the applied route plan.
        """
        require_valid_io(sda_io)
        require_valid_io(scl_io)
        validate_vadj_voltage(supply_voltage, confirm=confirm)

        bb = session.get_client()
        result = bb.bus.i2c_scan(
            sda=sda_io,
            scl=scl_io,
            io_voltage=session.get_vlogic(),
            supply_voltage=supply_voltage,
            frequency_hz=frequency_hz,
            pullups=pullups,
            start_addr=start_addr,
            stop_addr=stop_addr,
            skip_reserved=True,
            allow_split_supplies=allow_split_supplies,
        )
        result["success"] = True
        return result

    @mcp.tool()
    def spi_transfer(
        sck_io: int,
        supply_voltage: float,
        data: list[int],
        mosi_io: int | None = None,
        miso_io: int | None = None,
        cs_io: int | None = None,
        frequency_hz: int = 1_000_000,
        mode: int = 0,
        allow_split_supplies: bool = False,
        confirm: bool = False,
    ) -> dict:
        """
        Configure a routed external SPI bus and run one bounded full-duplex transfer.

        This changes hardware state: it enables the needed VADJ rail/e-fuse,
        sets VLOGIC to the MCP server startup voltage, enables level-shifter OE,
        closes the planned MUX paths, binds the ESP32 external SPI peripheral,
        and transfers the supplied bytes.

        Parameters:
        - sck_io: Physical BugBuster IO number (1-12) for SPI SCK.
        - supply_voltage: Target VADJ rail voltage in volts (3.0-15.0 V).
        - data: Bytes to clock out, each 0-255. Response length matches data length.
        - mosi_io: Optional physical IO for MOSI.
        - miso_io: Optional physical IO for MISO.
        - cs_io: Optional physical IO for chip-select.
        - frequency_hz: SPI clock in Hz.
        - mode: SPI mode 0-3.
        - allow_split_supplies: Normally false; reject pins spanning VADJ1/VADJ2.
        - confirm: Must be true for supply voltages above the safe threshold.

        Returns: received bytes and the applied route plan.
        """
        require_valid_io(sck_io)
        for io in (mosi_io, miso_io, cs_io):
            if io is not None:
                require_valid_io(io)
        validate_vadj_voltage(supply_voltage, confirm=confirm)
        if not data or len(data) > 512 or any((not isinstance(b, int)) or b < 0 or b > 255 for b in data):
            raise ValueError("data must be 1-512 byte values in range 0-255")

        bb = session.get_client()
        plan = bb.bus.setup_spi(
            sck=sck_io,
            mosi=mosi_io,
            miso=miso_io,
            cs=cs_io,
            io_voltage=session.get_vlogic(),
            supply_voltage=supply_voltage,
            frequency_hz=frequency_hz,
            mode=mode,
            allow_split_supplies=allow_split_supplies,
        )
        rx = bb.bus.spi_transfer(data)
        return {
            "success": True,
            "rx": list(rx),
            "plan": plan.as_dict(),
        }

    @mcp.tool()
    def spi_jedec_id(
        sck_io: int,
        miso_io: int,
        cs_io: int,
        supply_voltage: float,
        mosi_io: int | None = None,
        frequency_hz: int = 1_000_000,
        mode: int = 0,
        allow_split_supplies: bool = False,
        confirm: bool = False,
    ) -> dict:
        """
        Configure a routed external SPI bus and read a common JEDEC ID.

        Sends command 0x9F followed by three dummy bytes. Useful first smoke
        test for SPI flash and many SPI peripherals that expose JEDEC identity.
        """
        require_valid_io(sck_io)
        require_valid_io(miso_io)
        require_valid_io(cs_io)
        if mosi_io is not None:
            require_valid_io(mosi_io)
        validate_vadj_voltage(supply_voltage, confirm=confirm)

        bb = session.get_client()
        plan = bb.bus.setup_spi(
            sck=sck_io,
            mosi=mosi_io,
            miso=miso_io,
            cs=cs_io,
            io_voltage=session.get_vlogic(),
            supply_voltage=supply_voltage,
            frequency_hz=frequency_hz,
            mode=mode,
            allow_split_supplies=allow_split_supplies,
        )
        result = bb.bus.spi_jedec_id()
        result["success"] = True
        result["plan"] = plan.as_dict()
        return result

    @mcp.tool()
    def defer_i2c_read(address: int, length: int, timeout_ms: int = 100) -> dict:
        """
        Queue an I2C read on the already-configured external bus.

        Use scan_i2c_bus first, or configure the bus through Python. This queues
        the transaction on the ESP32 deferred worker so the operation can run
        independent of host polling jitter.
        """
        bb = session.get_client()
        job_id = bb.bus.defer_i2c_read(address, length, timeout_ms=timeout_ms)
        return {"success": True, "job_id": job_id, "kind": "i2c_read"}

    @mcp.tool()
    def defer_i2c_write_read(
        address: int,
        write_data: list[int],
        read_length: int,
        timeout_ms: int = 100,
    ) -> dict:
        """
        Queue an I2C write/read transaction on the already-configured external bus.
        """
        if not write_data or len(write_data) > 255 or any((not isinstance(b, int)) or b < 0 or b > 255 for b in write_data):
            raise ValueError("write_data must be 1-255 byte values in range 0-255")
        bb = session.get_client()
        job_id = bb.bus.defer_i2c_write_read(address, write_data, read_length, timeout_ms=timeout_ms)
        return {"success": True, "job_id": job_id, "kind": "i2c_write_read"}

    @mcp.tool()
    def defer_spi_transfer(data: list[int], timeout_ms: int = 100) -> dict:
        """
        Queue an SPI transfer on the already-configured external bus.
        """
        if not data or len(data) > 512 or any((not isinstance(b, int)) or b < 0 or b > 255 for b in data):
            raise ValueError("data must be 1-512 byte values in range 0-255")
        bb = session.get_client()
        job_id = bb.bus.defer_spi_transfer(data, timeout_ms=timeout_ms)
        return {"success": True, "job_id": job_id, "kind": "spi_transfer"}

    @mcp.tool()
    def get_deferred_bus_result(job_id: int) -> dict:
        """
        Poll a queued deferred I2C/SPI operation.
        """
        result = session.get_client().bus.deferred_result(job_id)
        result["success"] = True
        return result
