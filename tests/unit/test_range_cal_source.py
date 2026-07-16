"""Regression checks for DAQ HAT autorange threshold calibration sequencing."""
from pathlib import Path

SRC = Path("Firmware/DAQ_HAT/ESP32P4/src/cal/range_cal.c").read_text()


def _body(name: str) -> str:
    markers = (f"static esp_err_t {name}", f"static void {name}")
    starts = [SRC.find(marker) for marker in markers]
    start = min(pos for pos in starts if pos >= 0)
    next_marker = SRC.find("\nstatic ", start + 1)
    if next_marker == -1:
        next_marker = len(SRC)
    return SRC[start:next_marker]


def test_range_cal_primes_supply_at_2v_before_prompting_for_resistor():
    task = _body("cal_task")
    assert "CAL_START_V" in SRC
    assert task.index("ramp_voltage(b, CAL_START_V)") < task.index("smu_enable(&b->smu, true)")
    assert task.index("smu_enable(&b->smu, true)") < task.index("c->phase = RANGE_CAL_PROMPT_A")


def test_range_cal_ignores_enable_time_inrush_trigger_before_ramping():
    for name in ("pass_a", "pass_b"):
        body = _body(name)
        assert "ignore_initial_asserted" in body
        assert body.index("ignore_initial_asserted") < body.index("ramp_step")
        assert "CAL_FF_STABLE_SAMPLES" in SRC


def test_range_cal_cleanup_leaves_known_safe_state_not_stale_range_current():
    assert "range_cal_restore_safe" in SRC
    restore = _body("range_cal_restore_safe")
    assert "smu_set_current_limit(&b->smu, SMU_CAL_RESTORE_CURRENT_A)" in restore
    assert "ramp_voltage(b, CAL_START_V)" in restore
    assert "range_manager_force(&b->range, RANGE_UNKNOWN)" in restore
