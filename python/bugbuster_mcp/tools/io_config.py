"""
BugBuster MCP — IO configuration tools.

Tools: configure_io, set_supply_voltage, reset_device
"""

from __future__ import annotations
import logging
from .. import session
from ..safety import (
    require_valid_io, require_analog_io,
    validate_vadj_voltage,
)

log = logging.getLogger(__name__)

_PORT_MODE_NAMES = {
    "DISABLED":        0,
    "ANALOG_IN":       1,
    "ANALOG_OUT":      2,
    "CURRENT_IN":      3,
    "CURRENT_OUT":     4,
    "DIGITAL_IN":      5,
    "DIGITAL_OUT":     6,
    "DIGITAL_IN_LOW":  7,
    "DIGITAL_OUT_LOW": 8,
    "RTD":             9,
    "HART":            10,
    "HAT":             11,
}


def register(mcp) -> None:

    @mcp.tool()
    def configure_io(
        io:      int,
        mode:    str,
        bipolar: bool = False,
    ) -> dict:
        """
        Configure a physical IO port (1-12) to the specified operating mode.
        Equivalent to Arduino pinMode(). Must be called before read/write operations.

        Parameters:
        - io: IO number 1-12. IOs 1, 4, 7, 10 support analog modes; others are digital-only.
        - mode: Operating mode string. One of:
            DISABLED        — Safe high-impedance state (default)
            ANALOG_IN       — Voltage input (0-12 V, 24-bit ADC). IOs 1,4,7,10 only.
            ANALOG_OUT      — Voltage output (0-12 V DAC). IOs 1,4,7,10 only.
            CURRENT_IN      — 4-20 mA input (externally powered). IOs 1,4,7,10 only.
            CURRENT_OUT     — 4-20 mA current source. IOs 1,4,7,10 only.
            DIGITAL_IN      — GPIO input (high-drive level shifter).
            DIGITAL_OUT     — GPIO output (high-drive level shifter).
            DIGITAL_IN_LOW  — GPIO input (low-drive level shifter).
            DIGITAL_OUT_LOW — GPIO output (low-drive level shifter).
            RTD             — Resistance/PT100/PT1000 measurement. IOs 1,4,7,10 only.
            HART            — HART modem overlay (4-20 mA with HART). IOs 1,4,7,10 only.
            HAT             — Route IO to HAT expansion connector. IOs 1,4,7,10 only.
        - bipolar: Enable ±12 V range for ANALOG_IN/ANALOG_OUT (default: 0-12 V only).

        Returns: io, mode, success, warnings.
        """
        require_valid_io(io)
        mode_upper = mode.upper()
        if mode_upper not in _PORT_MODE_NAMES:
            raise ValueError(
                f"Unknown mode {mode!r}. Valid modes: {', '.join(_PORT_MODE_NAMES)}"
            )

        # Analog modes require analog-capable IO
        analog_modes = {"ANALOG_IN", "ANALOG_OUT", "CURRENT_IN", "CURRENT_OUT",
                        "RTD", "HART", "HAT"}
        if mode_upper in analog_modes:
            require_analog_io(io, mode_upper)

        hal = session.get_hal()
        from bugbuster.hal import PortMode
        port_mode = PortMode(_PORT_MODE_NAMES[mode_upper])

        hal.configure(io, port_mode, bipolar=bipolar)

        warnings = []
        # Post-configure fault check
        try:
            from ..safety import check_faults_post
            warnings = check_faults_post(session.get_client())
        except Exception:
            pass

        return {
            "io":       io,
            "mode":     mode_upper,
            "bipolar":  bipolar,
            "success":  True,
            "warnings": warnings,
        }

    @mcp.tool()
    def set_supply_voltage(
        rail:    int,
        voltage: float,
        confirm: bool = False,
    ) -> dict:
        """
        Set the adjustable DC supply voltage for an IO group.

        Parameters:
        - rail: Supply rail to configure.
            1 = VADJ1 (3-15 V) — powers IOs 1-6 (Blocks 1 & 2).
            2 = VADJ2 (3-15 V) — powers IOs 7-12 (Blocks 3 & 4).
        - voltage: Target voltage in volts (3.0-15.0 V).
        - confirm: Must be True for voltages above 12 V (safety gate).

        Note: VLOGIC (the logic level for digital IOs) is fixed at server
        startup via --vlogic and cannot be changed at runtime.

        Returns: rail, voltage, success.
        """
        if rail == 0:
            raise ValueError(
                "VLOGIC cannot be changed by AI tools. "
                f"It is fixed at {session.get_vlogic():.1f} V "
                "(set via --vlogic at server startup)."
            )
        elif rail in (1, 2):
            validate_vadj_voltage(voltage, index=rail, confirm=confirm)
            hal = session.get_hal()
            hal.set_voltage(rail=rail, voltage=voltage)
            warnings = []
            try:
                from ..safety import check_faults_post
                warnings = check_faults_post(session.get_client())
            except Exception:
                pass
            return {
                "rail":     f"VADJ{rail}",
                "voltage":  voltage,
                "success":  True,
                "warnings": warnings,
            }
        else:
            raise ValueError(
                f"Invalid rail {rail}. Use 0 (VLOGIC), 1 (VADJ1), or 2 (VADJ2)."
            )

    @mcp.tool()
    def reset_device() -> dict:
        """
        Safely reset the BugBuster to a known state.

        This performs:
        1. HAL shutdown — disables all outputs, opens all MUX switches,
           disables power rails and e-fuses.
        2. AD74416H device reset — all channels to HIGH_IMP.
        3. HAL re-initialization — re-enables ±15 V analog supply and MUX.

        Safe to call at any time. Use this if the device is in an unknown
        state or after a fault condition.

        Returns: success, message.
        """
        from .. import session as _sess
        _sess.reset_session()
        # Re-initialize
        session.get_hal()
        return {
            "success": True,
            "message": "Device reset and HAL re-initialized. All IOs are now DISABLED.",
        }
