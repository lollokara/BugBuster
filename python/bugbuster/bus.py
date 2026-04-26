"""
External I2C/SPI bus planning helpers.

This module is intentionally side-effect free for now: it computes the IO
route, power, MUX, and warning contract that the firmware bus engine will later
apply. Keeping the planner host-runnable lets tests lock the safety contract
before timing-critical firmware work starts.
"""

from __future__ import annotations

from dataclasses import dataclass
from typing import Any

from .constants import PowerControl
from .hal import DEFAULT_ROUTING, PortMode, IORouting


class BusPlanError(ValueError):
    """Raised when a requested external bus route is invalid or unsafe."""


_BUS_I2C = "i2c"
_BUS_SPI = "spi"
_DEFAULT_I2C_FREQ_HZ = 400_000
_DEFAULT_SPI_FREQ_HZ = 1_000_000
_I2C_RESERVED_ADDRESSES = set(range(0x00, 0x08)) | set(range(0x78, 0x80))


@dataclass(frozen=True)
class BusRouteEntry:
    role: str
    io: int
    esp_gpio: int
    block: int
    io_block: int
    position: int
    mux_device: int
    mux_mask: int
    supply: str
    supply_idac: int
    efuse: str

    @classmethod
    def from_routing(cls, role: str, rt: IORouting, mode: PortMode) -> "BusRouteEntry":
        return cls(
            role=role,
            io=rt.io_num,
            esp_gpio=rt.esp_gpio,
            block=rt.block,
            io_block=rt.io_block,
            position=rt.position,
            mux_device=rt.mux_device,
            mux_mask=rt.mux_map[mode],
            supply=rt.supply.name,
            supply_idac=rt.supply_idac,
            efuse=rt.efuse.name,
        )

    def as_dict(self) -> dict[str, Any]:
        return {
            "role": self.role,
            "io": self.io,
            "esp_gpio": self.esp_gpio,
            "block": self.block,
            "io_block": self.io_block,
            "position": self.position,
            "mux_device": self.mux_device,
            "mux_mask": self.mux_mask,
            "supply": self.supply,
            "supply_idac": self.supply_idac,
            "efuse": self.efuse,
        }


@dataclass(frozen=True)
class BusRoutePlan:
    kind: str
    pins: dict[str, int]
    routes: tuple[BusRouteEntry, ...]
    mux_states: tuple[int, int, int, int]
    io_voltage: float
    supply_voltage: float
    frequency_hz: int
    pullups: str
    warnings: tuple[str, ...]
    side_effects: tuple[str, ...]
    reserved_addresses_skipped: tuple[int, ...] = ()

    @property
    def supplies(self) -> tuple[str, ...]:
        return tuple(dict.fromkeys(route.supply for route in self.routes))

    @property
    def efuses(self) -> tuple[str, ...]:
        return tuple(dict.fromkeys(route.efuse for route in self.routes))

    @property
    def esp_gpios(self) -> dict[str, int]:
        return {route.role: route.esp_gpio for route in self.routes}

    def as_dict(self) -> dict[str, Any]:
        return {
            "kind": self.kind,
            "pins": dict(self.pins),
            "routes": [route.as_dict() for route in self.routes],
            "mux_states": list(self.mux_states),
            "io_voltage": self.io_voltage,
            "supply_voltage": self.supply_voltage,
            "frequency_hz": self.frequency_hz,
            "pullups": self.pullups,
            "supplies": list(self.supplies),
            "efuses": list(self.efuses),
            "esp_gpios": self.esp_gpios,
            "warnings": list(self.warnings),
            "side_effects": list(self.side_effects),
            "reserved_addresses_skipped": [f"0x{addr:02X}" for addr in self.reserved_addresses_skipped],
        }


def _group_clear_mask(position: int) -> int:
    if position == 1:
        return 0x0F
    if position == 2:
        return 0x30
    return 0xC0


def _mux_state_for(routes: tuple[BusRouteEntry, ...]) -> tuple[int, int, int, int]:
    state = [0, 0, 0, 0]
    for route in routes:
        clear = _group_clear_mask(route.position)
        state[route.mux_device] = (state[route.mux_device] & ~clear) | route.mux_mask
    return tuple(state)  # type: ignore[return-value]


def _validate_io(role: str, io: int) -> IORouting:
    if io not in DEFAULT_ROUTING:
        raise BusPlanError(f"{role} IO must be 1-12, got {io!r}")
    rt = DEFAULT_ROUTING[io]
    if PortMode.DIGITAL_IN not in rt.valid_modes:
        raise BusPlanError(f"{role}=IO{io} cannot be used as a digital bus pin")
    return rt


