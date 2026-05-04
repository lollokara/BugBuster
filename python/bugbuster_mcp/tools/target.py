"""
BugBuster MCP — Target control tools.

Tools: target_power_up, enter_bootloader, release_bootloader

These tools control the DUT power rail, eFuse, BOOT pin, and UART bridge
as an atomic, correctly sequenced operation — avoiding the eFuse trip that
occurs when the rail and eFuse are enabled back-to-back.
"""

from __future__ import annotations
import time
from .. import session


def register(mcp) -> None:

    @mcp.tool()
    def target_power_up(
        supply_voltage: float = 5.0,
        rail:           int   = 1,
        settle_ms:      int   = 500,
    ) -> dict:
        """
        Safely power up the target device on VADJ1 or VADJ2.

        Sequence:
        1. Disable eFuse for the rail (clears any previous trip).
        2. Enable VADJ rail and set voltage.
        3. Wait settle_ms milliseconds for the rail to stabilise.
        4. Enable eFuse.

        This avoids the capacitive-inrush eFuse trip that happens when the
        rail and eFuse are enabled simultaneously.

        Parameters:
        - supply_voltage: Target rail voltage in volts (3.0 – 15.0). Default 5.0.
        - rail: 1 = VADJ1 (IOs 1-6), 2 = VADJ2 (IOs 7-12). Default 1.
        - settle_ms: Milliseconds to wait between rail enable and eFuse enable.
                     Increase for targets with large input capacitance. Default 500.

        Returns: rail, voltage, efuse, success, warnings.
        """
        from bugbuster.constants import PowerControl
        bb = session.get_client()

        efuse_ctrl = PowerControl.EFUSE1 if rail == 1 else PowerControl.EFUSE2
        vadj_ctrl  = PowerControl.VADJ1  if rail == 1 else PowerControl.VADJ2
        idac_ch    = 1                   if rail == 1 else 2

        # Step 1 — disable eFuse (clears any latched trip)
        bb.power_set(efuse_ctrl, on=False)

        # Step 2 — enable rail and set voltage
        bb.power_set(vadj_ctrl, on=True)
        bb.idac_set_voltage(idac_ch, supply_voltage)

        # Step 3 — wait for rail to settle
        time.sleep(settle_ms / 1000.0)

        # Step 4 — enable eFuse
        bb.power_set(efuse_ctrl, on=True)

        # Brief check
        time.sleep(0.1)
        status = bb.get_status()
        power  = status.get("power", {})

        pg_key = "vadj1_pg" if rail == 1 else "vadj2_pg"
        ef_faults = power.get("efuse_faults", [False, False, False, False])
        ef_tripped = ef_faults[0] if rail == 1 else ef_faults[1]

        warnings = []
        if not power.get(pg_key, True):
            warnings.append(f"VADJ{rail} power-good signal not asserted — check load.")
        if ef_tripped:
            warnings.append(f"eFuse{rail} tripped — possible overcurrent. Check wiring.")

        return {
            "rail":     f"VADJ{rail}",
            "voltage":  supply_voltage,
            "efuse":    f"EFUSE{rail}",
            "success":  not ef_tripped,
            "warnings": warnings,
        }

    @mcp.tool()
    def enter_bootloader(
        boot_io:        int   = 2,
        tx_io:          int   = 1,
        rx_io:          int   = 3,
        baudrate:       int   = 115200,
        supply_voltage: float = 5.0,
        rail:           int   = 1,
        settle_ms:      int   = 500,
    ) -> dict:
        """
        Enter the ESP32-C6 (or any ROM UART bootloader) download mode.

        Sequence:
        1. Assert BOOT low on boot_io (GPIO0 / BOOT pin = active-low entry).
        2. Power-cycle eFuse to reset the target while BOOT is held low.
        3. Configure the UART bridge: tx_io → target RX, rx_io → target TX.
        4. Release BOOT high so subsequent normal resets boot normally.

        The UART bridge is left enabled. esptool should be pointed at
        USB CDC #1 (/dev/cu.usbmodemXXXXX63) after this call.

        Parameters:
        - boot_io:        BugBuster IO driving the target BOOT/GPIO0 pin. Default 2.
        - tx_io:          BugBuster IO connected to the target RX pin. Default 1.
        - rx_io:          BugBuster IO connected to the target TX pin. Default 3.
        - baudrate:       UART baud rate. Default 115200.
        - supply_voltage: Target supply voltage (V). Default 5.0.
        - rail:           VADJ rail (1 or 2). Default 1.
        - settle_ms:      Rail settle delay in ms. Default 500.

        Returns: success, uart_bridge, boot_io, warnings.
        """
        from bugbuster.constants import PowerControl
        from bugbuster.hal import DEFAULT_ROUTING

        bb  = session.get_client()
        hal = session.get_hal()

        efuse_ctrl = PowerControl.EFUSE1 if rail == 1 else PowerControl.EFUSE2
        vadj_ctrl  = PowerControl.VADJ1  if rail == 1 else PowerControl.VADJ2
        idac_ch    = 1                   if rail == 1 else 2

        # IO -> GPIO mapping (firmware UART_IO_GPIO_MAP)
        _ROUTING = {1: 4, 2: 2, 3: 1, 4: 7, 5: 6, 6: 5,
                    7: 8, 8: 9, 9: 10, 10: 11, 11: 12, 12: 13}
        tx_gpio = _ROUTING.get(tx_io)
        rx_gpio = _ROUTING.get(rx_io)
        boot_gpio = _ROUTING.get(boot_io)
        if tx_gpio is None or rx_gpio is None or boot_gpio is None:
            raise ValueError(f"Unsupported IO numbers: tx_io={tx_io}, rx_io={rx_io}, boot_io={boot_io}")

        # MUX switch masks (from hal.py _SW_* constants)
        _SW_A_HIGH = 0x01   # Group A (position 1) — analog-capable IOs (3,6,9,12)
        _SW_B_HIGH = 0x10   # Group B (position 2)
        _SW_C_HIGH = 0x40   # Group C (position 3)
        _GROUP_MASK = {1: 0x0F, 2: 0x30, 3: 0xC0}
        _DRIVE_MASK = {1: _SW_A_HIGH, 2: _SW_B_HIGH, 3: _SW_C_HIGH}

        def _mux_set_io(io_num, drive):
            """Set MUX for one IO without touching power rails.
            drive=True  → connect ESP GPIO to terminal (TX / BOOT out)
            drive=False → same switch for input (RX) — same bit, different GPIO dir
            Both TX and RX use the same MUX switch (ESP_HIGH); direction is set by
            the ESP GPIO matrix via uart_set_pin / set_gpio_value."""
            rt = DEFAULT_ROUTING[io_num]
            mask = _DRIVE_MASK[rt.position] if drive else _DRIVE_MASK[rt.position]
            cur = hal._mux_state[rt.mux_device]
            cur = (cur & ~_GROUP_MASK[rt.position]) | mask
            hal._mux_state[rt.mux_device] = cur
            bb.mux_set_all(hal._mux_state)

        # 1 — Assert BOOT low BEFORE any power reaches the target.
        #     Drive the BOOT GPIO low through the MUX (VLOGIC-powered, no VADJ needed).
        _mux_set_io(boot_io, drive=True)
        bb.set_gpio_value(boot_gpio, False)   # drive GPIO low through level-shifter

        # 2 — Configure UART bridge (pure UART register + GPIO matrix writes).
        bb.set_uart_config(
            bridge_id=0, uart_num=1,
            tx_pin=tx_gpio, rx_pin=rx_gpio,
            baudrate=baudrate,
            data_bits=8, parity=0, stop_bits=0,
            enabled=True,
        )

        # 3 — Firmware's bus_planner clobbers the TX MUX switch (sets both TX and RX
        #     as digital inputs). Re-assert correct MUX state for all three IOs.
        _mux_set_io(tx_io,   drive=True)   # UART TX out → target RX
        _mux_set_io(rx_io,   drive=False)  # UART RX in  ← target TX
        _mux_set_io(boot_io, drive=True)   # BOOT still held low

        # 4 — Now power up: VADJ first, wait to settle, then eFuse.
        bb.power_set(efuse_ctrl, on=False)
        bb.power_set(vadj_ctrl, on=True)
        bb.idac_set_voltage(idac_ch, supply_voltage)
        time.sleep(settle_ms / 1000.0)
        bb.power_set(efuse_ctrl, on=True)

        # 5 — Leave BOOT low; esptool will connect while it's held low.
        # Call release_bootloader() after flashing to reboot into the app.
        # (Do NOT release here.)

        # Check faults
        warnings = []
        status = bb.get_status()
        power  = status.get("power", {})
        ef_faults = power.get("efuse_faults", [False, False, False, False])
        if ef_faults[0] if rail == 1 else ef_faults[1]:
            warnings.append(f"eFuse{rail} tripped after bootloader entry — check wiring.")

        return {
            "success": not warnings,
            "uart_bridge": {
                "bridge_id": 0,
                "tx_io": tx_io, "tx_gpio": tx_gpio,
                "rx_io": rx_io, "rx_gpio": rx_gpio,
                "baudrate": baudrate,
                "note": "Serial bridge active on USB CDC #1 (second virtual COM port).",
            },
            "boot_io":  boot_io,
            "boot_pin": "LOW (held — call release_bootloader after flashing)",
            "warnings": warnings,
        }

    @mcp.tool()
    def release_bootloader(
        boot_io: int = 2,
        rail:    int = 1,
    ) -> dict:
        """
        Release the BOOT pin and power-cycle the target to boot normally.

        Call this after flashing to reboot into the application.

        Parameters:
        - boot_io: BugBuster IO connected to the target BOOT/GPIO0 pin. Default 2.
        - rail:    VADJ rail (1 or 2). Default 1.

        Returns: success, boot_io.
        """
        from bugbuster.constants import PowerControl
        from bugbuster.hal import PortMode

        bb  = session.get_client()
        hal = session.get_hal()

        efuse_ctrl = PowerControl.EFUSE1 if rail == 1 else PowerControl.EFUSE2

        hal.configure(boot_io, PortMode.DIGITAL_OUT)
        hal.write_digital(boot_io, True)
        time.sleep(0.1)

        bb.power_set(efuse_ctrl, on=False)
        time.sleep(0.3)
        bb.power_set(efuse_ctrl, on=True)
        time.sleep(0.5)

        return {
            "success":  True,
            "boot_io":  boot_io,
            "boot_pin": "HIGH",
            "note":     "Target power-cycled into normal boot mode.",
        }
