"""
test_01_core.py — Core device functionality tests.

Covers: ping, firmware version, device info, status, reset, faults.
Tests run over both USB and HTTP transports (parametrized via the 'device' fixture).
"""

import time
import pytest
from bugbuster import ChannelFunction
from conftest import assert_no_faults

pytestmark = [pytest.mark.timeout(10)]


# ---------------------------------------------------------------------------
# Ping
# ---------------------------------------------------------------------------

def test_ping(usb_device):
    """
    Ping the device and verify it responds within 500 ms.
    Also verifies the echoed token matches and uptime is non-negative.
    USB only — HTTP transport has no ping endpoint.
    """
    start = time.monotonic()
    result = usb_device.ping()
    elapsed_ms = (time.monotonic() - start) * 1000.0

    assert elapsed_ms < 500.0, f"Ping latency {elapsed_ms:.1f} ms exceeds 500 ms limit"
    assert result.token == 0xDEADBEEF, f"Token mismatch: {result.token:#010x}"
    assert result.uptime_ms >= 0, f"uptime_ms must be non-negative, got {result.uptime_ms}"
    assert_no_faults(usb_device)


# ---------------------------------------------------------------------------
# Firmware version
# ---------------------------------------------------------------------------

def test_firmware_version(device):
    """
    Retrieve the firmware version and assert all three components are non-negative integers.
    Works over both USB and HTTP.
    """
    major, minor, patch = device.get_firmware_version()

    assert isinstance(major, int), f"fw_major must be int, got {type(major)}"
    assert isinstance(minor, int), f"fw_minor must be int, got {type(minor)}"
    assert isinstance(patch, int), f"fw_patch must be int, got {type(patch)}"
    assert major >= 0, f"fw_major must be >= 0, got {major}"
    assert minor >= 0, f"fw_minor must be >= 0, got {minor}"
    assert patch >= 0, f"fw_patch must be >= 0, got {patch}"
    assert_no_faults(device)


# ---------------------------------------------------------------------------
# Device info
# ---------------------------------------------------------------------------

def test_device_info(device):
    """
    Read silicon identification and assert expected fields are present.
    DeviceInfo has: spi_ok, silicon_rev, silicon_id0, silicon_id1.
    """
    info = device.get_device_info()

    # DeviceInfo is a namedtuple with spi_ok, silicon_rev, silicon_id0, silicon_id1
    assert hasattr(info, "silicon_id0"), "DeviceInfo missing silicon_id0"
    assert hasattr(info, "silicon_id1"), "DeviceInfo missing silicon_id1"
    assert hasattr(info, "silicon_rev"), "DeviceInfo missing silicon_rev (revision)"
    assert isinstance(info.silicon_id0, int), f"silicon_id0 must be int, got {type(info.silicon_id0)}"
    assert isinstance(info.silicon_id1, int), f"silicon_id1 must be int, got {type(info.silicon_id1)}"
    assert info.silicon_rev >= 0, f"silicon_rev must be >= 0, got {info.silicon_rev}"
    assert_no_faults(device)


# ---------------------------------------------------------------------------
# Device status
# ---------------------------------------------------------------------------

def test_get_status(device):
    """
    Call get_status() and verify it returns a dict with a 'channels' list of 4 items.
    Each channel entry should have an 'id' field.
    """
    status = device.get_status()

    assert isinstance(status, dict), f"get_status() must return dict, got {type(status)}"
    assert "channels" in status, "Status dict missing 'channels' key"
    channels = status["channels"]
    assert isinstance(channels, list), f"channels must be a list, got {type(channels)}"
    assert len(channels) == 4, f"Expected 4 channels, got {len(channels)}"

    for i, ch in enumerate(channels):
        assert isinstance(ch, dict), f"channels[{i}] must be a dict"
        assert "id" in ch or "function" in ch, (
            f"channels[{i}] missing id/function key: {ch.keys()}"
        )
    assert_no_faults(device)


# ---------------------------------------------------------------------------
# Reset clears state
# ---------------------------------------------------------------------------

@pytest.mark.destructive
def test_reset_clears_state(device):
    """
    Set channel 0 to VOUT, then call reset(), and verify the device
    returns to a known safe state (channels present in status with function = 0 = HIGH_IMP).
    """
    # Put ch0 into VOUT mode
    device.set_channel_function(0, ChannelFunction.VOUT)
    time.sleep(0.05)

    # Reset should put all channels back to HIGH_IMP
    device.reset()
    time.sleep(0.1)

    status = device.get_status()
    channels = status["channels"]
    assert len(channels) == 4, f"Expected 4 channels after reset, got {len(channels)}"

    ch0 = channels[0]
    assert ch0.get("function", -1) == int(ChannelFunction.HIGH_IMP), (
        f"Channel 0 function should be HIGH_IMP (0) after reset, got {ch0.get('function')}"
    )
    assert_no_faults(device)


# ---------------------------------------------------------------------------
# Get faults
# ---------------------------------------------------------------------------

def test_get_faults_returns_dict(device):
    """
    Call get_faults() and verify it returns a dict without crashing.
    The dict should contain at least 'alert_status' and 'channels'.
    """
    faults = device.get_faults()

    assert isinstance(faults, dict), f"get_faults() must return dict, got {type(faults)}"
    assert "alert_status" in faults, "Faults dict missing 'alert_status'"
    assert "channels" in faults, "Faults dict missing 'channels'"


# ---------------------------------------------------------------------------
# Check faults
# ---------------------------------------------------------------------------

def test_check_faults_returns_list(device):
    """
    Call check_faults() and verify it returns a list (may be empty).
    Each element should be a dict with expected keys.
    """
    faults = device.check_faults()

    assert isinstance(faults, list), f"check_faults() must return list, got {type(faults)}"
    for f in faults:
        assert isinstance(f, dict), f"Each fault must be a dict, got {type(f)}"
        assert "source" in f, f"Fault dict missing 'source': {f}"
        assert "description" in f, f"Fault dict missing 'description': {f}"


# ---------------------------------------------------------------------------
# Firmware version consistency
# ---------------------------------------------------------------------------

def test_firmware_version_consistent(device):
    """
    Call get_firmware_version() twice and verify the result is identical.
    Ensures the firmware version is stable across repeated queries.
    """
    v1 = device.get_firmware_version()
    v2 = device.get_firmware_version()
    assert v1 == v2, f"Firmware version changed between calls: {v1} vs {v2}"
    assert_no_faults(device)


# ---------------------------------------------------------------------------
# Status has temperature
# ---------------------------------------------------------------------------

def test_status_has_die_temperature(device):
    """
    Verify get_status() includes a die_temp_c field with a plausible value.
    The AD74416H internal sensor reports -40 to +125 °C nominally.
    """
    status = device.get_status()

    assert "die_temp_c" in status, "Status missing 'die_temp_c'"
    temp = status["die_temp_c"]
    assert isinstance(temp, (int, float)), f"die_temp_c must be numeric, got {type(temp)}"
    assert -50.0 <= temp <= 150.0, (
        f"die_temp_c={temp:.1f} °C is outside plausible range [-50, 150]"
    )
    assert_no_faults(device)
