"""
BugBuster MCP — Advanced / low-level tools.

Tools: mux_control, register_access, idac_control

These tools expose the underlying hardware directly. Incorrect use can
damage connected devices or put the BugBuster in an inconsistent state.
"""

from __future__ import annotations
from .. import session


def register(mcp) -> None:

    @mcp.tool()
    def mux_control(
        action:               str,
        i_understand_the_risk: bool = False,
        device:               int  = 0,
        switch:               int  = 0,
        closed:               bool = False,
        states:               list = None,
    ) -> dict:
        """
        Direct control of the ADGS2414D analog MUX switch matrix.

        WARNING: This tool bypasses HAL safety checks. Setting incorrect
        MUX states can short-circuit signals, damage connected devices, or
        put BugBuster in an inconsistent state. Use configure_io instead for
        normal IO routing.

        You MUST pass i_understand_the_risk=True to use this tool.

        The MUX matrix has 5 ADGS2414D chips (32 main switches + 8 self-test):
        - Devices 0-3: Main routing matrix (8 switches each)
        - Device 4: Self-test / calibration routing

        Parameters:
        - action: "get" to read current state, "set_switch" to control one
                  switch, "set_all" to write all switch states.
        - i_understand_the_risk: Must be True.
        - device: ADGS2414D device index (0-4) for set_switch.
        - switch: Switch index within device (0-7) for set_switch.
        - closed: True = switch closed (connected), False = open (disconnected).
        - states: List of 4 integers (one per main device) for set_all action.

        Returns: action, states (for get), success.
        """
        if not i_understand_the_risk:
            raise ValueError(
                "mux_control requires i_understand_the_risk=True. "
                "Incorrect MUX settings can short-circuit signals or damage hardware. "
                "Use configure_io for normal IO routing."
            )

        bb = session.get_client()
        action_lower = action.lower()

        if action_lower == "get":
            s = bb.mux_get()
            return {
                "action": "get",
                "states": s,
                "note": "Each integer is a bitmask: bit N = switch N state (1=closed).",
            }
        elif action_lower == "set_switch":
            bb.mux_set_switch(device=device, switch=switch, closed=closed)
            return {"action": "set_switch", "device": device, "switch": switch, "closed": closed, "success": True}
        elif action_lower == "set_all":
            if not states or len(states) != 4:
                raise ValueError("set_all requires states=[s0, s1, s2, s3] (4 integers).")
            bb.mux_set_all(states)
            return {"action": "set_all", "states": states, "success": True}
        else:
            raise ValueError(f"Unknown action {action!r}. Use 'get', 'set_switch', or 'set_all'.")

    @mcp.tool()
    def register_access(
        action:                str,
        register_address:      int,
        value:                 int  = 0,
        i_understand_the_risk: bool = False,
    ) -> dict:
        """
        Read or write raw AD74416H SPI registers. USB transport only.

        WARNING: Writing incorrect register values can damage the AD74416H
        or connected devices. Only use this for low-level debugging or
        accessing registers not covered by other tools.

        You MUST pass i_understand_the_risk=True to use this tool.

        Parameters:
        - action: "read" or "write".
        - register_address: AD74416H register address (see datasheet).
        - value: Value to write (for write action, 16-bit).
        - i_understand_the_risk: Must be True.

        Returns: action, register_address, value (read), success.
        """
        if not i_understand_the_risk:
            raise ValueError(
                "register_access requires i_understand_the_risk=True. "
                "Raw register writes can damage the AD74416H or connected devices."
            )
        if not session.is_usb():
            raise RuntimeError("register_access is only available over USB transport.")

        bb = session.get_client()
        action_lower = action.lower()

        if action_lower == "read":
            val = bb.register_read(register_address)
            return {"action": "read", "register_address": register_address, "value": val}
        elif action_lower == "write":
            bb.register_write(register_address, value)
            return {"action": "write", "register_address": register_address, "value": value, "success": True}
        else:
            raise ValueError(f"Unknown action {action!r}. Use 'read' or 'write'.")

    @mcp.tool()
    def idac_control(
        action:  str,
        channel: int   = 0,
        voltage: float = None,
        code:    int   = None,
    ) -> dict:
        """
        Direct control of the DS4424 IDAC (current DAC) that adjusts power supply voltages.

        The DS4424 has 4 channels that inject current into the LTM8063 feedback
        networks to adjust VADJ1, VADJ2, and VLOGIC output voltages.

        In normal use, call set_supply_voltage instead. Use this tool for
        diagnostics or fine-grained voltage trimming.

        Parameters:
        - action: "status" to read state, "set_voltage" to set by voltage,
                  "set_code" to write raw DAC code.
        - channel: IDAC channel (0=VLOGIC, 1=VADJ1, 2=VADJ2, 3=unused).
        - voltage: Target voltage for set_voltage action.
        - code: Raw DAC code (0-127) for set_code action.

        Returns: action, channel, status/result.
        """
        bb = session.get_client()
        action_lower = action.lower()

        if action_lower == "status":
            st = bb.idac_get_status()
            return {"action": "status", "idac": st}
        elif action_lower == "set_voltage":
            if voltage is None:
                raise ValueError("set_voltage requires voltage parameter.")
            bb.idac_set_voltage(channel, voltage)
            return {"action": "set_voltage", "channel": channel, "voltage": voltage, "success": True}
        elif action_lower == "set_code":
            if code is None:
                raise ValueError("set_code requires code parameter (0-127).")
            if not (0 <= code <= 127):
                raise ValueError(f"IDAC code must be 0-127, got {code}.")
            bb.idac_set_code(channel, code)
            return {"action": "set_code", "channel": channel, "code": code, "success": True}
        else:
            raise ValueError(
                f"Unknown action {action!r}. Use 'status', 'set_voltage', or 'set_code'."
            )
