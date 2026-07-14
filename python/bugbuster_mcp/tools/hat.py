"""
BugBuster MCP — HAT v2 management tools.

Tools: hat_get_caps, hat_get_rail_status, hat_health_summary,
       hat_preflight_rail_operation, hat_set_rail_enable, hat_set_led_state,
       hat_la_set_route
"""

from __future__ import annotations
from .. import session
from ..safety import require_hat


HAT_RAIL_NAMES = {
    0: "3V3_ADJ",
    1: "VADJ3",
    2: "VADJ4",
}

HAT_RAIL_STATUS_NAMES = {
    0: "ok",
    1: "fault",
    2: "calibration_invalid",
    3: "busy",
}


def _rail_status_name(status: int) -> str:
    return HAT_RAIL_STATUS_NAMES.get(int(status), f"unknown_{int(status)}")


def _rail_list_by_id(rail_status: dict) -> dict[int, dict]:
    rails = rail_status.get("rails", []) if isinstance(rail_status, dict) else []
    out: dict[int, dict] = {}
    for rail in rails:
        try:
            rail_id = int(rail.get("rail_id", rail.get("railId")))
        except (TypeError, ValueError):
            continue
        out[rail_id] = rail
    return out


def build_hat_health_summary(
    status: dict | None = None,
    caps: dict | None = None,
    rail_status: dict | None = None,
    cal_status: dict | None = None,
    la_status: dict | None = None,
) -> dict:
    """Build a compact, transport-agnostic HAT health summary."""
    status = status or {}
    caps = caps or {}
    rail_status = rail_status or {}
    cal_status = cal_status or {}
    la_status = la_status or {}

    rails = []
    faulted_rails = []
    for rail_id, rail in sorted(_rail_list_by_id(rail_status).items()):
        raw_status = int(rail.get("status", 0) or 0)
        entry = {
            "rail_id": rail_id,
            "name": HAT_RAIL_NAMES.get(rail_id, f"rail_{rail_id}"),
            "enabled": bool(rail.get("enabled", False)),
            "voltage_mv": int(rail.get("voltage_mv", rail.get("voltageMv", 0)) or 0),
            "current_ma": int(rail.get("current_ma", rail.get("currentMa", 0)) or 0),
            "status": raw_status,
            "status_name": _rail_status_name(raw_status),
        }
        rails.append(entry)
        if raw_status != 0:
            faulted_rails.append(entry)

    cal_state = int(cal_status.get("state", 0) or 0) if isinstance(cal_status, dict) else 0
    cal_summary = {
        "state": cal_state,
        "running": cal_state == 1,
        "succeeded": cal_state == 2,
        "failed": cal_state == 3,
        "progress": int(cal_status.get("progress", 0) or 0) if isinstance(cal_status, dict) else 0,
        "rail_id": cal_status.get("rail_id", cal_status.get("railId")) if isinstance(cal_status, dict) else None,
        "persist_state": cal_status.get("persist_state", cal_status.get("persistState")) if isinstance(cal_status, dict) else None,
        "validation_flags": cal_status.get("validation_flags", cal_status.get("validationFlags")) if isinstance(cal_status, dict) else None,
    }

    present = bool(status.get("detected", status.get("present", False)))
    connected = bool(status.get("connected", False))
    degraded = bool(status.get("degraded", False)) or bool(faulted_rails) or cal_summary["failed"]

    return {
        "present": present,
        "connected": connected,
        "healthy": present and connected and not degraded,
        "degraded": degraded,
        "fw_version": status.get("fw_version") or (
            f"{status.get('fwMajor')}.{status.get('fwMinor')}"
            if status.get("fwMajor") is not None else None
        ),
        "capabilities": {
            "hw_revision": caps.get("hw_revision", caps.get("hwRevision")),
            "rail_count": caps.get("rail_count", caps.get("railCount")),
            "led_count": caps.get("led_count", caps.get("ledCount")),
            "shifted_io_count": caps.get("shifted_io_count", caps.get("shiftedIoCount")),
            "la_routes": caps.get("la_routes", caps.get("laRouteCount")),
        },
        "rails": rails,
        "faulted_rails": faulted_rails,
        "calibration": cal_summary,
        "logic_analyzer": {
            "state": la_status.get("state"),
            "state_name": la_status.get("state_name", la_status.get("stateName")),
            "route": status.get("la_route", status.get("laRoute")),
            "usb_mounted": la_status.get("usb_mounted", la_status.get("usbMounted")),
            "stop_reason": la_status.get("stop_reason", la_status.get("stopReason")),
        },
    }


