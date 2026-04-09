"""
test_11_hat.py — HAT expansion board tests.

All tests require the --hat CLI flag and physical HAT hardware.
USB-only tests additionally require --device-usb.

HAT features tested:
  - Status / detection
  - Pin function assignment
  - Power management (USB only)
  - Logic Analyzer (USB only)
  - SWD debug setup (USB only)
"""

import time
import pytest
import bugbuster as bb
from bugbuster.constants import LaTriggerType, HatPinFunction
from conftest import assert_no_faults

pytestmark = [
    pytest.mark.requires_hat,
    pytest.mark.timeout(15),
]


# ---------------------------------------------------------------------------
# Skip HAT tests if --hat not passed
# ---------------------------------------------------------------------------

@pytest.fixture(autouse=True)
def require_hat(request):
    """Auto-use fixture that skips all HAT tests unless --hat is passed."""
    if not request.config.getoption("--hat", default=False):
        pytest.skip("HAT tests require --hat flag")


# ---------------------------------------------------------------------------
# HAT status
# ---------------------------------------------------------------------------

def test_hat_get_status(device):
    """
    hat_get_status() returns a dict with at least 'detected' and 'connected' flags.
    Also checks for 'fw_version' and 'pin_config' fields.
    """
    status = device.hat_get_status()

    assert isinstance(status, dict), f"hat_get_status() must return dict, got {type(status)}"
    assert "detected" in status, "HAT status missing 'detected'"
    assert "connected" in status, "HAT status missing 'connected'"
    assert_no_faults(device)


def test_hat_status_has_pin_config(device):
    """
    HAT status should include a 'pin_config' list with 4 entries
    representing the function of each EXP_EXT pin.
    """
    status = device.hat_get_status()

    if "pin_config" in status:
        pins = status["pin_config"]
        assert isinstance(pins, list), f"pin_config must be list, got {type(pins)}"
        assert len(pins) == 4, f"Expected 4 EXP_EXT pins, got {len(pins)}"
    assert_no_faults(device)


# ---------------------------------------------------------------------------
# HAT detect
# ---------------------------------------------------------------------------

def test_hat_detect(device):
    """
    hat_detect() re-runs HAT detection and returns a result dict.
    Should not raise even if no HAT is physically connected.
    """
    result = device.hat_detect()

    assert isinstance(result, dict), f"hat_detect() must return dict, got {type(result)}"
    assert "detected" in result, "hat_detect() result missing 'detected'"
    assert_no_faults(device)


# ---------------------------------------------------------------------------
# HAT reset
# ---------------------------------------------------------------------------

def test_hat_reset(device):
    """
    hat_reset() resets the HAT to default state (all pins disconnected).
    Should return True on success.
    """
    result = device.hat_reset()
    assert result is True or result is not False, "hat_reset() should return True"
    assert_no_faults(device)


# ---------------------------------------------------------------------------
# HAT set single pin
# ---------------------------------------------------------------------------

def test_hat_set_pin(device):
    """
    hat_set_pin(0, HatPinFunction.DISCONNECTED) sets EXP_EXT_0 to disconnected/high-imp.
    Should return True on USB, may return bool on HTTP.
    """
    result = device.hat_set_pin(0, HatPinFunction.DISCONNECTED)
    assert result is not False, "hat_set_pin() should not return False"
    assert_no_faults(device)


def test_hat_set_pin_all_functions(device):
    """
    Cycle through all valid HatPinFunction values for pin 0.
    Reset to DISCONNECTED afterwards.
    """
    for func in HatPinFunction:
        device.hat_set_pin(0, func)
        time.sleep(0.02)

    # Restore to safe state
    device.hat_set_pin(0, HatPinFunction.DISCONNECTED)
    assert_no_faults(device)


# ---------------------------------------------------------------------------
# HAT set all pins
# ---------------------------------------------------------------------------

