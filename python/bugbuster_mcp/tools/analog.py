"""
BugBuster MCP — Analog I/O tools.

Tools: read_voltage, read_current, read_resistance, write_voltage, write_current
"""

from __future__ import annotations
from .. import session
from ..safety import (
    require_analog_io, validate_dac_voltage, validate_dac_current,
    require_io_mode, check_faults_post,
)


def register(mcp) -> None:

    @mcp.tool()
    def read_voltage(io: int) -> dict:
        """
        Read the voltage on an ANALOG_IN IO port.

        The IO must be configured as ANALOG_IN with configure_io first.
        Uses the 24-bit ADC (AD74416H) for high-accuracy measurement.

        Parameters:
        - io: IO number — must be 3, 6, 9, or 12 (analog-capable IOs).

        Returns: io, voltage_v (float), unit ("V").
        """
        require_analog_io(io, "read_voltage")
        hal = session.get_hal()
        from bugbuster.hal import PortMode
        require_io_mode(hal, io, PortMode.ANALOG_IN, "read_voltage")
        v = hal.read_voltage(io)
        return {"io": io, "voltage_v": round(v, 6), "unit": "V"}

    @mcp.tool()
    def read_current(io: int) -> dict:
        """
        Read the loop current on a CURRENT_IN IO port.

        The IO must be configured as CURRENT_IN with configure_io first.
        Measures 4-20 mA loop current using the AD74416H current input mode.

        Parameters:
        - io: IO number — must be 3, 6, 9, or 12.

        Returns: io, current_ma (float), unit ("mA").
        """
        require_analog_io(io, "read_current")
        hal = session.get_hal()
        from bugbuster.hal import PortMode
        require_io_mode(hal, io, PortMode.CURRENT_IN, "read_current")
        mA = hal.read_current(io)
        return {"io": io, "current_ma": round(mA, 6), "unit": "mA"}

    @mcp.tool()
    def read_resistance(
        io:        int,
        as_temp:   str = "",
    ) -> dict:
        """
        Measure resistance or temperature on an RTD IO port.

        The IO must be configured as RTD with configure_io first.
        Drives a precision excitation current (1 mA) and measures the
        resulting voltage to compute resistance.

        Parameters:
        - io: IO number — must be 3, 6, 9, or 12.
        - as_temp: Optional temperature conversion.
            "" or "none" — return raw resistance in ohms.
            "pt100"      — convert to °C using PT100 curve (100 Ω nominal).
            "pt1000"     — convert to °C using PT1000 curve (1000 Ω nominal).

        Returns: io, resistance_ohm, temperature_c (if as_temp set), unit.
        """
        require_analog_io(io, "read_resistance")
        hal = session.get_hal()
        from bugbuster.hal import PortMode
        require_io_mode(hal, io, PortMode.RTD, "read_resistance")

        from typing import Any
        result: dict[str, Any] = {"io": io}
        ohms = hal.read_resistance(io)
        result["resistance_ohm"] = round(ohms, 4)

        mode = as_temp.lower().strip()
        if mode in ("pt100",):
            temp = hal.read_temperature_pt100(io)
            result["temperature_c"] = round(temp, 3)
            result["unit"] = "°C (PT100)"
        elif mode in ("pt1000",):
            temp = hal.read_temperature_pt1000(io)
            result["temperature_c"] = round(temp, 3)
            result["unit"] = "°C (PT1000)"
        else:
            result["unit"] = "Ω"

        return result

    @mcp.tool()
    def write_voltage(
        io:      int,
        voltage: float,
        bipolar: bool = False,
    ) -> dict:
        """
        Set a DAC voltage output on an ANALOG_OUT IO port.

        The IO must be configured as ANALOG_OUT with configure_io first.
        Uses the 16-bit AD74416H DAC.

        Parameters:
        - io: IO number — must be 3, 6, 9, or 12.
        - voltage: Output voltage in volts.
            Unipolar mode (default): 0.0 to 12.0 V.
            Bipolar mode: -12.0 to +12.0 V.
        - bipolar: Set True if the IO was configured with bipolar=True.

        Returns: io, voltage_v, success, warnings.
        """
        require_analog_io(io, "write_voltage")
        validate_dac_voltage(voltage, bipolar=bipolar)
        hal = session.get_hal()
        from bugbuster.hal import PortMode
        require_io_mode(hal, io, PortMode.ANALOG_OUT, "write_voltage")
        hal.write_voltage(io, voltage, bipolar=bipolar)
        warnings = check_faults_post(session.get_client())
        return {
            "io":       io,
            "voltage_v": voltage,
            "success":  True,
            "warnings": warnings,
        }

    @mcp.tool()
    def write_current(
        io:               int,
        current_ma:       float,
        allow_full_range: bool = False,
    ) -> dict:
        """
        Set a current source output on a CURRENT_OUT IO port.

        The IO must be configured as CURRENT_OUT with configure_io first.
        Drives a 4-20 mA current loop using the AD74416H current output mode.

        Safety: Defaults to 8 mA maximum. Use allow_full_range=True for up to 25 mA.

        Parameters:
        - io: IO number — must be 1, 4, 7, or 10.
        - current_ma: Output current in milliamps (0.0 to 8.0 mA by default).
        - allow_full_range: Set True to allow up to 25 mA (full 4-20 mA range).

        Returns: io, current_ma, success, warnings.
        """
        require_analog_io(io, "write_current")
        validate_dac_current(current_ma, allow_full_range=allow_full_range)
        hal = session.get_hal()
        from bugbuster.hal import PortMode
        require_io_mode(hal, io, PortMode.CURRENT_OUT, "write_current")
        hal.write_current(io, current_ma)
        warnings = check_faults_post(session.get_client())
        return {
            "io":         io,
            "current_ma": current_ma,
            "success":    True,
            "warnings":   warnings,
        }