def build_hat_rail_preflight(
    rail_id: int,
    *,
    target_voltage_mv: int | None = None,
    rail_status: dict | None = None,
    cal_status: dict | None = None,
    caps: dict | None = None,
) -> dict:
    """Return whether a HAT rail operation is safe to attempt."""
    reasons: list[str] = []
    warnings: list[str] = []

    if rail_id not in HAT_RAIL_NAMES:
        reasons.append("rail_id must be 0 (3V3_ADJ), 1 (VADJ3), or 2 (VADJ4)")

    if target_voltage_mv is not None:
        if rail_id == 0 and not 1700 <= int(target_voltage_mv) <= 5000:
            reasons.append("3V3_ADJ target must be between 1700 and 5000 mV")
        if rail_id in (1, 2) and not 1800 <= int(target_voltage_mv) <= 36000:
            reasons.append("VADJ3/VADJ4 target must be between 1800 and 36000 mV")

    if caps:
        rail_count = caps.get("rail_count", caps.get("railCount"))
        if rail_count is not None and rail_id >= int(rail_count):
            reasons.append(f"HAT reports only {rail_count} rail(s)")

    rails_by_id = _rail_list_by_id(rail_status or {})
    rail = rails_by_id.get(rail_id)
    if rail:
        raw_status = int(rail.get("status", 0) or 0)
        if raw_status != 0:
            reasons.append(f"{HAT_RAIL_NAMES.get(rail_id, rail_id)} status is {_rail_status_name(raw_status)}")

    cal_state = int((cal_status or {}).get("state", 0) or 0)
    cal_rail = (cal_status or {}).get("rail_id", (cal_status or {}).get("railId"))
    if cal_state == 1:
        if cal_rail is None or int(cal_rail) == rail_id:
            reasons.append("calibration is currently running on this rail")
        else:
            warnings.append(f"calibration is currently running on rail {cal_rail}")
    elif cal_state == 3:
        warnings.append("last calibration failed; verify rail calibration before powering a DUT")

    return {
        "ok": not reasons,
        "rail_id": rail_id,
        "rail_name": HAT_RAIL_NAMES.get(rail_id, f"rail_{rail_id}"),
        "target_voltage_mv": target_voltage_mv,
        "reasons": reasons,
        "warnings": warnings,
    }


