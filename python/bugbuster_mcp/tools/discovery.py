"""
BugBuster MCP — Discovery and status tools.

Tools: device_status, device_info, check_faults, selftest, crash_info, boot_report,
crash_dump_save, crash_clear
"""

from __future__ import annotations
import logging
from typing import Any

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
        out: dict[str, Any] = {"has_faults": False, "faults": [], "fault_log": []}

        # AD74416H fault/alert registers
        try:
            f = bb.get_faults()
            alert = f.get("alert_status", 0)
            supply_alert = f.get("supply_alert_status", 0)
            ch_faults = [(ch.get("id", i), ch.get("alert", 0))
                         for i, ch in enumerate(f.get("channels", []))]
            if alert or supply_alert or any(ca for _, ca in ch_faults):
                out["has_faults"] = True
                if alert:
                    out["faults"].append(f"AD74416H global alert: 0x{alert:04X}")
                if supply_alert:
                    out["faults"].append(f"AD74416H supply alert: 0x{supply_alert:04X}")
                for ch_id, ca in ch_faults:
                    if ca:
                        out["faults"].append(f"Channel {ch_id} alert: 0x{ca:04X}")
        except Exception as e:
            out["faults"].append(f"Could not read AD74416H faults: {e}")

        # PCA9535 e-fuse / power status
        ps: dict = {}
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
            # The firmware switches a tripped e-fuse off, which also clears its
            # FLT input, so efuse_faults above reads False again. The trip only
            # survives in the log: report a logged trip whose fuse is still off.
            enables = ps.get("efuse_enables", [])
            tripped = sorted({int(ev.get("channel", -1)) for ev in out["fault_log"]
                              if ev.get("type") == 0})
            for ch in tripped:
                if 0 <= ch < len(enables) and not enables[ch] and not (
                        ps.get("efuse_faults") or [False] * 4)[ch]:
                    out["has_faults"] = True
                    out["faults"].append(
                        f"E-fuse {ch + 1} tripped and was auto-disabled "
                        f"(IO_Block {ch + 1} overcurrent). Remove the overload, "
                        f"then re-enable it."
                    )
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

        Checks: boot test status (VADJ1, VADJ2, VLOGIC), AD74416H internal
        supplies (AVDD_HI, DVCC, AVCC, AVSS), and cached supply rail voltages
        from the self-test worker.

        Returns a dict with: boot_test (boot.ran / boot.passed), supplies,
        supply_voltages_cached, all_pass (bool), warnings (list).
        """
        bb  = session.get_client()
        out: dict[str, Any] = {"all_pass": True, "warnings": []}

        # Boot test status
        try:
            st = bb.selftest_status()
            out["boot_test"] = st
            boot = st.get("boot") or {}
            if not boot.get("ran", False):
                out["warnings"].append("Boot self-test has not run.")
            elif not boot.get("passed", False):
                out["all_pass"] = False
                out["warnings"].append("Boot self-test failed.")
        except Exception as e:
            out["boot_test"] = {"error": str(e)}
            out["warnings"].append(f"Could not read boot test status: {e}")

        # Internal supply voltages
        try:
            supplies = bb.selftest_internal_supplies()
            out["supplies"] = supplies
            # USB returns snake_case, HTTP /api/selftest/supplies camelCase.
            valid = supplies.get("valid", False)
            ok = supplies.get("supplies_ok", supplies.get("suppliesOk", False))
            if not valid:
                out["all_pass"] = False
                out["warnings"].append("Internal supply measurement is not valid.")
            elif not ok:
                out["all_pass"] = False
                out["warnings"].append(
                    "AD74416H internal supplies out of range: " + ", ".join(
                        f"{k}={supplies[k]:.2f}" for k in
                        ("avdd_hi_v", "dvcc_v", "avcc_v", "avss_v",
                         "avddHiV", "dvccV", "avccV", "avssV")
                        if isinstance(supplies.get(k), (int, float))
                    )
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
        # IO-23: a bare file stem only - no separators, drive or "..".
        if not name or os.path.basename(name) != name or "/" in name or "\\" in name \
                or os.path.isabs(name) or name.startswith("."):
            return f"Error: invalid board profile name '{name}'. Use a name from list_boards."
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
                devs = discover_mdns(timeout=float(timeout_s))
            except ImportError as e:
                out["network_error"] = str(e)
                out["hint"] = 'pip install "bugbuster[network]"'
                out["devices"] = []
                return out
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

    # ------------------------------------------------------------------
    # Crash dump and boot report (HTTP transport, admin token)
    # ------------------------------------------------------------------

    @mcp.tool()
    def crash_info() -> dict:
        """
        Why did the device last reset, and is there a coredump to analyse?

        Reads GET /api/system/crash. Returns the reset reason, boot count and
        consecutive-crash streak, and when a coredump is stored: task, PC,
        exception name (e.g. LoadProhibited), backtrace, registers, the size,
        whether the dump belongs to the firmware now running (elf_match), and
        ``pre`` - the heap/stack/boot-phase snapshot taken seconds before the
        crash. ``warnings`` lists what looks wrong; ``next_steps`` says how to
        decode it.

        HTTP transport only (USB sessions get an error naming the fix).
        """
        bb = session.get_client()
        try:
            c = bb.get_crash_info()
        except NotImplementedError as e:
            return {"error": str(e), "transport": "usb" if session.is_usb() else "http"}
        out: dict[str, Any] = {
            "summary": c.summary(),
            "ready": c.ready,
            "reset": {"reason": c.reset_reason, "abnormal": c.abnormal,
                      "boot": c.boot, "streak": c.streak, "bootloop": c.bootloop},
            "has_dump": c.has_dump,
            "warnings": c.warnings(),
        }
        if c.has_dump:
            out["dump"] = {
                "size_bytes": c.size, "panic": c.panic, "task": c.task,
                "pc": f"0x{c.pc:08x}" if c.pc is not None else None,
                "exccause": c.exccause, "exccause_name": c.exccause_name,
                "vaddr": f"0x{c.vaddr:08x}" if c.vaddr is not None else None,
                "backtrace": [f"0x{a:08x}" for a in c.backtrace],
                "backtrace_corrupt": c.bt_corrupt,
                "registers_a0_a15": [f"0x{r:08x}" for r in c.regs],
                "dump_elf_sha": c.dump_elf, "matches_running_firmware": c.elf_match,
            }
            out["next_steps"] = [
                "crash_dump_save() to download the ELF coredump",
                c.addr2line_command(), c.decode_command(),
                "crash_clear(confirm=True) once the dump is saved",
            ]
        if c.pre:
            p = c.pre
            out["pre_crash"] = {
                "boot": p.boot, "phase": p.phase, "uptime_ms": p.up_ms,
                "internal_free": p.int_free, "internal_min_ever": p.int_min,
                "internal_largest_block": p.int_largest,
                "psram_free": p.psram_free,
                "tightest_stack": {"task": p.min_stack_task, "free_bytes": p.min_stack_free},
                "wifi_sta": p.wifi_sta, "hat": p.hat, "usb": p.usb,
            }
        return out

    @mcp.tool()
    def boot_report(log_text: str = "") -> dict:
        """
        The diagnostics package the device logs ~30 s after every boot: system
        and firmware identity, heap/DMA/PSRAM, per-task stack headroom, network,
        HAT, script engine state and the crash summary.

        With no arguments it is built live from the device (HTTP transport).
        Pass ``log_text`` - raw lines exported from the log platform or a serial
        capture, containing ``BOOTRPT <boot> <section> <i>/<n> <json>`` - to
        decode past boots instead; every boot found is returned, oldest first,
        so a regression between boots is visible.
        """
        if log_text.strip():
            from bugbuster.crash import parse_bootrpt_lines
            reports = parse_bootrpt_lines(log_text)
            if not reports:
                return {"error": "no BOOTRPT lines found in log_text", "reports": []}
            return {"count": len(reports),
                    "reports": [dict(r.to_dict(), summary=r.summary()) for r in reports]}
        bb = session.get_client()
        try:
            r = bb.get_boot_report()
        except NotImplementedError as e:
            return {"error": str(e), "transport": "usb" if session.is_usb() else "http"}
        return dict(r.to_dict(), summary=r.summary())

    @mcp.tool()
    def crash_dump_save(directory: str = "") -> dict:
        """
        Download the stored ELF coredump to the host (~100-250 KB, 768 B per
        request, so allow a few seconds). The file is named
        ``bugbuster-coredump-boot<N>-<elfsha>.elf`` inside ``directory``
        (default: the system temp dir). Save it BEFORE crash_clear().

        Returns the path, size and a CRC-less sanity check (ELF magic). HTTP
        transport only.
        """
        import os
        import tempfile

        bb = session.get_client()
        try:
            c = bb.get_crash_info()
            if not c.has_dump:
                return {"error": "no valid coredump is stored on the device", "saved": False}
            data = bb.download_coredump()
        except NotImplementedError as e:
            return {"error": str(e), "transport": "usb" if session.is_usb() else "http"}
        except FileNotFoundError as e:
            return {"error": str(e), "saved": False}
        target = directory or tempfile.gettempdir()
        os.makedirs(target, exist_ok=True)
        sha = "".join(ch for ch in c.dump_elf[:9] if ch.isalnum()) or "unknown"
        path = os.path.join(target, f"bugbuster-coredump-boot{c.boot}-{sha}.elf")
        with open(path, "wb") as fh:
            fh.write(data)
        return {"saved": True, "path": path, "size_bytes": len(data),
                "elf_magic_ok": data[:4] == b"\x7fELF",
                "matches_running_firmware": c.elf_match,
                "decode": c.decode_command(path)}

    @mcp.tool()
    def crash_clear(confirm: bool = False) -> dict:
        """
        Erase the coredump stored in flash (and the pre-crash snapshot with it).
        Irreversible: call crash_dump_save() first. A new crash overwrites the
        dump anyway; clearing just stops the old one being reported every boot.

        Requires confirm=True. HTTP transport, admin token.
        """
        from ..safety import require_confirm
        require_confirm(confirm, "crash_clear", "permanently erases the stored coredump")
        bb = session.get_client()
        try:
            had = bb.clear_coredump()
        except NotImplementedError as e:
            return {"error": str(e), "transport": "usb" if session.is_usb() else "http"}
        return {"cleared": True, "had_dump": had}
