"""
BugBuster MCP — HAT v2 management tools.

Tools: hat_get_caps, hat_get_rail_status, hat_set_rail_enable, hat_set_led_state, hat_la_set_route
"""

from __future__ import annotations
from .. import session
from ..safety import require_hat


def register(mcp) -> None:

    @mcp.tool()
    def hat_get_caps() -> dict:
        """
        Get HAT v2 hardware capabilities.

        Returns capability metadata including hardware revision, rail count,
        LED count, logical shifted IO count, and logic analyzer routes.

        Returns: hw_revision, flags, rail_count, led_count, shifted_io_count,
                 la_routes, fw_version, hvpak_present.
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
        return bb.hat_set_rail_enable(rail_id, enable)

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