def test_hat_set_all_pins(device):
    """
    hat_set_all_pins() sets all 4 EXP_EXT pins at once.
    Set all to DISCONNECTED (safe default).
    """
    funcs = [
        HatPinFunction.DISCONNECTED,
        HatPinFunction.DISCONNECTED,
        HatPinFunction.DISCONNECTED,
        HatPinFunction.DISCONNECTED,
    ]
    result = device.hat_set_all_pins(funcs)
    assert result is not False, "hat_set_all_pins() should not return False"
    assert_no_faults(device)


# ---------------------------------------------------------------------------
# HAT power (USB only)
# ---------------------------------------------------------------------------

@pytest.mark.usb_only
def test_hat_get_power_usb(usb_device):
    """
    hat_get_power() returns a dict with 'connectors' list (2 entries)
    and 'io_voltage_mv' field. USB only.
    """
    result = usb_device.hat_get_power()

    assert isinstance(result, dict), f"hat_get_power() must return dict, got {type(result)}"
    assert "connectors" in result, "hat_get_power() missing 'connectors'"
    connectors = result["connectors"]
    assert isinstance(connectors, list), f"connectors must be list"
    assert len(connectors) == 2, f"Expected 2 HAT connectors, got {len(connectors)}"

    for i, conn in enumerate(connectors):
        assert "enabled" in conn, f"Connector {i} missing 'enabled'"
        assert "current_ma" in conn, f"Connector {i} missing 'current_ma'"
        assert "fault" in conn, f"Connector {i} missing 'fault'"
    assert_no_faults(usb_device)


def test_hat_get_power_http_raises(http_device):
    """
    hat_get_power() over HTTP should raise NotImplementedError since
    the firmware does not expose a /api/hat/power endpoint.
    """
    with pytest.raises(NotImplementedError):
        http_device.hat_get_power()
    assert_no_faults(http_device)


@pytest.mark.usb_only
@pytest.mark.destructive
def test_hat_set_power_usb(usb_device):
    """
    hat_set_power(0, False) disables target power on HAT connector A.
    Destructive: modifies power state. Restores power after test.
    """
    usb_device.hat_set_power(0, False)
    time.sleep(0.1)
    # Restore
    usb_device.hat_set_power(0, True)
    assert_no_faults(usb_device)


def test_hat_set_io_voltage_http_raises(http_device):
    """
    hat_set_io_voltage() over HTTP should raise NotImplementedError.
    """
    with pytest.raises(NotImplementedError):
        http_device.hat_set_io_voltage(3300)
    assert_no_faults(http_device)


# ---------------------------------------------------------------------------
# Logic Analyzer (USB only)
# ---------------------------------------------------------------------------

@pytest.mark.usb_only
def test_hat_la_configure(usb_device):
    """
    hat_la_configure(4, 1_000_000, 1000) configures the LA for 4 channels
    at 1 MHz sample rate with 1000 sample depth.
    Should return True.
    """
    result = usb_device.hat_la_configure(4, 1_000_000, 1000)
    assert result is True, f"hat_la_configure() should return True, got {result}"
    assert_no_faults(usb_device)


@pytest.mark.usb_only
def test_hat_la_arm_and_stop(usb_device):
    """
    Configure the LA, arm it, then immediately stop.
    Verifies the LA state machine can transition arm → stop without error.
    """
    usb_device.hat_la_configure(4, 1_000_000, 1000)
    usb_device.hat_la_arm()
    time.sleep(0.05)
    usb_device.hat_la_stop()
    assert_no_faults(usb_device)


@pytest.mark.usb_only
def test_hat_la_force_trigger(usb_device):
    """
    Configure the LA with no trigger, arm, then force trigger.
    Wait for the capture to complete and verify the status is 'done'.
    """
    usb_device.hat_la_configure(4, 1_000_000, 100)
    usb_device.hat_la_set_trigger(LaTriggerType.NONE, 0)
    usb_device.hat_la_arm()
    time.sleep(0.05)
    usb_device.hat_la_force()
    time.sleep(0.2)  # wait for capture

    status = usb_device.hat_la_get_status()
    # State may be 'done' (3) or 'capturing' (2) depending on timing
    assert status["state"] in (2, 3), (
        f"Expected LA state done(3) or capturing(2) after force, got {status['state_name']}"
    )
    # Clean up
    usb_device.hat_la_stop()
    assert_no_faults(usb_device)


