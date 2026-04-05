"""
BugBuster MCP — Digital I/O tools.

Tools: read_digital, write_digital
"""

from __future__ import annotations
from .. import session
from ..safety import require_valid_io, require_io_mode, check_faults_post


def register(mcp) -> None:

    @mcp.tool()
    def read_digital(io: int) -> dict:
        """
        Read the logic level on a DIGITAL_IN or DIGITAL_IN_LOW IO port.

        The IO must be configured as DIGITAL_IN or DIGITAL_IN_LOW with
        configure_io first. All 12 IOs (1-12) support digital input.
        The voltage threshold is determined by the VLOGIC setting (default 3.3 V).

        Parameters:
        - io: IO number 1-12.

        Returns: io, state (bool — True = HIGH, False = LOW), voltage_level ("VLOGIC").
        """
        require_valid_io(io)
        hal = session.get_hal()
        from bugbuster.hal import PortMode
        require_io_mode(
            hal, io,
            {PortMode.DIGITAL_IN, PortMode.DIGITAL_IN_LOW},
            "read_digital",
        )
        state = hal.read_digital(io)
        return {
            "io":            io,
            "state":         state,
            "level":         "HIGH" if state else "LOW",
            "voltage_level": "VLOGIC",
        }

    @mcp.tool()
    def write_digital(io: int, state: bool) -> dict:
        """
        Set the logic level on a DIGITAL_OUT or DIGITAL_OUT_LOW IO port.

        The IO must be configured as DIGITAL_OUT or DIGITAL_OUT_LOW with
        configure_io first. All 12 IOs (1-12) support digital output.
        Output voltage is determined by the VLOGIC setting (default 3.3 V).

        Parameters:
        - io: IO number 1-12.
        - state: True = drive HIGH (VLOGIC), False = drive LOW (GND).

        Returns: io, state, success, warnings.
        """
        require_valid_io(io)
        hal = session.get_hal()
        from bugbuster.hal import PortMode
        require_io_mode(
            hal, io,
            {PortMode.DIGITAL_OUT, PortMode.DIGITAL_OUT_LOW},
            "write_digital",
        )
        hal.write_digital(io, state)
        warnings = check_faults_post(session.get_client())
        return {
            "io":       io,
            "state":    state,
            "level":    "HIGH" if state else "LOW",
            "success":  True,
            "warnings": warnings,
        }
