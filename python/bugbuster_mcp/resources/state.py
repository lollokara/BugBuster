"""
BugBuster MCP — Resource handlers for bugbuster:// URIs.

Resources provide read-only state queries for AI context.
"""

from __future__ import annotations
import json
from .. import session
from ..config import ANALOG_IOS, ALL_IOS, IO_TO_RAIL, IO_TO_IOBLOCK


def register(mcp) -> None:

    @mcp.resource("bugbuster://board")
    def board_resource() -> str:
        """Detailed structured knowledge about the DUT (pin mapping, safety, etc.)."""
        profile = session.get_active_board_profile()
        if profile is None:
            return json.dumps({
                "status": "No active board profile.",
                "action": "Use list_boards and set_board tools to load a profile."
            }, indent=2)
        return json.dumps(profile, indent=2)

    @mcp.resource("bugbuster://status")
    def status_resource() -> str:
        """Full device state snapshot including all channels, power, and faults."""
        bb  = session.get_client()
        out = {}
        try:
            out["device"]    = bb.get_status()
        except Exception as e:
            out["device"]    = {"error": str(e)}
        try:
            out["power"]     = bb.power_get_status()
        except Exception as e:
            out["power"]     = {"error": str(e)}
        try:
            out["hat"]       = bb.hat_get_status()
        except Exception as e:
            out["hat"]       = {"error": str(e)}
        out["transport"] = "usb" if session.is_usb() else "http"
        return json.dumps(out, indent=2)

    @mcp.resource("bugbuster://power")
    def power_resource() -> str:
        """All supply voltages, USB PD contract, and e-fuse protection status."""
        bb  = session.get_client()
        out = {}
        try:
            out["pca_status"] = bb.power_get_status()
        except Exception as e:
            out["pca_status"] = {"error": str(e)}
        try:
            out["idac"]       = bb.idac_get_status()
        except Exception as e:
            out["idac"]       = {"error": str(e)}
        try:
            out["usbpd"]      = bb.usbpd_get_status()
        except Exception:
            try:
                out["usbpd"]  = bb._http_get("/usbpd")
            except Exception as e:
                out["usbpd"]  = {"error": str(e)}
        return json.dumps(out, indent=2)

    @mcp.resource("bugbuster://faults")
    def faults_resource() -> str:
        """Active hardware faults with remediation hints."""
        bb  = session.get_client()
        from typing import Any
        out: dict[str, Any] = {"faults": [], "fault_log": []}
        try:
            f = bb.get_faults()
            out["ad74416h_faults"] = f
        except Exception as e:
            out["ad74416h_faults"] = {"error": str(e)}
        try:
            ps = bb.power_get_status()
            for i, tripped in enumerate(ps.get("efuse_faults", [])):
                if tripped:
                    out["faults"].append({
                        "type":     "efuse_trip",
                        "io_block": i + 1,
                        "message":  f"E-fuse {i + 1} tripped. Overcurrent on IO_Block {i + 1}.",
                        "action":   "Reduce load or check wiring. Re-enable with power_control.",
                    })
        except Exception as e:
            out["faults"].append({"error": str(e)})
        try:
            out["fault_log"] = bb.power_get_fault_log()
        except Exception:
            pass
        return json.dumps(out, indent=2)

    @mcp.resource("bugbuster://hat")
    def hat_resource() -> str:
        """HAT expansion board detection, pin configuration, and logic analyzer state."""
        bb = session.get_client()
        out = {}
        try:
            out["status"]    = bb.hat_get_status()
        except Exception as e:
            out["status"]    = {"error": str(e)}
        try:
            out["power"]     = bb.hat_get_power()
        except Exception as e:
            out["power"]     = {"error": str(e)}
        try:
            out["la_status"] = bb.hat_la_get_status()
        except Exception as e:
            out["la_status"] = {"error": str(e)}
        return json.dumps(out, indent=2)

    @mcp.resource("bugbuster://capabilities")
    def capabilities_resource() -> str:
        """Static device capabilities: available IOs, valid modes, voltage limits."""
        caps = {
            "io_count": 12,
            "analog_ios": sorted(ANALOG_IOS),
            "digital_ios": sorted(ALL_IOS - ANALOG_IOS),
            "io_to_rail": IO_TO_RAIL,
            "io_to_ioblock": IO_TO_IOBLOCK,
            "modes": {
                "analog_ios_only": [
                    "ANALOG_IN", "ANALOG_OUT", "CURRENT_IN", "CURRENT_OUT",
                    "RTD", "HART", "HAT",
                ],
                "all_ios": [
                    "DISABLED", "DIGITAL_IN", "DIGITAL_OUT",
                    "DIGITAL_IN_LOW", "DIGITAL_OUT_LOW",
                ],
            },
            "voltage_limits": {
                "vadj_min_v":          3.0,
                "vadj_max_v":          15.0,
                "vadj_confirm_above_v": 12.0,
                "dac_max_unipolar_v":  12.0,
                "dac_max_bipolar_v":   12.0,
                "vlogic_min_v":        1.8,
                "vlogic_max_v":        5.0,
                "current_max_safe_ma": 8.0,
                "current_max_full_ma": 25.0,
            },
            "waveform_gen": {
                "shapes":     ["sine", "square", "triangle", "sawtooth"],
                "freq_min_hz": 0.01,
                "freq_max_hz": 100.0,
                "channels":   [3, 6, 9, 12],
            },
            "logic_analyzer": {
                "max_channels":  4,
                "max_rate_hz":   10_000_000,
                "max_depth":     76_800,
                "requires_hat":  True,
            },
            "swd_debug": {
                "protocol":       "CMSIS-DAP",
                "requires_hat":   True,
                "target_voltage": "configurable 1200-5500 mV",
            },
            "transports": ["usb", "http"],
        }
        return json.dumps(caps, indent=2)
