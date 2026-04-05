"""
BugBuster MCP — Debug and communication tools.

Tools: setup_serial_bridge, setup_swd, uart_config
"""

from __future__ import annotations
from .. import session
from ..safety import require_valid_io, require_hat


def register(mcp) -> None:

    @mcp.tool()
    def setup_serial_bridge(
        tx_io:    int,
        rx_io:    int,
        baudrate: int = 115200,
        bridge:   int = 0,
    ) -> dict:
        """
        Route the UART serial bridge to two IO ports.

        Configures the MUX to connect the specified IOs to their ESP32 GPIO
        pins and sets up the UART peripheral. Use this to communicate with
        a target device's serial port.

        After setup, the serial data appears on USB CDC #1 (the second
        virtual serial port). Use any terminal or serial reader on that port.

        Parameters:
        - tx_io: IO number (1-12) to use as UART TX (data out to target).
        - rx_io: IO number (1-12) to use as UART RX (data in from target).
        - baudrate: Baud rate (default 115200). Common values: 9600, 38400,
                    57600, 115200, 230400, 921600.
        - bridge: Bridge index (0 or 1, default 0).

        Note: TX and RX must be different IOs.
        Note: The VLOGIC voltage determines the logic level (default 3.3 V).
              Use set_supply_voltage(0, voltage) to change it.

        Returns: tx_io, rx_io, baudrate, bridge, success.
        """
        require_valid_io(tx_io)
        require_valid_io(rx_io)
        if tx_io == rx_io:
            raise ValueError("TX IO and RX IO must be different.")

        VALID_BAUDS = {300, 600, 1200, 2400, 4800, 9600, 19200, 38400, 57600,
                       115200, 230400, 460800, 921600, 1000000, 2000000, 3000000}
        if baudrate not in VALID_BAUDS:
            raise ValueError(
                f"Baudrate {baudrate} is not a standard value. "
                f"Use one of: {sorted(VALID_BAUDS)}"
            )
        if bridge not in (0, 1):
            raise ValueError("Bridge must be 0 or 1.")

        hal = session.get_hal()
        hal.set_serial(tx=tx_io, rx=rx_io, baudrate=baudrate, bridge=bridge)

        return {
            "tx_io":    tx_io,
            "rx_io":    rx_io,
            "baudrate": baudrate,
            "bridge":   bridge,
            "success":  True,
            "note":     (
                f"Serial bridge {bridge} configured. "
                f"Connect to USB CDC #1 (second virtual COM port) to communicate with the target."
            ),
        }

    @mcp.tool()
    def setup_swd(
        target_voltage_mv: int = 3300,
        connector:         int = 0,
    ) -> dict:
        """
        Configure the SWD debug probe on the HAT expansion board.

        Sets up the RP2040 HAT as a CMSIS-DAP compliant SWD debug probe
        targeting an ARM Cortex-M microcontroller. The HAT handles SWCLK
        and SWDIO automatically.

        Requires the HAT expansion board (RP2040).

        Parameters:
        - target_voltage_mv: Target MCU IO voltage in millivolts (default 3300 mV = 3.3 V).
                              Common values: 1800, 2500, 3300, 5000.
        - connector: HAT target connector to use (0 = connector A, 1 = connector B).

        After calling this tool:
        - The HAT appears as a CMSIS-DAP USB device to the host.
        - Use OpenOCD, pyOCD, or arm-none-eabi-gdb to program/debug the target.
        - The UART bridge on the HAT (debugprobe UART) provides serial output.

        Returns: success, target_voltage_mv, connector, message.
        """
        if target_voltage_mv not in (1800, 2500, 3300, 5000):
            # Allow any reasonable value but warn for non-standard ones
            if not (1200 <= target_voltage_mv <= 5500):
                raise ValueError(
                    f"target_voltage_mv {target_voltage_mv} is outside 1200-5500 mV range."
                )

        bb = session.get_client()
        require_hat(bb)

        ok = bb.hat_setup_swd(
            target_voltage_mv=target_voltage_mv,
            connector=connector,
        )
        if not ok:
            raise RuntimeError(
                "HAT did not acknowledge SWD setup. "
                "Check that the HAT firmware supports this command."
            )

        return {
            "success":           True,
            "target_voltage_mv": target_voltage_mv,
            "connector":         connector,
            "message": (
                f"SWD probe configured. Target voltage: {target_voltage_mv / 1000:.2f} V, "
                f"connector: {'A' if connector == 0 else 'B'}. "
                "Connect via OpenOCD or pyOCD using CMSIS-DAP transport."
            ),
        }

    @mcp.tool()
    def uart_config(
        bridge_id: int  = 0,
        baudrate:  int  = None,
        data_bits: int  = None,
        parity:    str  = None,
        stop_bits: int  = None,
    ) -> dict:
        """
        Read or update UART bridge configuration.

        Called with no optional parameters: returns the current UART config.
        Called with parameters: updates the specified settings.

        Parameters:
        - bridge_id: Bridge index (0 or 1, default 0).
        - baudrate: New baud rate (optional, leave None to keep current).
        - data_bits: Data bits — 5, 6, 7, or 8 (optional).
        - parity: Parity — "none", "even", "odd" (optional).
        - stop_bits: Stop bits — 1 or 2 (optional).

        Returns: bridge_id, current_config, updated (bool).
        """
        bb = session.get_client()
        current = bb.get_uart_config()

        if not (baudrate or data_bits or parity or stop_bits):
            return {
                "bridge_id":    bridge_id,
                "config":       current,
                "updated":      False,
            }

        # Apply updates
        cfg = current[bridge_id] if isinstance(current, list) and bridge_id < len(current) else {}

        new_baudrate  = baudrate   if baudrate   else cfg.get("baudrate", 115200)
        new_data_bits = data_bits  if data_bits  else cfg.get("data_bits", 8)
        new_stop_bits = stop_bits  if stop_bits  else cfg.get("stop_bits", 1)

        _PARITY_MAP = {"none": 0, "even": 1, "odd": 2}
        if parity:
            parity_key = parity.lower()
            if parity_key not in _PARITY_MAP:
                raise ValueError(f"parity must be 'none', 'even', or 'odd'.")
            new_parity = _PARITY_MAP[parity_key]
        else:
            new_parity = cfg.get("parity", 0)

        bb.set_uart_config(
            bridge_id=bridge_id,
            uart_num=bridge_id + 1,
            baudrate=new_baudrate,
            data_bits=new_data_bits,
            parity=new_parity,
            stop_bits=new_stop_bits,
        )

        return {
            "bridge_id": bridge_id,
            "config": {
                "baudrate":  new_baudrate,
                "data_bits": new_data_bits,
                "parity":    parity or "unchanged",
                "stop_bits": new_stop_bits,
            },
            "updated": True,
        }
