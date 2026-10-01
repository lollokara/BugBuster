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
from ..safety import validate_vadj_voltage


def _rail_controls(rail: int):
    """(e-fuse controls, VADJ control, IDAC channel) for a VADJ rail.

    VADJ1 feeds EFUSE1 + EFUSE2 (IO 1-6), VADJ2 feeds EFUSE3 + EFUSE4 (IO 7-12),
    per the firmware route table in bus_planner.cpp.
    """
    from bugbuster.constants import PowerControl as P
    if rail == 1:
        return (P.EFUSE1, P.EFUSE2), P.VADJ1, 1
    if rail == 2:
        return (P.EFUSE3, P.EFUSE4), P.VADJ2, 2
    raise ValueError(f"rail must be 1 or 2, got {rail!r}")


def _rail_warnings(bb, rail: int) -> list[str]:
    """E-fuse trips and missing power-good for one rail, from the PCA9535 status."""
    st = bb.power_get_status()
    faults = st.get("efuse_faults") or [False] * 4
    warnings = []
    for idx in ((0, 1) if rail == 1 else (2, 3)):
        if idx < len(faults) and faults[idx]:
            warnings.append(f"eFuse{idx + 1} tripped - possible overcurrent. Check wiring.")
    if not st.get(f"vadj{rail}_pg", True):
        warnings.append(f"VADJ{rail} power-good signal not asserted - check load.")
    return warnings