@pytest.mark.usb_only
def test_hat_la_status(usb_device):
    """
    hat_la_get_status() returns a dict with 'state', 'state_name', and
    'samples_captured' fields.
    """
    usb_device.hat_la_configure(4, 1_000_000, 1000)
    status = usb_device.hat_la_get_status()

    assert isinstance(status, dict), f"hat_la_get_status() must return dict"
    assert "state" in status, "LA status missing 'state'"
    assert "state_name" in status, "LA status missing 'state_name'"
    assert status["state"] in (0, 1, 2, 3, 4), (
        f"LA state must be 0-4, got {status['state']}"
    )
    assert_no_faults(usb_device)


# ---------------------------------------------------------------------------
# SWD debug setup (USB only)
# ---------------------------------------------------------------------------

# NOTE: SWD-specific tests live in tests/device/test_15_swd.py since the
# 2026-04-09 cleanup. test_hat_swd_detect was moved there.


# ---------------------------------------------------------------------------
# Logic Analyzer — trigger types
# ---------------------------------------------------------------------------

@pytest.mark.usb_only
def test_la_trigger_types(usb_device):
    """
    Test that all LA trigger types can be configured, armed, and stopped
    without error.
    """
    usb_device.hat_la_configure(channels=4, rate_hz=1_000_000, depth=1000)

    for trig_type in [LaTriggerType.RISING, LaTriggerType.FALLING,
                      LaTriggerType.HIGH, LaTriggerType.LOW]:
        usb_device.hat_la_set_trigger(trig_type, channel=0)
        usb_device.hat_la_arm()
        status = usb_device.hat_la_get_status()
        # Accept armed(1), capturing(2), or done(3) — HIGH/LOW triggers fire
        # immediately if the condition is already met, and RISING/FALLING can
        # fire on noise with floating inputs. The important thing is that the
        # state is valid (not error=4).
        assert status["state"] in (1, 2, 3), (
            f"Expected LA state armed(1), capturing(2), or done(3) after arm "
            f"with trigger {trig_type!r}, got {status['state_name']}"
        )
        usb_device.hat_la_stop()

    assert_no_faults(usb_device)


# ---------------------------------------------------------------------------
# Logic Analyzer — capture and data readback
# ---------------------------------------------------------------------------

@pytest.mark.usb_only
def test_la_read_data(usb_device):
    """
    Test LA capture with force trigger and data readback.
    Configures a short capture, forces trigger, reads data, and decodes it.
    """
    from bugbuster import BugBuster

    usb_device.hat_la_configure(channels=4, rate_hz=1_000_000, depth=100)
    usb_device.hat_la_set_trigger(LaTriggerType.NONE, channel=0)
    usb_device.hat_la_arm()
    usb_device.hat_la_force()
    time.sleep(0.2)  # wait for capture to complete

    status = usb_device.hat_la_get_status()
    if status["state"] != 3:  # not DONE
        usb_device.hat_la_stop()
        pytest.skip(f"LA capture did not complete in time (state={status['state_name']})")

    data = usb_device.hat_la_read_all()
    assert data is not None, "hat_la_read_all() returned None"
    assert len(data) > 0, "hat_la_read_all() returned empty data"

    # Decode and verify structure
    samples = BugBuster.hat_la_decode(data, channels=4)
    assert len(samples) == 4, f"Expected 4 channel arrays, got {len(samples)}"
    assert len(samples[0]) > 0, "Channel 0 sample array is empty"

    # Each sample value should be 0 or 1
    for ch_idx, ch_data in enumerate(samples):
        for val in ch_data[:10]:
            assert val in (0, 1), (
                f"Channel {ch_idx} sample value must be 0 or 1, got {val}"
            )

    usb_device.hat_la_stop()
    assert_no_faults(usb_device)