def register(mcp) -> None:

    @mcp.tool()
    def hat_get_caps() -> dict:
        """
        Get HAT v2 hardware capabilities.

        Returns capability metadata including hardware revision, rail count,
        LED count, logical shifted IO count, and logic analyzer routes.

        Returns: hw_revision, flags, rail_count, led_count, shifted_io_count,
                 la_routes, fw_version.
        """
        bb = session.get_client()
        require_hat(bb)
        return bb.hat_get_caps()

    @mcp.tool()
    def hat_get_rail_status() -> dict:
        """
        Get status of HAT power rails.

        Returns status for 3V3_ADJ, VADJ3, and VADJ4 including enabled state,
        measured voltage, measured current, and fault status.

        Returns: count, rails (list of rail objects).
        """
        bb = session.get_client()
        require_hat(bb)
        return bb.hat_get_rail_status()

    @mcp.tool()
    def hat_health_summary() -> dict:
        """
        Return a compact HAT health summary for agents and automation.

        Combines HAT presence, capabilities, rail status, calibration state, and
        logic-analyzer status when available.
        """
        bb = session.get_client()
        require_hat(bb)
        status = bb.hat_get_status()
        caps = {}
        rails = {}
        cal = {}
        la = {}
        try:
            caps = bb.hat_get_caps()
        except Exception as e:
            caps = {"error": str(e)}
        try:
            rails = bb.hat_get_rail_status()
        except Exception as e:
            rails = {"error": str(e)}
        try:
            cal = bb.hat_calibrate_status()
        except Exception as e:
            cal = {"error": str(e)}
        try:
            la = bb.hat_la_get_status()
        except Exception as e:
            la = {"error": str(e)}
        return build_hat_health_summary(status, caps, rails, cal, la)

    @mcp.tool()
    def hat_preflight_rail_operation(
        rail_id: int,
        target_voltage_mv: int | None = None,
    ) -> dict:
        """
        Check whether a HAT rail operation is safe before mutating hardware.

        Parameters:
        - rail_id: Rail ID (0 = 3V3_ADJ, 1 = VADJ3, 2 = VADJ4).
        - target_voltage_mv: Optional requested voltage in millivolts.

        Returns: ok, rail name, blocking reasons, and non-blocking warnings.
        """
        bb = session.get_client()
        require_hat(bb)
        caps = {}
        rails = {}
        cal = {}
        try:
            caps = bb.hat_get_caps()
        except Exception:
            pass
        try:
            rails = bb.hat_get_rail_status()
        except Exception:
            pass
        try:
            cal = bb.hat_calibrate_status()
        except Exception:
            pass
        return build_hat_rail_preflight(
            rail_id,
            target_voltage_mv=target_voltage_mv,
            rail_status=rails,
            cal_status=cal,
            caps=caps,
        )

    @mcp.tool()
    def hat_set_rail_enable(
        rail_id: int,
        enable: bool,
    ) -> dict:
        """
        Enable or disable a HAT v2 power rail.

        Parameters:
        - rail_id: Rail ID (0 = 3V3_ADJ, 1 = VADJ3, 2 = VADJ4).
        - enable: True to enable, False to disable.

        Returns: refreshed status of all HAT rails.
        """
        if rail_id not in (0, 1, 2):
            raise ValueError(f"Invalid rail_id {rail_id}. Valid values: 0 (3V3_ADJ), 1 (VADJ3), 2 (VADJ4)")
        bb = session.get_client()
        require_hat(bb)
        preflight = build_hat_rail_preflight(
            rail_id,
            rail_status=bb.hat_get_rail_status(),
            cal_status=bb.hat_calibrate_status(),
            caps=bb.hat_get_caps(),
        )
        if not preflight["ok"]:
            raise RuntimeError("HAT rail preflight failed: " + "; ".join(preflight["reasons"]))
        return bb.hat_set_rail_enable(rail_id, enable)

    @mcp.tool()
    def hat_set_rail_voltage(
        rail_id: int,
        voltage_mv: int,
        confirm: bool = False,
    ) -> dict:
        """
        Set the voltage of a HAT v2 adjustable rail (VADJ3 or VADJ4).

        Automatically negotiates the appropriate USB-C PD profile *before*
        programming the DC-DC converter, ensuring the supply is always set
        first.

        VADJ3 and VADJ4 are LTM8083 buck-boost regulators fed from the
        USB-C PD bus.  They can step up (boost) or step down (buck) relative
        to the PD input voltage, and can produce up to ~30 V.  For targets
        above 20 V the manager uses the 20 V PD profile and lets the
        converter boost.  For lower targets it selects the smallest PD
        profile that keeps the converter in a comfortable buck-down region.

        Parameters:
        - rail_id: HAT rail ID. 1 = VADJ3, 2 = VADJ4.
        - voltage_mv: Target voltage in millivolts (e.g. 12000 for 12 V,
          27000 for 27 V).  Maximum is ~30000 mV; minimum ~1200 mV.
        - confirm: Must be True when voltage_mv > 15000 mV (15 V).

        Returns: pd_voltage_v, rail_id, voltage_mv, success, rail_status.
        """
        from bugbuster.pd_manager import ensure_pd_for_output, ConverterTopology

        if rail_id not in (1, 2):
            raise ValueError(
                f"Invalid rail_id {rail_id}. Valid values: 1 (VADJ3), 2 (VADJ4)."
            )
        if voltage_mv <= 0:
            raise ValueError(f"voltage_mv must be positive, got {voltage_mv}.")
        if voltage_mv > 15_000 and not confirm:
            raise ValueError(
                f"Requested {voltage_mv} mV ({voltage_mv / 1000:.2f} V) exceeds 15 V. "
                "Set confirm=True to acknowledge the high-voltage operation."
            )

        bb = session.get_client()
        require_hat(bb)

        # Preflight check
        preflight = build_hat_rail_preflight(
            rail_id,
            target_voltage_mv=voltage_mv,
            rail_status=bb.hat_get_rail_status(),
            cal_status=bb.hat_calibrate_status(),
            caps=bb.hat_get_caps(),
        )
        if not preflight["ok"]:
            raise RuntimeError(
                "HAT rail preflight failed: " + "; ".join(preflight["reasons"])
            )

        target_v = voltage_mv / 1000.0

        # Negotiate the minimum PD profile before touching the DCDC.
        # VADJ3/4 are buck-boost — headroom logic differs from pure-buck rails.
        pd_v = ensure_pd_for_output(bb, target_v=target_v, topology=ConverterTopology.BUCK_BOOST)

        # Program the rail voltage after the supply is ready.
        rail_status = bb.hat_set_rail_voltage(rail_id, voltage_mv)

        return {
            "success":    True,
            "rail_id":    rail_id,
            "voltage_mv": voltage_mv,
            "pd_voltage_v": pd_v,
            "rail_status": rail_status,
        }

    @mcp.tool()
    def hat_set_led_state(
        led_id: int,
        color_code: int,
    ) -> dict:
        """
        Set a HAT status LED color.

        Parameters:
        - led_id: Status LED index (1..8).
        - color_code: Color code mapping:
            0 — Off
            1 — Red
            2 — Green
            3 — Blue
            4 — Yellow
            5 — Cyan
            6 — Magenta
            7 — White
            Other — Orange alert

        Returns: success, message.
        """
        if led_id < 1 or led_id > 8:
            raise ValueError(f"Invalid led_id {led_id}. Must be between 1 and 8 inclusive.")
        bb = session.get_client()
        require_hat(bb)
        ok = bb.hat_set_led_state(led_id, color_code)
        return {
            "success": ok,
            "message": f"LED {led_id} set to color code {color_code}." if ok else f"Failed to set LED {led_id}."
        }

    @mcp.tool()
    def hat_la_set_route(
        route_id: int,
    ) -> dict:
        """
        Select Logic Analyzer capture route.

        Parameters:
        - route_id: Route ID:
            0 — Low-speed (GPIO2-5, 4 channels)
            1 — High-speed (Conn1, 3 channels)

        Returns: success, route_id, message.
        """
        if route_id not in (0, 1):
            raise ValueError(f"Invalid route_id {route_id}. Valid values: 0 (low-speed), 1 (high-speed)")
        bb = session.get_client()
        require_hat(bb)
        ok = bb.hat_la_set_route(route_id)
        return {
            "success": ok,
            "route_id": route_id,
            "message": f"LA route set to {'low-speed' if route_id == 0 else 'high-speed'}."
        }

    @mcp.tool()
    def hat_calibrate_start(
        rail_id: int,
    ) -> dict:
        """
        Start HAT auto-calibration on a specific rail.

        Parameters:
        - rail_id: Rail ID (1 = VADJ3, 2 = VADJ4).

        Returns: starting state.
        """
        if rail_id not in (1, 2):
            raise ValueError(f"Invalid rail_id {rail_id}. Valid values: 1 (VADJ3), 2 (VADJ4)")
        bb = session.get_client()
        require_hat(bb)
        status = bb.hat_calibrate_start(rail_id)
        return {"status": status}

    @mcp.tool()
    def hat_calibrate_status() -> dict:
        """
        Get the status of the current HAT calibration sweep.

        Returns: state, progress, rail_id, last_error.
        """
        bb = session.get_client()
        require_hat(bb)
        return bb.hat_calibrate_status()

    @mcp.tool()
    def hat_calibrate_import(
        rail_id: int,
        points: list[dict],
    ) -> dict:
        """
        Import calibration data (max 6 points) to the HAT.

        Parameters:
        - rail_id: Rail ID (0 = 3V3_ADJ, 1 = VADJ3, 2 = VADJ4).
        - points: List of calibration points, each point containing 'dac_code' (int) and 'measured_v' (float).
                  Example: [{"dac_code": -8, "measured_v": 3.4}, ...]

        Returns: success.
        """
        if rail_id not in (0, 1, 2):
            raise ValueError(f"Invalid rail_id {rail_id}. Valid values: 0 (3V3_ADJ), 1 (VADJ3), 2 (VADJ4)")
        bb = session.get_client()
        require_hat(bb)
        parsed_points = []
        for pt in points:
            if "dac_code" not in pt or "measured_v" not in pt:
                raise ValueError("Each point must contain 'dac_code' and 'measured_v'")
            parsed_points.append({
                "dac_code": int(pt["dac_code"]),
                "measured_v": float(pt["measured_v"])
            })
        ok = bb.hat_calibrate_import(rail_id, parsed_points)
        return {"success": ok}

    @mcp.tool()
    def hat_set_io_bank(
        dirs: int,
        ups: int,
        dns: int,
    ) -> dict:
        """
        Configure directions and pulls for the shifted IO bank.

        Parameters:
        - dirs: Direction mask (1 = output, 0 = input). Bit 0 corresponds to shifted IO 0 (GPIO10).
        - ups: Pull-up enable mask (1 = enabled).
        - dns: Pull-down enable mask (1 = enabled).

        Returns: success.
        """
        bb = session.get_client()
        require_hat(bb)
        ok = bb.hat_set_io_bank(dirs, ups, dns)
        return {"success": ok}

    @mcp.tool()
    def hat_set_level_shift(
        oe: bool,
        dir: bool,
    ) -> dict:
        """
        Override level shifter output enable (oe) and direction (dir) pins.

        Parameters:
        - oe: True to enable outputs, False to disable.
        - dir: True for output direction (RP2040 -> Host), False for input (Host -> RP2040).

        Returns: oe, dir.
        """
        bb = session.get_client()
        require_hat(bb)
        return bb.hat_set_level_shift(oe, dir)
