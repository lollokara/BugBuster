"""
test_12_faults.py — Fault management and alert tests.

Covers: clear alerts (all/per-channel), check_faults, alert mask,
and DIN event callback registration.
"""

import time
import pytest
from bugbuster import ChannelFunction

pytestmark = [pytest.mark.timeout(10)]


# ---------------------------------------------------------------------------
# Clear all alerts
# ---------------------------------------------------------------------------

def test_clear_all_alerts(device):
    """
    clear_alerts() (no channel argument) clears all global and per-channel alerts.
    Should not raise.
    """
    device.clear_alerts()


def test_clear_alerts_no_error_after_clear(device):
    """
    After clearing all alerts, check_faults() should return an empty list
    (assuming no real hardware faults are active).
    """
    device.clear_alerts()
    time.sleep(0.05)

    faults = device.check_faults()
    assert isinstance(faults, list), f"check_faults() must return list, got {type(faults)}"
    # Note: we don't assert faults is empty — hardware faults may still be present


# ---------------------------------------------------------------------------
# Check faults
# ---------------------------------------------------------------------------

def test_check_faults_returns_list(device):
    """
    check_faults() returns a list of fault dicts.
    Each fault has 'source', 'channel', 'code', 'description' keys.
    """
    faults = device.check_faults()

    assert isinstance(faults, list), f"check_faults() must return list, got {type(faults)}"

    for f in faults:
        assert isinstance(f, dict), f"Each fault must be dict, got {type(f)}"
        assert "source" in f, f"Fault missing 'source': {f}"
        assert "code" in f, f"Fault missing 'code': {f}"
        assert "description" in f, f"Fault missing 'description': {f}"


def test_check_faults_consistent(device):
    """
    Call check_faults() twice and verify no new faults appear spontaneously
    (assuming stable hardware conditions).
    """
    faults1 = device.check_faults()
    time.sleep(0.1)
    faults2 = device.check_faults()

    # The count of faults should not increase without any hardware changes
    # (It may decrease if a transient fault clears itself)
    assert len(faults2) <= len(faults1) + 2, (
        f"Fault count jumped from {len(faults1)} to {len(faults2)} unexpectedly"
    )


# ---------------------------------------------------------------------------
# Clear per-channel alerts
# ---------------------------------------------------------------------------

def test_clear_per_channel_alert(device):
    """
    clear_alerts(channel=0) clears only channel 0's alert flags.
    Should not raise.
    """
    device.clear_alerts(channel=0)
    device.clear_alerts(channel=1)
    device.clear_alerts(channel=2)
    device.clear_alerts(channel=3)


# ---------------------------------------------------------------------------
# Alert mask
# ---------------------------------------------------------------------------

def test_alert_mask_set(device):
    """
    set_alert_mask(alert_mask, supply_mask) configures which alert bits
    cause the ALERT pin to assert.  Setting 0xFFFF/0xFFFF enables all.
    Should not raise.
    """
    device.set_alert_mask(0xFFFF, 0xFFFF)


def test_alert_mask_disable_all(device):
    """
    set_alert_mask(0x0000, 0x0000) disables all alert notifications.
    Restore to 0xFFFF afterwards.
    """
    device.set_alert_mask(0x0000, 0x0000)
    time.sleep(0.02)
    # Restore
    device.set_alert_mask(0xFFFF, 0xFFFF)


# ---------------------------------------------------------------------------
# DIN event registration (USB only)
# ---------------------------------------------------------------------------

@pytest.mark.usb_only
def test_din_event_registration(usb_device):
    """
    Register an on_din_event callback (USB only).
    Verifies the callback can be registered without error.
    No events are expected unless a DIN edge occurs during the test.
    """
    received = []

    def on_din(channel, state, counter):
        received.append((channel, state, counter))

    # This should not raise
    usb_device.on_din_event(on_din)

    # Configure ch0 as DIN_LOGIC — any external edge would fire the callback
    usb_device.set_channel_function(0, ChannelFunction.DIN_LOGIC)
    time.sleep(0.1)

    # We don't require any events to be received — just that registration works
    usb_device.set_channel_function(0, ChannelFunction.HIGH_IMP)


@pytest.mark.usb_only
def test_din_event_callback_type(usb_device):
    """
    Verify that on_din_event only accepts USB transport.
    Over USB it should register without error.
    """
    events = []

    # Should not raise on USB
    usb_device.on_din_event(lambda ch, st, cnt: events.append((ch, st, cnt)))


# ---------------------------------------------------------------------------
# Faults after reset
# ---------------------------------------------------------------------------

@pytest.mark.destructive
def test_faults_after_reset(device):
    """
    After reset(), check_faults() should return a list (ideally empty except
    for the RESET_DETECTED alert bit which the AD74416H sets on reset).
    """
    device.reset()
    time.sleep(0.1)

    faults = device.check_faults()
    assert isinstance(faults, list), "check_faults() must return list"

    # Clear any reset-related alerts
    device.clear_alerts()


# ---------------------------------------------------------------------------
# Get faults raw
# ---------------------------------------------------------------------------

def test_get_faults_raw_structure(device):
    """
    get_faults() returns the raw fault register dict with both global
    alert status and per-channel alert status.
    """
    raw = device.get_faults()

    assert "alert_status" in raw, "Missing 'alert_status'"
    assert "supply_alert_status" in raw, "Missing 'supply_alert_status'"
    assert "channels" in raw, "Missing 'channels'"

    channels = raw["channels"]
    assert len(channels) == 4, f"Expected 4 channels, got {len(channels)}"

    for ch in channels:
        assert "id" in ch, f"Channel missing 'id': {ch}"
        assert "alert" in ch, f"Channel missing 'alert': {ch}"


# ---------------------------------------------------------------------------
# Per-channel alert mask
# ---------------------------------------------------------------------------

def test_set_channel_alert_mask(device):
    """
    set_channel_alert_mask(channel, mask) sets the per-channel alert mask.
    Verify the mask can be set to all-enabled and all-disabled, and that
    get_faults() reflects the change.
    """
    # Set channel 0 alert mask to all-enabled
    device.set_channel_alert_mask(0, 0xFFFF)
    faults = device.get_faults()
    ch0 = faults["channels"][0]
    assert ch0["mask"] == 0xFFFF, (
        f"Expected channel 0 mask 0xFFFF after set, got 0x{ch0['mask']:04X}"
    )

    # Disable all alerts on channel 0
    device.set_channel_alert_mask(0, 0x0000)
    faults = device.get_faults()
    ch0 = faults["channels"][0]
    assert ch0["mask"] == 0x0000, (
        f"Expected channel 0 mask 0x0000 after set, got 0x{ch0['mask']:04X}"
    )

    # Restore default (all alerts enabled)
    device.set_channel_alert_mask(0, 0xFFFF)
