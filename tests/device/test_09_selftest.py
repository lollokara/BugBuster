"""
test_09_selftest.py — Self-test, calibration, and diagnostics tests.

Covers: boot selftest status, supply measurement via U23 MUX, e-fuse
current monitoring, and auto-calibration.

Most tests use USB only since the detailed self-test response is only
available over the binary protocol (HTTP provides a simplified summary).
"""

import time

import pytest
from conftest import assert_no_faults

pytestmark = [pytest.mark.timeout(60)]  # includes pre/post device reset overhead


# ---------------------------------------------------------------------------
# Selftest status
# ---------------------------------------------------------------------------

def test_selftest_status(device):
    """
    selftest_status() returns a dict with 'boot' and 'cal' sub-dicts.
    boot: {ran, passed, vadj1_v, vadj2_v, vlogic_v}
    cal:  {status, channel, points, error_mv}
    """
    result = device.selftest_status()

    assert isinstance(result, dict), f"selftest_status() must return dict, got {type(result)}"
    assert "boot" in result or "cal" in result, (
        f"selftest_status() missing 'boot'/'cal' keys: {list(result.keys())}"
    )
    assert_no_faults(device)


def test_selftest_status_boot_fields(device):
    """
    The 'boot' sub-dict in selftest_status() must contain 'ran' flag.
    """
    result = device.selftest_status()

    boot = result.get("boot")
    if boot is None:
        pytest.skip("selftest_status() does not include 'boot' key on this firmware")

    assert isinstance(boot, dict), f"boot must be dict, got {type(boot)}"
    assert "ran" in boot, f"boot dict missing 'ran': {boot}"
    assert isinstance(boot["ran"], (bool, int)), "boot.ran must be bool/int"
    assert_no_faults(device)


def test_selftest_status_cal_fields(device):
    """
    The 'cal' sub-dict in selftest_status() must contain 'status' field.
    Cal status: 0=idle, 1=running, 2=success, 3=failed.
    """
    result = device.selftest_status()

    cal = result.get("cal")
    if cal is None:
        pytest.skip("selftest_status() does not include 'cal' key on this firmware")

    assert isinstance(cal, dict), f"cal must be dict, got {type(cal)}"
    assert "status" in cal, f"cal dict missing 'status': {cal}"
    assert cal["status"] in (0, 1, 2, 3), (
        f"cal.status must be 0-3, got {cal['status']}"
    )
    assert_no_faults(device)


# ---------------------------------------------------------------------------
# Supply measurement (U23 self-test MUX)
# ---------------------------------------------------------------------------

@pytest.mark.usb_only
def test_selftest_measure_supply_vadj1(usb_device):
    """
    selftest_measure_supply(0) measures VADJ1 via the U23 self-test MUX.
    Returns voltage in volts; should be > 0 if the supply is powered.
    Rail 0 = VADJ1, 1 = VADJ2, 2 = VLOGIC.
    """
    voltage = usb_device.selftest_measure_supply(0)

    assert isinstance(voltage, (int, float)), (
        f"selftest_measure_supply() must return float, got {type(voltage)}"
    )
    # -1.0 means unavailable; any other negative value is suspicious
    assert voltage >= -1.0, f"Unexpected negative voltage: {voltage}"
    assert_no_faults(usb_device)


@pytest.mark.usb_only
def test_selftest_measure_supply_all_rails(usb_device):
    """
    Measure all 3 supply rails (VADJ1=0, VADJ2=1, VLOGIC=2).
    Each should return a float (may be -1.0 if unavailable/not connected).
    """
    for rail in range(3):
        v = usb_device.selftest_measure_supply(rail)
        assert isinstance(v, (int, float)), (
            f"Rail {rail}: selftest_measure_supply() must return float, got {type(v)}"
        )
    assert_no_faults(usb_device)


# ---------------------------------------------------------------------------
# E-fuse current monitoring
# ---------------------------------------------------------------------------