def _validate_unique_pins(pins: dict[str, int]) -> None:
    values = list(pins.values())
    if len(values) != len(set(values)):
        raise BusPlanError("Bus roles must use distinct IO numbers")


def _electrical_warnings(
    *,
    io_voltage: float,
    supply_voltage: float,
    pullups: str,
    frequency_hz: int,
) -> list[str]:
    warnings: list[str] = []
    if abs(io_voltage - supply_voltage) > 0.05:
        warnings.append(
            "io_voltage and supply_voltage differ; verify the target expects separate logic and supply rails"
        )
    if pullups == "internal":
        warnings.append(
            "internal I2C pull-ups are weak; use external pull-ups for reliable 400 kHz operation"
        )
        if frequency_hz > 100_000:
            warnings.append("internal pull-ups requested above 100 kHz; firmware should clamp or reject this")
    return warnings


def _side_effects(plan_routes: tuple[BusRouteEntry, ...], io_voltage: float, supply_voltage: float) -> tuple[str, ...]:
    supplies = tuple(dict.fromkeys(route.supply for route in plan_routes))
    efuses = tuple(dict.fromkeys(route.efuse for route in plan_routes))
    return (
        f"set VLOGIC to {io_voltage:.3g} V",
        *(f"enable {supply} and set to {supply_voltage:.3g} V" for supply in supplies),
        *(f"enable {efuse}" for efuse in efuses),
        "enable level-shifter OE",
        "open conflicting MUX groups before closing requested bus paths",
    )


