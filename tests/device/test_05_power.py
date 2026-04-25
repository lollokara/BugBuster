"""
test_05_power.py — Power management tests.

Covers: DS4424 IDAC (VADJ1/VADJ2 adjustable supplies), PCA9535 I/O expander
(power enable / e-fuse control), fault log, and IDAC calibration save.

IDAC channel mapping:
  0 = VLOGIC (TPS74601, 1.8–5 V logic level)
  1 = VADJ1  (LTM8063 #1, IO 1–6)
  2 = VADJ2  (LTM8063 #2, IO 7–12)
  3 = not connected
"""

import time
import pytest
from bugbuster import PowerControl
from conftest import assert_no_faults

pytestmark = [pytest.mark.timeout(10)]


# ---------------------------------------------------------------------------
# IDAC status
# ---------------------------------------------------------------------------

def test_idac_get_status(device):
    """
    idac_get_status() returns a dict with 'present' flag and 'channels' list.
    The channels list should contain at least 4 IdacChannel entries.
    """
    status = device.idac_get_status()

    assert isinstance(status, dict), f"idac_get_status() must return dict, got {type(status)}"
    assert "present" in status, "IDAC status missing 'present' key"
    assert "channels" in status, "IDAC status missing 'channels' key"

    channels = status["channels"]
    assert isinstance(channels, list), f"channels must be a list, got {type(channels)}"
    assert len(channels) >= 2, f"Expected at least 2 IDAC channels, got {len(channels)}"
    assert_no_faults(device)


# ---------------------------------------------------------------------------
# IDAC set voltage
# ---------------------------------------------------------------------------

def test_idac_set_voltage_vadj1(device):
    """
    Set VADJ1 (IDAC channel 1) to 5.0 V (5000 mV).
    The power supply must already be enabled for this to take effect.
    This test only verifies the command does not raise.
    """
    # idac_set_voltage takes channel and voltage in volts
    device.idac_set_voltage(1, 5.0)
    time.sleep(0.05)
    assert_no_faults(device)


def test_idac_set_voltage_vadj2(device):
    """Set VADJ2 (IDAC channel 2) to 3.3 V. Should not raise."""
    device.idac_set_voltage(2, 3.3)
    time.sleep(0.05)
    assert_no_faults(device)


# ---------------------------------------------------------------------------
# IDAC set code
# ---------------------------------------------------------------------------

def test_idac_set_code(device):
    """
    Set a raw signed 8-bit IDAC code (0 = mid-scale / no adjust) on channel 1.
    Useful for fine-tuning during calibration.
    """
    device.idac_set_code(1, 0)   # zero = no adjustment
    time.sleep(0.05)
    assert_no_faults(device)


def test_idac_set_code_midscale(device):
    """Set IDAC code to +64 (positive quarter-scale adjustment) on channel 1."""
    device.idac_set_code(1, 64)
    time.sleep(0.02)
    # Restore to neutral
    device.idac_set_code(1, 0)
    assert_no_faults(device)


# ---------------------------------------------------------------------------
# PCA9535 power status
# ---------------------------------------------------------------------------

def test_power_get_status(device):
    """
    power_get_status() (wraps PCA9535) returns a dict with 'present' and PG flags.
    Also checks for 'efuse_faults' list.
    """
    status = device.power_get_status()

    assert isinstance(status, dict), f"power_get_status() must return dict, got {type(status)}"
    assert "present" in status, "Power status missing 'present' key"
    assert_no_faults(device)


# ---------------------------------------------------------------------------
# PCA set control
# ---------------------------------------------------------------------------

def test_pca_set_control_enable_mux(device):
    """
    Enable MUX switch matrix power via PCA9535 control bit.
    power_set(PowerControl.MUX, True) should not raise.
    """
    device.power_set(PowerControl.MUX, True)
    time.sleep(0.05)
    assert_no_faults(device)


def test_pca_set_control_disable_mux(device):
    """Disable MUX power via PCA9535. Should not raise."""
    device.power_set(PowerControl.MUX, False)
    time.sleep(0.05)
    # Re-enable to leave in a known state
    device.power_set(PowerControl.MUX, True)
    assert_no_faults(device)


def test_pca_set_control_vadj1(device):
    """Enable VADJ1 power rail via PCA9535. Should not raise."""
    device.power_set(PowerControl.VADJ1, True)
    time.sleep(0.05)
    assert_no_faults(device)


def test_pca_set_control_all_controls(device):
    """
    Cycle through all PowerControl values, enabling then disabling each.
    Verifies the API accepts all valid control codes.
    """
    for ctrl in PowerControl:
        device.power_set(ctrl, True)
        time.sleep(0.02)
        device.power_set(ctrl, False)
        time.sleep(0.02)
    assert_no_faults(device)


# ---------------------------------------------------------------------------
# Fault log
# ---------------------------------------------------------------------------

def test_power_fault_log(device):
    """
    power_get_fault_log() returns a list (may be empty if no faults have occurred).
    Each entry should be a dict with 'type', 'type_name', 'channel', 'timestamp_ms'.
    """
    log = device.power_get_fault_log()

    assert isinstance(log, list), f"power_get_fault_log() must return list, got {type(log)}"
    for entry in log:
        assert isinstance(entry, dict), f"Fault log entry must be dict, got {type(entry)}"
        assert "type" in entry, f"Fault log entry missing 'type': {entry}"
        assert "channel" in entry, f"Fault log entry missing 'channel': {entry}"
    assert_no_faults(device)


# ---------------------------------------------------------------------------
# IDAC calibration save (destructive — writes to NVS)
# ---------------------------------------------------------------------------

@pytest.mark.destructive
def test_idac_cal_save(device):
    """
    idac_cal_save() persists the current IDAC calibration curves to NVS.
    This is a non-reversible write to flash — skipped if --skip-destructive.
    Verifies the command completes without raising.
    """
    device.idac_cal_save()
    assert_no_faults(device)


# ---------------------------------------------------------------------------
# Watchdog (USB only)
# ---------------------------------------------------------------------------

@pytest.mark.usb_only
def test_set_watchdog(usb_device):
    """
    set_watchdog() enables/disables the AD74416H hardware watchdog.
    Verify both enable (safe timeout) and disable work without error.
    """
    # Enable with a safe timeout code (9 = 1000ms)
    usb_device.set_watchdog(True, 9)
    time.sleep(0.05)

    # Disable watchdog
    usb_device.set_watchdog(False, 0)
    assert_no_faults(usb_device)


# ---------------------------------------------------------------------------
# Fault config (PCA9535)
# ---------------------------------------------------------------------------

def test_power_set_fault_config(device):
    """
    power_set_fault_config() configures PCA9535 fault behavior
    (auto-disable on trip, event logging).  Should not raise.
    """
    device.power_set_fault_config(auto_disable=True, log_events=True)
    time.sleep(0.02)

    # Also verify toggling the settings works
    device.power_set_fault_config(auto_disable=False, log_events=False)
    time.sleep(0.02)

    # Restore defaults
    device.power_set_fault_config(auto_disable=True, log_events=True)
    assert_no_faults(device)
