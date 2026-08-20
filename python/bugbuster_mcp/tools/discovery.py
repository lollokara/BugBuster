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
    def reset_link() -> dict:
        """
        Recover a wedged USB control link without restarting the MCP server.

        Symptom this fixes: every BBP command times out ("No response for
        cmd=0x.. within 5.0s") while the device is clearly alive - HTTP still
        answers and the DAQ data plane still streams. That happens when the
        transport's reader thread dies (a device re-enumeration, e.g. the P4
        resetting during an OTA, will do it). Writes keep succeeding because
        the port is still open, so nothing looks wrong until every command
        times out.

        Closes the serial port, rejoins the reader thread, reopens and
        re-runs the BBP handshake. Safe to call at any time; it does not touch
        device state, IO configuration or the DAQ HAT.

        Returns: method used, and link health before/after.
        """
        result = session.reconnect()
        result["message"] = (
            "Link healthy." if result.get("healthy_after")
            else "Link still unhealthy - check the cable and that no other "
                 "process (desktop app, pio device monitor) holds CDC0.")
        return result

    @mcp.tool()
    def link_status() -> dict:
        """
        Report the health of the host<->device control link.

        ``healthy`` is False when the port is open but the reader thread has
        died - the failure mode where writes succeed and every response times
        out. Use reset_link to recover.

        Returns: transport, port, healthy.
        """
        return {
            "transport": session.get_transport(),
            "port": session.get_port(),
            "healthy": session.link_healthy(),
        }

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
    def device_memory() -> dict:
        """
        Return live memory pressure for the ESP32-S3 mainboard.

        The S3 runs tight on INTERNAL SRAM. Use this before and after loading
        scripts, starting streams, or triggering an OTA to see whether the
        device has headroom left.

        Prefer this over device_status()'s free_heap: that figure sums internal
        and PSRAM, so on a PSRAM board it looks healthy while internal RAM --
        the pool that actually runs out -- is nearly exhausted.

        Returns internal/psram pools (free, min-ever, largest contiguous block,
        total, used %, fragmentation %), per-task stack headroom, a one-line
        summary, and a warnings list that is empty when the device is healthy.
        """
        bb = session.get_client()
        try:
            m = bb.get_memory_status()
        except Exception as e:
            return {"error": str(e),
                    "transport": "usb" if session.is_usb() else "http"}

        def pool(p) -> dict:
            return {
                "free_bytes": p.free_bytes,
                "min_ever_bytes": p.min_ever_bytes,
                "largest_block_bytes": p.largest_block_bytes,
                "total_bytes": p.total_bytes,
                "used_pct": round(p.used_pct, 1),
                "fragmentation_pct": round(p.fragmentation_pct, 1),
            }

        warnings = m.warnings()
        return {
            "internal": pool(m.internal),
            "psram": pool(m.psram) if m.has_psram else None,
            "tasks": [
                {
                    "name": t.name,
                    "declared_bytes": t.declared_bytes,
                    "free_bytes": t.free_bytes,
                    "peak_used_bytes": t.peak_used_bytes,
                    "used_pct": round(t.used_pct, 1),
                    "running": t.running,
                }
                for t in m.tasks
            ],
            "uptime_ms": m.uptime_ms,
            "summary": m.summary(),
            "warnings": warnings,
            "healthy": not warnings,
            "transport": "usb" if session.is_usb() else "http",
        }

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
            # Power-good is meaningless while a rail is disabled - it reads low
            # simply because the rail is off. Reporting that as "overloaded or
            # shorted" sends the user hunting a short that does not exist, which
            # is exactly the false alarm this tool exists to avoid.
            for idx, (en_key, pg_key) in enumerate(
                (("vadj1_en", "vadj1_pg"), ("vadj2_en", "vadj2_pg")), start=1
            ):
                if ps.get(en_key, False) and not ps.get(pg_key, True):
                    out["has_faults"] = True
                    out["faults"].append(
                        f"VADJ{idx} is enabled but power-good is lost - "
                        f"supply {idx} overloaded or shorted."
                    )
        except Exception as e:
            out["faults"].append(f"Could not read power status: {e}")

        # PCA9535 fault event log
        try:
            out["fault_log"] = bb.power_get_fault_log()
        except Exception as e:
            log.warning("Fault log fetch failed: %s", e)
            out["fault_log"] = []
            out["faults"].append(f"Could not fetch fault log (degraded state): {e}")

        if not out["has_faults"]:
            out["faults"].append("No active faults.")

        return out

    @mcp.tool()
    def selftest() -> dict:
        """
        Run the BugBuster built-in self-test suite.

        Checks: boot test status, internal supply voltages (±15 V, VADJ1,
        VADJ2, VLOGIC, 3.3 V), and cached supply rail voltages from the
        self-test worker.

        Returns a dict with: boot_ok, supplies (voltages),
        supply_voltages_cached, all_pass (bool), warnings (list).
        """
        bb  = session.get_client()
        from typing import Any
        out: dict[str, Any] = {"all_pass": True, "warnings": []}

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

        # Cached supply rail voltages from the self-test worker
        try:
            supplies_cached = bb.selftest_supplies_cached()
            out["supply_voltages_cached"] = supplies_cached
            out["efuse_currents"] = supplies_cached  # backward-compatible alias
        except Exception as e:
            out["supply_voltages_cached"] = {"error": str(e)}
            out["efuse_currents"] = {"error": str(e)}

        if out["all_pass"]:
            out["summary"] = "All self-tests passed."
        else:
            out["summary"] = f"Self-test issues found: {'; '.join(out['warnings'])}"

        return out

    @mcp.tool()
    def list_boards() -> list[str]:
        """
        List available board profiles in the bugbuster_mcp/board_profiles directory.
        Returns a list of board names (without .json extension).
        """
        import os
        profile_dir = os.path.join(os.path.dirname(os.path.dirname(__file__)), "board_profiles")
        if not os.path.exists(profile_dir):
            return []
        return [
            f[:-5] for f in os.listdir(profile_dir)
            if f.endswith(".json")
        ]

    @mcp.tool()
    def set_board(name: str) -> str:
        """
        Set the active board profile for the current session.

        This provides the AI with structured knowledge about the DUT (pin mapping,
        voltage domains, safety limits).  Always call this if you know what
        device is connected to BugBuster.

        Args:
            name: The name of the board profile (from list_boards).
        """
        import os
        profile_dir = os.path.join(os.path.dirname(os.path.dirname(__file__)), "board_profiles")
        profile_path = os.path.join(profile_dir, f"{name}.json")
        
        if not os.path.exists(profile_path):
            return f"Error: Board profile '{name}' not found in {profile_dir}."
            
        session.set_active_board(name)
        profile = session.get_active_board_profile()
        
        if profile:
            desc = profile.get("description", "No description")
            return f"Board profile set to '{name}' ({desc}).  Use bugbuster://board resource for details."
        else:
            return f"Error: Failed to load board profile '{name}'."

    @mcp.tool()
    def discover_devices(timeout_s: float = 2.0, usb: bool = True,
                         network: bool = True) -> dict:
        """
        Find BugBuster boards on USB and on the LAN.

        USB scan reads serial-port descriptors (Espressif VID 0x303A, mainboard
        PID 0x4002) and reports which port is CDC0 - the one that speaks BBP.
        The other CDC interface is the text console and will not answer.
        Network scan browses mDNS `_bugbuster._tcp`, which the firmware
        advertises once it joins WiFi.

        Use this when a connection fails, or to find the port to pass to the
        server's --port. The server auto-detects by default, so you normally
        do not need to.

        Parameters:
        - timeout_s: mDNS wait (1-5 s typical).
        - usb / network: enable each scan.

        Returns: usb_ports (device, vid_pid, interface, likely_bbp_port),
        active_transport/active_port, and network devices. mDNS needs the
        `zeroconf` extra (`pip install "bugbuster[network]"`).
        """
        out: dict = {}

        if usb:
            try:
                from bugbuster.discovery import list_usb_ports
                ports = list_usb_ports(all_ports=True)
                out["usb_ports"] = [
                    {
                        "device": p.device,
                        "vid_pid": f"{p.vid:04X}:{p.pid:04X}" if p.vid else None,
                        "description": p.description,
                        "interface": p.interface_index,
                        "is_bugbuster": p.is_bugbuster,
                    }
                    for p in ports
                ]
                bb_ports = [p for p in ports if p.is_bugbuster]
                out["likely_bbp_port"] = bb_ports[0].device if bb_ports else None
            except Exception as exc:
                out["usb_error"] = str(exc)
            out["active_transport"] = session.get_transport()
            out["active_port"] = session.get_port()

        if network:
            try:
                from bugbuster.discovery import discover_mdns
            except ImportError as e:
                out["network_error"] = str(e)
                out["hint"] = 'pip install "bugbuster[network]"'
                out["devices"] = []
                return out
            devs = discover_mdns(timeout=float(timeout_s))
            out["count"] = len(devs)
            out["devices"] = [
                {
                    "hostname": d.hostname,
                    "fqdn": d.fqdn,
                    "ip": d.ip,
                    "port": d.port,
                    "firmware": d.firmware,
                    "mac": d.mac,
                    "proto": d.proto,
                    "model": d.model,
                    "http_base": d.http_base,
                }
                for d in devs
            ]
        return out