class BugBusterBusManager:
    """Side-effect-free bus planner attached to :class:`bugbuster.client.BugBuster`."""

    def __init__(self, client):
        self._client = client

    def plan_i2c(
        self,
        *,
        sda: int,
        scl: int,
        io_voltage: float,
        supply_voltage: float,
        frequency_hz: int = _DEFAULT_I2C_FREQ_HZ,
        pullups: str = "external",
        allow_split_supplies: bool = False,
    ) -> BusRoutePlan:
        pullups = pullups.lower()
        if pullups not in {"external", "internal", "off"}:
            raise BusPlanError("pullups must be 'external', 'internal', or 'off'")
        pins = {"sda": sda, "scl": scl}
        return self._plan(
            kind=_BUS_I2C,
            pins=pins,
            role_modes={"sda": PortMode.DIGITAL_IN, "scl": PortMode.DIGITAL_IN},
            io_voltage=io_voltage,
            supply_voltage=supply_voltage,
            frequency_hz=frequency_hz,
            pullups=pullups,
            allow_split_supplies=allow_split_supplies,
            reserved_addresses_skipped=tuple(sorted(_I2C_RESERVED_ADDRESSES)),
        )

    def plan_spi(
        self,
        *,
        sck: int,
        io_voltage: float,
        supply_voltage: float,
        mosi: int | None = None,
        miso: int | None = None,
        cs: int | None = None,
        frequency_hz: int = _DEFAULT_SPI_FREQ_HZ,
        allow_split_supplies: bool = False,
    ) -> BusRoutePlan:
        if mosi is None and miso is None:
            raise BusPlanError("SPI requires at least one of mosi or miso")
        pins = {"sck": sck}
        if mosi is not None:
            pins["mosi"] = mosi
        if miso is not None:
            pins["miso"] = miso
        if cs is not None:
            pins["cs"] = cs
        role_modes = {
            role: (PortMode.DIGITAL_IN if role == "miso" else PortMode.DIGITAL_OUT)
            for role in pins
        }
        return self._plan(
            kind=_BUS_SPI,
            pins=pins,
            role_modes=role_modes,
            io_voltage=io_voltage,
            supply_voltage=supply_voltage,
            frequency_hz=frequency_hz,
            pullups="off",
            allow_split_supplies=allow_split_supplies,
        )

    def sessions(self) -> list[dict[str, Any]]:
        """Return active external bus bindings."""
        return self.status()["sessions"]

    def status(self) -> dict[str, Any]:
        """Return active firmware external I2C/SPI binding status."""
        if self._client is None:
            return {"sessions": []}
        if getattr(self._client, "_usb", False):
            sessions: list[dict[str, Any]] = []
            # USB status command is not added yet; report host-side manager availability.
            return {"sessions": sessions, "transport": "usb"}
        raw = self._client._http_get("/bus/status")
        sessions = []
        i2c = raw.get("i2c", {})
        if i2c.get("ready"):
            sessions.append({"kind": "i2c", **i2c})
        spi = raw.get("spi", {})
        if spi.get("ready"):
            sessions.append({"kind": "spi", **spi})
        return {"sessions": sessions, "raw": raw, "transport": "http"}

    def setup_i2c(
        self,
        *,
        sda: int,
        scl: int,
        io_voltage: float,
        supply_voltage: float,
        frequency_hz: int = _DEFAULT_I2C_FREQ_HZ,
        pullups: str = "external",
        allow_split_supplies: bool = False,
    ) -> BusRoutePlan:
        """
        Apply the electrical route and bind the ESP32 external I2C peripheral.

        This performs the side effects described by :meth:`plan_i2c`: VLOGIC,
        VADJ, e-fuse, level-shifter OE, MUX state, then firmware I2C setup.
        """
        if pullups == "internal":
            import logging
            logging.getLogger("bugbuster").warning(
                "Internal pullups are weak (~45k). External pullups are highly recommended for I2C."
            )

        plan = self.plan_i2c(
            sda=sda,
            scl=scl,
            io_voltage=io_voltage,
            supply_voltage=supply_voltage,
            frequency_hz=frequency_hz,
            pullups=pullups,
            allow_split_supplies=allow_split_supplies,
        )
        self._apply_route(plan)
        self._client.ext_i2c_setup(
            sda_gpio=plan.esp_gpios["sda"],
            scl_gpio=plan.esp_gpios["scl"],
            frequency_hz=frequency_hz,
            pullups=pullups,
        )
        return plan

    configure_i2c = setup_i2c

    def i2c_scan(
        self,
        *,
        sda: int | None = None,
        scl: int | None = None,
        io_voltage: float = 3.3,
        supply_voltage: float = 3.3,
        frequency_hz: int = _DEFAULT_I2C_FREQ_HZ,
        pullups: str = "external",
        start_addr: int = 0x08,
        stop_addr: int = 0x77,
        skip_reserved: bool = True,
        timeout_ms: int = 50,
        allow_split_supplies: bool = False,
    ) -> dict[str, Any]:
        """
        Scan the external I2C bus, optionally setting up the route first.

        Pass ``sda`` and ``scl`` for one-shot setup+scan, or omit both to scan
        the currently configured firmware external I2C session.
        """
        plan = None
        if sda is not None or scl is not None:
            if sda is None or scl is None:
                raise BusPlanError("i2c_scan requires both sda and scl when setting up a route")
            plan = self.setup_i2c(
                sda=sda,
                scl=scl,
                io_voltage=io_voltage,
                supply_voltage=supply_voltage,
                frequency_hz=frequency_hz,
                pullups=pullups,
                allow_split_supplies=allow_split_supplies,
            )
        addresses = self._client.ext_i2c_scan(
            start_addr=start_addr,
            stop_addr=stop_addr,
            skip_reserved=skip_reserved,
            timeout_ms=timeout_ms,
        )
        return {
            "kind": "i2c_scan",
            "addresses": [f"0x{addr:02X}" for addr in addresses],
            "address_values": addresses,
            "count": len(addresses),
            "plan": plan.as_dict() if plan else None,
        }

    def i2c_write(self, address: int, data: bytes | bytearray | list[int], *, timeout_ms: int = 100) -> int:
        """Write bytes on the configured external I2C bus."""
        return self._client.ext_i2c_write(address, data, timeout_ms=timeout_ms)

    def i2c_read(self, address: int, length: int, *, timeout_ms: int = 100) -> bytes:
        """Read bytes on the configured external I2C bus."""
        return self._client.ext_i2c_read(address, length, timeout_ms=timeout_ms)

    def i2c_write_read(
        self,
        address: int,
        write_data: bytes | bytearray | list[int],
        read_length: int,
        *,
        timeout_ms: int = 100,
    ) -> bytes:
        """Perform a register-style write then repeated-start read."""
        return self._client.ext_i2c_write_read(address, write_data, read_length, timeout_ms=timeout_ms)

    def setup_spi(
        self,
        *,
        sck: int,
        io_voltage: float,
        supply_voltage: float,
        mosi: int | None = None,
        miso: int | None = None,
        cs: int | None = None,
        frequency_hz: int = _DEFAULT_SPI_FREQ_HZ,
        mode: int = 0,
        allow_split_supplies: bool = False,
    ) -> BusRoutePlan:
        """Apply the electrical route and bind the ESP32 external SPI peripheral."""
        if mode not in (0, 1, 2, 3):
            raise BusPlanError("SPI mode must be 0, 1, 2, or 3")
        plan = self.plan_spi(
            sck=sck,
            mosi=mosi,
            miso=miso,
            cs=cs,
            io_voltage=io_voltage,
            supply_voltage=supply_voltage,
            frequency_hz=frequency_hz,
            allow_split_supplies=allow_split_supplies,
        )
        self._apply_route(plan)
        gpios = plan.esp_gpios
        self._client.ext_spi_setup(
            sck_gpio=gpios["sck"],
            mosi_gpio=gpios.get("mosi"),
            miso_gpio=gpios.get("miso"),
            cs_gpio=gpios.get("cs"),
            frequency_hz=frequency_hz,
            mode=mode,
        )
        return plan

    configure_spi = setup_spi

    def spi_transfer(self, data: bytes | bytearray | list[int], *, timeout_ms: int = 100) -> bytes:
        """Transfer bytes on the configured external SPI bus."""
        return self._client.ext_spi_transfer(data, timeout_ms=timeout_ms)

    def defer_i2c_read(self, address: int, length: int, *, timeout_ms: int = 100) -> int:
        """Queue an I2C read on the ESP32 deferred bus worker."""
        return self._client.ext_job_submit_i2c_read(address, length, timeout_ms=timeout_ms)

    def defer_i2c_write_read(
        self,
        address: int,
        write_data: bytes | bytearray | list[int],
        read_length: int,
        *,
        timeout_ms: int = 100,
    ) -> int:
        """Queue an I2C write/read transaction on the ESP32 deferred bus worker."""
        return self._client.ext_job_submit_i2c_write_read(
            address,
            write_data,
            read_length,
            timeout_ms=timeout_ms,
        )

    def defer_spi_transfer(self, data: bytes | bytearray | list[int], *, timeout_ms: int = 100) -> int:
        """Queue an SPI transfer on the ESP32 deferred bus worker."""
        return self._client.ext_job_submit_spi_transfer(data, timeout_ms=timeout_ms)

    def deferred_result(self, job_id: int) -> dict[str, Any]:
        """Poll a deferred bus job and normalize bytes for JSON callers."""
        result = dict(self._client.ext_job_get(job_id))
        data = result.get("data")
        if isinstance(data, (bytes, bytearray)):
            result["data"] = list(data)
            result["data_hex"] = bytes(data).hex().upper()
        return result

    def spi_jedec_id(self, *, timeout_ms: int = 100) -> dict[str, Any]:
        """Read common SPI flash/device JEDEC ID using command 0x9F."""
        rx = self.spi_transfer([0x9F, 0x00, 0x00, 0x00], timeout_ms=timeout_ms)
        ident = bytes(rx[1:4]) if len(rx) >= 4 else b""
        return {
            "raw": list(rx),
            "manufacturer_id": ident[0] if len(ident) > 0 else None,
            "memory_type": ident[1] if len(ident) > 1 else None,
            "capacity": ident[2] if len(ident) > 2 else None,
            "jedec_id": ident.hex().upper(),
        }

    def _plan(
        self,
        *,
        kind: str,
        pins: dict[str, int],
        role_modes: dict[str, PortMode],
        io_voltage: float,
        supply_voltage: float,
        frequency_hz: int,
        pullups: str,
        allow_split_supplies: bool,
        reserved_addresses_skipped: tuple[int, ...] = (),
    ) -> BusRoutePlan:
        _validate_unique_pins(pins)
        routes = tuple(
            BusRouteEntry.from_routing(role, _validate_io(role, io), role_modes[role])
            for role, io in pins.items()
        )
        supplies = {route.supply for route in routes}
        if len(supplies) > 1 and not allow_split_supplies:
            names = ", ".join(sorted(supplies))
            raise BusPlanError(f"bus pins span multiple supplies ({names}); pass allow_split_supplies=True to override")

        warnings = _electrical_warnings(
            io_voltage=io_voltage,
            supply_voltage=supply_voltage,
            pullups=pullups,
            frequency_hz=frequency_hz,
        )
        return BusRoutePlan(
            kind=kind,
            pins=dict(pins),
            routes=routes,
            mux_states=_mux_state_for(routes),
            io_voltage=float(io_voltage),
            supply_voltage=float(supply_voltage),
            frequency_hz=int(frequency_hz),
            pullups=pullups,
            warnings=tuple(warnings),
            side_effects=_side_effects(routes, io_voltage, supply_voltage),
            reserved_addresses_skipped=reserved_addresses_skipped,
        )

    def _apply_route(self, plan: BusRoutePlan) -> None:
        if self._client is None:
            raise NotImplementedError("route application requires a BugBuster client")

        self._client.power_set(PowerControl.MUX, on=True)
        self._client.set_level_shifter_oe(on=True)
        self._client.idac_set_voltage(0, plan.io_voltage)

        for route in plan.routes:
            self._client.power_set(PowerControl[route.supply], on=True)
            self._client.idac_set_voltage(route.supply_idac, plan.supply_voltage)
            self._client.power_set(PowerControl[route.efuse], on=True)

        self._client.mux_set_all(list(plan.mux_states))


__all__ = [
    "BugBusterBusManager",
    "BusPlanError",
    "BusRouteEntry",
    "BusRoutePlan",
]