@pytest.mark.usb_only
@pytest.mark.xfail(
    strict=False,
    reason=(
        "Wire-protocol drift: Python CmdId.SELFTEST_EFUSE_CURRENTS=0x07 "
        "collides with firmware BBP_CMD_SELFTEST_SUPPLY_VOLTAGES_CACHED=0x07 "
        "(see Firmware/ESP32/src/bbp/cmds/cmd_selftest.cpp). Firmware has "
        "no BBP efuse-currents handler; only the CLI 'efusei' command "
        "exposes per-channel IMON. Fix requires either reassigning the "
        "Python cmd-id, adding a firmware BBP handler, or deleting the "
        "Python wrapper. Tracked in PRD Lane G1."
    ),
)
def test_selftest_efuse_currents(usb_device):
    """
    selftest_efuse_currents() returns a dict with 'available' and 'currents' list.
    When available, currents[i] is the measured e-fuse output current in Amps.
    A value of -1.0 means the channel is unavailable.
    """
    result = usb_device.selftest_efuse_currents()

    assert isinstance(result, dict), (
        f"selftest_efuse_currents() must return dict, got {type(result)}"
    )
    assert "available" in result, "Result missing 'available' key"
    assert "currents" in result, "Result missing 'currents' key"

    currents = result["currents"]
    assert isinstance(currents, list), f"'currents' must be list, got {type(currents)}"
    assert len(currents) == 4, f"Expected 4 e-fuse channels, got {len(currents)}"

    for i, c in enumerate(currents):
        assert isinstance(c, (int, float)), f"currents[{i}] must be float, got {type(c)}"
    assert_no_faults(usb_device)


# ---------------------------------------------------------------------------
# Auto-calibration
# ---------------------------------------------------------------------------

@pytest.mark.usb_only
@pytest.mark.slow
@pytest.mark.destructive
def test_selftest_auto_calibrate(usb_device):
    """
    selftest_auto_calibrate(1) sweeps IDAC1 codes, measures VADJ1 voltages,
    builds a calibration curve, and saves to NVS.

    This takes several seconds and modifies calibration flash — destructive.
    Returns a dict with 'status', 'channel', 'points', 'error_mv'.
    """
    result = usb_device.selftest_auto_calibrate(1)

    assert isinstance(result, dict), (
        f"selftest_auto_calibrate() must return dict, got {type(result)}"
    )
    assert "status" in result, "Cal result missing 'status'"
    assert_no_faults(usb_device)


# ---------------------------------------------------------------------------
# Selftest status after calibration run
# ---------------------------------------------------------------------------

@pytest.mark.usb_only
def test_selftest_status_cal_idle_after_boot(usb_device):
    """
    cal.status should eventually settle to a terminal state — 0 (idle),
    2 (success), or 3 (failed). A transient status=1 (in-progress) is
    accepted up to ~5 s because a preceding @destructive test in this
    module may have started an auto-cal that is still finishing when this
    test runs.
    """
    # Auto-cal background worker can take ~40-50 s (see selftest.cpp ~40-50 pt
    # sweep + 10 s soak). A preceding @destructive test starts it async; this
    # test must wait long enough for the worker to finish its restore block.
    deadline = time.monotonic() + 70.0
    last_status = None
    while time.monotonic() < deadline:
        cal = usb_device.selftest_status().get("cal", {})
        if not cal:
            break  # no cal slot in payload → nothing to assert
        last_status = cal.get("status")
        if last_status in (0, 2, 3):
            break
        time.sleep(0.25)

    if last_status is not None:
        assert last_status in (0, 2, 3), (
            f"cal.status did not reach idle/success/failed within 5 s; "
            f"last observed status={last_status}"
        )
    assert_no_faults(usb_device)


# ---------------------------------------------------------------------------
# Internal supplies measurement
# ---------------------------------------------------------------------------

@pytest.mark.usb_only
@pytest.mark.timeout(30)
def test_internal_supplies(usb_device):
    """
    selftest_internal_supplies() measures internal AD74416H supply rails
    using diagnostic slots.  Returns a dict with supply voltages and
    die temperature.  Takes ~8 seconds.
    """
    result = usb_device.selftest_internal_supplies()

    assert isinstance(result, dict), (
        f"selftest_internal_supplies() must return dict, got {type(result)}"
    )
    # Should contain at least the valid flag and supply measurements
    assert "valid" in result, "Result missing 'valid' key"
    assert len(result) > 0, "Result dict should not be empty"

    # When valid, supply voltages should be present
    if result.get("valid"):
        assert "avdd_hi_v" in result, "Missing 'avdd_hi_v' when valid"
        assert "dvcc_v" in result, "Missing 'dvcc_v' when valid"
        assert "avcc_v" in result, "Missing 'avcc_v' when valid"
    assert_no_faults(usb_device)
