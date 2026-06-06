from bugbuster_mcp.tools.hat import build_hat_health_summary, build_hat_rail_preflight


def test_hat_health_summary_marks_faulted_rail_degraded():
    summary = build_hat_health_summary(
        status={"detected": True, "connected": True, "fw_version": "3.0", "la_route": 1},
        caps={"rail_count": 3, "led_count": 8, "la_routes": 2, "hvpak_present": False},
        rail_status={
            "rails": [
                {"rail_id": 0, "enabled": True, "voltage_mv": 3300, "current_ma": 10, "status": 0},
                {"rail_id": 1, "enabled": True, "voltage_mv": 5000, "current_ma": 0, "status": 2},
            ]
        },
        cal_status={"state": 0, "progress": 0},
        la_status={"state": 2, "state_name": "done", "usb_mounted": True},
    )

    assert summary["present"] is True
    assert summary["connected"] is True
    assert summary["healthy"] is False
    assert summary["degraded"] is True
    assert summary["faulted_rails"][0]["name"] == "VADJ3"
    assert summary["faulted_rails"][0]["status_name"] == "calibration_invalid"
    assert summary["logic_analyzer"]["route"] == 1


def test_hat_rail_preflight_blocks_bad_voltage_and_running_calibration():
    result = build_hat_rail_preflight(
        1,
        target_voltage_mv=50000,
        rail_status={"rails": [{"rail_id": 1, "status": 0}]},
        cal_status={"state": 1, "rail_id": 1},
        caps={"rail_count": 3},
    )

    assert result["ok"] is False
    assert "VADJ3/VADJ4 target must be between 1800 and 36000 mV" in result["reasons"]
    assert "calibration is currently running on this rail" in result["reasons"]


def test_hat_rail_preflight_warns_when_other_rail_calibrating():
    result = build_hat_rail_preflight(
        2,
        target_voltage_mv=12000,
        rail_status={"rails": [{"rail_id": 2, "status": 0}]},
        cal_status={"state": 1, "rail_id": 1},
        caps={"rail_count": 3},
    )

    assert result["ok"] is True
    assert result["warnings"] == ["calibration is currently running on rail 1"]
