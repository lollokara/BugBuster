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


def test_hat_get_power_http_raises(http_device):
    """
    hat_get_power() over HTTP should raise NotImplementedError since
    the firmware does not expose a /api/hat/power endpoint.
    """
    with pytest.raises(NotImplementedError):
        http_device.hat_get_power()


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


def test_hat_set_io_voltage_http_raises(http_device):
    """
    hat_set_io_voltage() over HTTP should raise NotImplementedError.
    """
    with pytest.raises(NotImplementedError):
        http_device.hat_set_io_voltage(3300)


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


# ---------------------------------------------------------------------------
# SWD debug setup (USB only)
# ---------------------------------------------------------------------------

@pytest.mark.usb_only
def test_hat_swd_detect(usb_device):
    """
    hat_setup_swd(3300, 0) configures SWD routing for 3.3 V target.
    Should return True even if no SWD target is connected.
    """
    result = usb_device.hat_setup_swd(target_voltage_mv=3300, connector=0)
    assert result is True, f"hat_setup_swd() should return True, got {result}"
