"""
BugBuster MCP — Discovery and status tools.

Tools: device_status, device_info, check_faults, selftest
"""

from __future__ import annotations
import logging
from .. import session

log = logging.getLogger(__name__)


def register(mcp) -> None:

    @mcp.tool()
    def device_status() -> dict:
        """
        Return a full snapshot of the BugBuster device state.

        Includes: AD74416H channel states, die temperature, supply voltages,
        power-good signals, e-fuse status, HAT expansion board state, and
        active fault flags.

        Call this first to orient yourself before configuring any IOs.
        Returns a dict with keys: channels, die_temp_c, power, hat, transport.
        """
        bb = session.get_client()
        result = {}

        # Core device status
        try:
            result["device"] = bb.get_status()
        except Exception as e:
            result["device"] = {"error": str(e)}

        # Power/PCA9535 status
        try:
            result["power"] = bb.power_get_status()
        except Exception as e:
            result["power"] = {"error": str(e)}

        # HAT status (optional)
        try:
            result["hat"] = bb.hat_get_status()
        except Exception as e:
            result["hat"] = {"error": str(e)}

        result["transport"] = "usb" if session.is_usb() else "http"
        return result

    @mcp.tool()
    def device_info() -> dict:
        """
        Return BugBuster hardware identification and firmware version.

        Returns: silicon_id, silicon_rev, spi_ok, firmware_version (major.minor.patch).
        """
        bb = session.get_client()
        info = bb.get_device_info()
        fw   = bb.get_firmware_version()
        return {
            "spi_ok":           info.spi_ok,
            "silicon_rev":      info.silicon_rev,
            "silicon_id0":      info.silicon_id0,
            "silicon_id1":      info.silicon_id1,
            "firmware_version": f"{fw[0]}.{fw[1]}.{fw[2]}",
            "transport":        "usb" if session.is_usb() else "http",
        }

    @mcp.tool()
    def check_faults() -> dict:
        """
        Return all active hardware faults with human-readable descriptions.

        Checks: AD74416H channel alerts, e-fuse trip events, power-good
        failures, and the PCA9535 fault log.

        Returns: has_faults (bool), faults (list of strings), fault_log (list).
        """
        bb  = session.get_client()
        out = {"has_faults": False, "faults": [], "fault_log": []}

        # AD74416H fault/alert registers
        try:
            f = bb.get_faults()
            alert = f.get("alert_status", 0)
            supply_alert = f.get("supply_alert_status", 0)
            ch_faults = f.get("channel_alerts", [])
            if alert or supply_alert or any(ch_faults):
                out["has_faults"] = True
                if alert:
                    out["faults"].append(f"AD74416H global alert: 0x{alert:04X}")
                if supply_alert:
                    out["faults"].append(f"AD74416H supply alert: 0x{supply_alert:04X}")
                for i, ca in enumerate(ch_faults):
                    if ca:
                        out["faults"].append(f"Channel {i} alert: 0x{ca:04X}")
        except Exception as e:
            out["faults"].append(f"Could not read AD74416H faults: {e}")

        # PCA9535 e-fuse / power status
        try:
            ps = bb.power_get_status()
            efuse_faults = ps.get("efuse_faults", [])
            for i, tripped in enumerate(efuse_faults):
                if tripped:
                    out["has_faults"] = True
                    out["faults"].append(
                        f"E-fuse {i + 1} tripped (IO_Block {i + 1} overcurrent). "
                        f"Output disabled. Reduce load or check wiring."
                    )
            if not ps.get("vadj1_pg", True):
                out["has_faults"] = True
                out["faults"].append("VADJ1 power-good lost — supply 1 overloaded or shorted.")
            if not ps.get("vadj2_pg", True):
                out["has_faults"] = True
                out["faults"].append("VADJ2 power-good lost — supply 2 overloaded or shorted.")
        except Exception as e:
            out["faults"].append(f"Could not read power status: {e}")

        # PCA9535 fault event log
        try:
            out["fault_log"] = bb.power_get_fault_log()
        except Exception:
            out["fault_log"] = []

        if not out["has_faults"]:
            out["faults"].append("No active faults.")

        return out

    @mcp.tool()
    def selftest() -> dict:
        """
        Run the BugBuster built-in self-test suite.

        Checks: boot test status, internal supply voltages (±15 V, VADJ1,
        VADJ2, VLOGIC, 3.3 V), and e-fuse current measurements.

        Returns a dict with: boot_ok, supplies (voltages), efuse_currents,
        all_pass (bool), warnings (list).
        """
        bb  = session.get_client()
        out = {"all_pass": True, "warnings": []}

        # Boot test status
        try:
            st = bb.selftest_status()
            out["boot_test"] = st
            if not st.get("boot_ok", True):
                out["all_pass"] = False
                out["warnings"].append("Boot self-test failed.")
        except Exception as e:
            out["boot_test"] = {"error": str(e)}
            out["warnings"].append(f"Could not read boot test status: {e}")

        # Internal supply voltages
        try:
            supplies = bb.selftest_internal_supplies()
            out["supplies"] = supplies
            # Check for significant deviations
            nominal = {"3v3": 3.3, "vadj1": None, "vadj2": None, "vlogic": None}
            for k, nom in nominal.items():
                if nom and k in supplies:
                    v = supplies[k]
                    if abs(v - nom) / nom > 0.05:  # 5% tolerance
                        out["all_pass"] = False
                        out["warnings"].append(
                            f"Supply {k} reads {v:.3f} V (expected ~{nom:.1f} V)."
                        )
        except Exception as e:
            out["supplies"] = {"error": str(e)}
            out["warnings"].append(f"Could not measure supplies: {e}")

        # E-fuse current monitoring
        try:
            efuse = bb.selftest_efuse_currents()
            out["efuse_currents"] = efuse
        except Exception as e:
            out["efuse_currents"] = {"error": str(e)}

        if out["all_pass"]:
            out["summary"] = "All self-tests passed."
        else:
            out["summary"] = f"Self-test issues found: {'; '.join(out['warnings'])}"

        return out