def register(mcp) -> None:

    @mcp.tool()
    def target_power_up(
        supply_voltage: float = 5.0,
        rail:           int   = 1,
        settle_ms:      int   = 500,
        confirm:        bool  = False,
    ) -> dict:
        """
        Safely power up the target device on VADJ1 or VADJ2.

        Sequence:
        1. Disable both eFuses of the rail (clears any previous trip).
        2. Enable VADJ rail and set voltage.
        3. Wait settle_ms milliseconds for the rail to stabilise.
        4. Enable both eFuses of the rail.
        5. Read e-fuse trips and power-good back from the PCA9535.

        This avoids the capacitive-inrush eFuse trip that happens when the
        rail and eFuse are enabled simultaneously.

        Parameters:
        - supply_voltage: Target rail voltage in volts (3.0 - 15.0). Default 5.0.
        - rail: 1 = VADJ1 (IOs 1-6, EFUSE1+2), 2 = VADJ2 (IOs 7-12, EFUSE3+4). Default 1.
        - settle_ms: Milliseconds to wait between rail enable and eFuse enable.
                     Increase for targets with large input capacitance. Default 500.
        - confirm: Required for supply_voltage above 12 V.

        Returns: rail, voltage, efuses, success, warnings. success is False
        when an e-fuse tripped or power-good is missing.
        """
        efuses, vadj_ctrl, idac_ch = _rail_controls(rail)
        validate_vadj_voltage(supply_voltage, rail, confirm)
        bb = session.get_client()

        for ef in efuses:
            bb.power_set(ef, on=False)
        bb.power_set(vadj_ctrl, on=True)
        bb.idac_set_voltage(idac_ch, supply_voltage)
        time.sleep(settle_ms / 1000.0)
        for ef in efuses:
            bb.power_set(ef, on=True)

        time.sleep(0.1)
        warnings = _rail_warnings(bb, rail)
        return {
            "rail":     f"VADJ{rail}",
            "voltage":  supply_voltage,
            "efuses":   [ef.name for ef in efuses],
            "success":  not warnings,
            "warnings": warnings,
        }

    @mcp.tool()
    def enter_bootloader(
        boot_io:        int   = 1,
        tx_io:          int   = 2,
        rx_io:          int   = 3,
        baudrate:       int   = 115200,
        supply_voltage: float = 5.0,
        rail:           int   = 1,
        settle_ms:      int   = 500,
        confirm:        bool  = False,
    ) -> dict:
        """
        Enter the ESP32-C6 (or any ROM UART bootloader) download mode.

        Sequence:
        1. Assert BOOT low on boot_io (GPIO9 on ESP32-C6 = active-low entry).
        2. Full power cycle the target rail while BOOT is held low.
        3. Configure the UART bridge: tx_io → target RX, rx_io → target TX.

        The UART bridge is left enabled. esptool should be pointed at
        USB CDC #1 (/dev/cu.usbmodemXXXXX63) after this call.

        Default wiring (ESP32-C6 on BugBuster IO1-3):
        - boot_io=1  : IO1 → ESP32-C6 GPIO9 (BOOT, active-low)
        - tx_io=2    : IO2 → ESP32-C6 RX  (BugBuster drives)
        - rx_io=3    : IO3 ← ESP32-C6 TX  (BugBuster listens)

        Parameters:
        - boot_io:        BugBuster IO driving the target BOOT/GPIO0 pin. Default 2.
        - tx_io:          BugBuster IO connected to the target RX pin. Default 1.
        - rx_io:          BugBuster IO connected to the target TX pin. Default 3.
        - baudrate:       UART baud rate. Default 115200.
        - supply_voltage: Target supply voltage (V). Default 5.0.
        - rail:           VADJ rail (1 or 2). Default 1.
        - settle_ms:      Rail settle delay in ms. Default 500.
        - confirm:        Required for supply_voltage above 12 V.

        Returns: success, uart_bridge, boot_io, warnings.
        """
        from bugbuster.hal import DEFAULT_ROUTING

        efuses, vadj_ctrl, idac_ch = _rail_controls(rail)
        validate_vadj_voltage(supply_voltage, rail, confirm)
        bb  = session.get_client()
        hal = session.get_hal()

        # IO -> GPIO mapping (firmware UART_IO_GPIO_MAP)
        _ROUTING = {1: 4, 2: 2, 3: 1, 4: 7, 5: 6, 6: 5,
                    7: 8, 8: 9, 9: 10, 10: 11, 11: 12, 12: 13}
        tx_gpio = _ROUTING.get(tx_io)
        rx_gpio = _ROUTING.get(rx_io)
        if tx_gpio is None or rx_gpio is None:
            raise ValueError(f"Unsupported IO numbers: tx_io={tx_io}, rx_io={rx_io}")

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
        #     bus_planner_route_digital_input is called inside dio_configure and
        #     clobbers the MUX, so we re-assert the correct state immediately after.
        _mux_set_io(boot_io, drive=True)
        bb.dio_configure(boot_io, 2)        # mode 2 = OUTPUT (clobbers MUX)
        _mux_set_io(boot_io, drive=True)    # restore
        bb.dio_write(boot_io, False)        # drive low → ESP32 BOOT pin = 0

        # 2 — Configure UART bridge (also clobbers MUX via bus_planner).
        bb.set_uart_config(
            bridge_id=0, uart_num=1,
            tx_pin=tx_gpio, rx_pin=rx_gpio,
            baudrate=baudrate,
            data_bits=8, parity=0, stop_bits=0,
            enabled=True,
        )

        # 3 — Re-assert correct MUX state for all three IOs (firmware clobbered it).
        _mux_set_io(tx_io,   drive=True)   # UART TX out → target RX
        _mux_set_io(rx_io,   drive=False)  # UART RX in  ← target TX
        _mux_set_io(boot_io, drive=True)   # BOOT still held low

        # 4 — Full power cycle: turn everything off first, then bring up cleanly.
        #     This ensures the target resets even if it was already powered.
        for ef in efuses:
            bb.power_set(ef, on=False)
        bb.power_set(vadj_ctrl,  on=False)
        time.sleep(0.2)   # let target fully discharge

        bb.power_set(vadj_ctrl, on=True)
        bb.idac_set_voltage(idac_ch, supply_voltage)
        time.sleep(settle_ms / 1000.0)
        for ef in efuses:
            bb.power_set(ef, on=True)

        # 5 — Leave BOOT low; esptool will connect while it's held low.
        # Call release_bootloader() after flashing to reboot into the app.
        # (Do NOT release here.)

        time.sleep(0.1)
        warnings = _rail_warnings(bb, rail)

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

        Returns: success, boot_io, warnings.
        """
        from bugbuster.hal import PortMode

        efuses, _vadj, _ch = _rail_controls(rail)
        bb  = session.get_client()
        hal = session.get_hal()

        hal.configure(boot_io, PortMode.DIGITAL_OUT)
        hal.write_digital(boot_io, True)
        time.sleep(0.1)

        for ef in efuses:
            bb.power_set(ef, on=False)
        time.sleep(0.3)
        for ef in efuses:
            bb.power_set(ef, on=True)
        time.sleep(0.5)

        warnings = _rail_warnings(bb, rail)
        return {
            "success":  not warnings,
            "boot_io":  boot_io,
            "boot_pin": "HIGH",
            "note":     "Target power-cycled into normal boot mode.",
            "warnings": warnings,
        }
