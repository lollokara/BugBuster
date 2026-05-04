"""
BugBuster MCP — Power management tools.

Tools: usb_pd_status, usb_pd_select, power_control, wifi_status
"""

from __future__ import annotations
from .. import session
from ..safety import check_faults_post
from ..config import USBPD_ALLOWED_VOLTAGES


_USBPD_VOLTAGE_MAP = {5: 1, 9: 2, 12: 3, 15: 4, 18: 5, 20: 6}

_POWER_CONTROL_MAP = {
    "vadj1":   0,
    "vadj2":   1,
    "v15a":    2,
    "15v":     2,
    "mux":     3,
    "usb_hub": 4,
    "efuse1":  5,
    "efuse2":  6,
    "efuse3":  7,
    "efuse4":  8,
}


def register(mcp) -> None:

    @mcp.tool()
    def usb_pd_status() -> dict:
        """
        Return the USB Power Delivery contract status.

        Shows the current negotiated voltage and current from the USB-C PD
        source, and lists all available PDOs (Power Delivery Objects) that
        can be requested.

        Returns: present, negotiated_voltage_v, negotiated_current_a,
                 available_pdos, connected.
        """
        bb = session.get_client()
        return bb.usbpd_get_status()

    @mcp.tool()
    def usb_pd_select(voltage_v: int) -> dict:
        """
        Request a specific voltage from the USB-C Power Delivery source.

        Negotiates a new PD contract with the upstream USB-C charger.
        The charger must support the requested voltage.

        Parameters:
        - voltage_v: Requested voltage in volts. Must be one of: 5, 9, 12, 15, 18, 20.

        Returns: success, voltage_v, message.
        """
        if voltage_v not in USBPD_ALLOWED_VOLTAGES:
            raise ValueError(
                f"Voltage {voltage_v} V is not a standard USB PD voltage. "
                f"Valid values: {sorted(USBPD_ALLOWED_VOLTAGES)}"
            )
        bb = session.get_client()
        bb.usbpd_select_voltage(voltage_v)
        return {
            "success":   True,
            "voltage_v": voltage_v,
            "message":   f"Requested {voltage_v} V from USB-C PD source. "
                         f"Wait ~100ms for renegotiation to complete.",
        }

    @mcp.tool()
    def power_control(
        control: str,
        enable:  bool,
    ) -> dict:
        """
        Enable or disable a BugBuster power rail or protection device.

        Use this to manually control power supplies and e-fuses. In normal
        use, the HAL handles power sequencing automatically via configure_io
        and set_supply_voltage. Use this tool for manual control or recovery.

        Parameters:
        - control: Power control name. One of:
            "vadj1"   — LTM8063 supply 1 (powers IOs 1-6, 3-15 V)
            "vadj2"   — LTM8063 supply 2 (powers IOs 7-12, 3-15 V)
            "v15a"    — ±15 V analog supply (required for AD74416H analog modes)
            "mux"     — MUX switch matrix power
            "usb_hub" — Downstream USB hub power
            "efuse1"  — E-fuse protection for IO_Block 1 (IOs 1-3)
            "efuse2"  — E-fuse protection for IO_Block 2 (IOs 4-6)
            "efuse3"  — E-fuse protection for IO_Block 3 (IOs 7-9)
            "efuse4"  — E-fuse protection for IO_Block 4 (IOs 10-12)
        - enable: True to enable, False to disable.

        Returns: control, enable, success.
        """
        key = control.lower().strip()
        if key not in _POWER_CONTROL_MAP:
            raise ValueError(
                f"Unknown power control {control!r}. "
                f"Valid controls: {', '.join(sorted(_POWER_CONTROL_MAP))}"
            )
        bb = session.get_client()
        from bugbuster.constants import PowerControl
        ctrl = PowerControl(_POWER_CONTROL_MAP[key])
        bb.power_set(ctrl, enable)
        warnings = check_faults_post(bb)
        res = {
            "control": key,
            "enable":  enable,
            "success": True,
        }
        if warnings:
            res["warnings"] = warnings
        return res

    @mcp.tool()
    def wifi_status() -> dict:
        """
        Return BugBuster WiFi connection status.

        Shows the current WiFi mode (AP / STA), SSID, IP address, signal
        strength, and whether a WiFi connection is active.

        Returns: mode, ssid, ip, rssi_dbm, connected.
        """
        bb = session.get_client()
        return bb.wifi_get_status()

    @mcp.tool()
    def wifi_set_ap_password(password: str) -> dict:
        """
        Set the BugBuster SoftAP password.

        Persists the new password to NVS and applies it live — no reboot
        required. Current AP clients will be disconnected immediately when
        the password changes.

        Parameters:
        - password: New WPA2-PSK password. Must be 8–63 characters.

        Returns: success, message.
        """
        if len(password) < 8 or len(password) > 63:
            raise ValueError(
                f"AP password must be 8-63 characters (WPA2 requirement), got {len(password)}"
            )
        bb = session.get_client()
        ok = bb.wifi_set_ap_password(password)
        return {
            "success": ok,
            "message": (
                "AP password updated and applied live. Reconnect using the new password."
                if ok else
                "Failed to set AP password. Check firmware logs."
            ),
        }
