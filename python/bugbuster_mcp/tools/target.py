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


def _result_warnings(res: dict, rail: int) -> list[str]:
    """E-fuse trips, missing power-good and clamping from a rail_power_up() result."""
    first = 1 if rail == 1 else 3
    warnings = []
    for i, tripped in enumerate(res.get("efuse_faults") or []):
        if tripped:
            warnings.append(f"eFuse{first + i} tripped - possible overcurrent. Check wiring.")
    if not res.get("pg", True):
        warnings.append(f"VADJ{rail} power-good signal not asserted - check load.")
    if res.get("clamped"):
        warnings.append(f"VADJ{rail} clamped to {res['applied_v']:.2f} V by the DAC limits.")
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
        efuses, _vadj_ctrl, _idac_ch = _rail_controls(rail)
        validate_vadj_voltage(supply_voltage, rail, confirm)
        bb = session.get_client()

        # One firmware-sequenced frame (BBP RAIL_POWER_UP / HTTP rail_up): the
        # e-fuse/VADJ ordering, settle and blackout no longer depend on the host.
        res = bb.rail_power_up(rail, supply_voltage, settle_ms, confirm=confirm)
        warnings = _result_warnings(res, rail)
        return {
            "rail":      f"VADJ{rail}",
            "voltage":   supply_voltage,
            "applied_v": res["applied_v"],
            "efuses":    [ef.name for ef in efuses],
            "success":   not warnings,
            "warnings":  warnings,
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
        validate_vadj_voltage(supply_voltage, rail, confirm)
        _rail_controls(rail)
        bb  = session.get_client()
        hal = session.get_hal()

        # IO -> GPIO mapping (firmware UART_IO_GPIO_MAP)
        _ROUTING = {1: 4, 2: 2, 3: 1, 4: 7, 5: 6, 6: 5,
                    7: 8, 8: 9, 9: 10, 10: 11, 11: 12, 12: 13}
        tx_gpio = _ROUTING.get(tx_io)
        rx_gpio = _ROUTING.get(rx_io)
        if tx_gpio is None or rx_gpio is None:
            raise ValueError(f"Unsupported IO numbers: tx_io={tx_io}, rx_io={rx_io}")

        # 1 - Assert BOOT low BEFORE the target is power-cycled. dio_configure
        #     and the UART bridge route their IOs in firmware and MERGE into the
        #     live MUX state (IO-1), so the earlier host-side MUX re-assertion
        #     workaround is gone.
        bb.dio_configure(boot_io, 2)        # mode 2 = OUTPUT
        bb.dio_write(boot_io, False)        # drive low -> ESP32 BOOT pin = 0

        # 2 - UART bridge: tx_io -> target RX, rx_io <- target TX.
        bb.set_uart_config(
            bridge_id=0, uart_num=1,
            tx_pin=tx_gpio, rx_pin=rx_gpio,
            baudrate=baudrate,
            data_bits=8, parity=0, stop_bits=0,
            enabled=True,
        )
        if isinstance(getattr(hal, "_mux_state", None), list):
            hal._mux_state[:] = bb.mux_get()   # keep the HAL shadow in sync

        # 3 - Full power cycle in ONE firmware-sequenced frame so the target
        #     resets with BOOT held low (e-fuses off, VADJ off + discharge, set V,
        #     VADJ on, settle, e-fuses armed, PG/fault read).
        res = bb.rail_power_up(rail, supply_voltage, settle_ms,
                               confirm=confirm, power_cycle=True)

        # 4 - Leave BOOT low; esptool connects while it is held low.
        # Call release_bootloader() after flashing to reboot into the app.
        warnings = _result_warnings(res, rail)

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

        _rail_controls(rail)
        bb  = session.get_client()
        hal = session.get_hal()

        hal.configure(boot_io, PortMode.DIGITAL_OUT)
        hal.write_digital(boot_io, True)
        time.sleep(0.1)

        # Cycle the rail's e-fuses at the rail's CURRENT setpoint in one
        # firmware-sequenced frame (VADJ stays on; 300 ms off-time as before).
        cur_v = bb.idac_get_status()["channels"][rail].target_v
        res = bb.rail_power_up(rail, cur_v, 300, confirm=cur_v > 12.0)

        warnings = _result_warnings(res, rail)
        return {
            "success":  not warnings,
            "boot_io":  boot_io,
            "boot_pin": "HIGH",
            "note":     "Target power-cycled into normal boot mode.",
            "warnings": warnings,
        }
