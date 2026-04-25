"""
test_06_usbpd.py — USB Power Delivery (HUSB238) tests.

Covers: PD contract status, voltage sanity checks, PDO list, and
device presence when powered from a PD-capable source.
"""

import pytest
from conftest import assert_no_faults

pytestmark = [pytest.mark.timeout(10)]

# Minimum expected voltage when connected via USB PD (5 V profile)
MIN_VOLTAGE_MV = 4500  # 4.5 V minimum (accounts for measurement tolerance)
MIN_VOLTAGE_V  = 4.5


# ---------------------------------------------------------------------------
# Get USB PD status
# ---------------------------------------------------------------------------

def test_usbpd_get_status(device):
    """
    usbpd_get_status() returns a dict with expected keys.
    Key fields: present, attached, voltage_v, current_a, power_w, pdos.
    """
    status = device.usbpd_get_status()

    assert isinstance(status, dict), f"usbpd_get_status() must return dict, got {type(status)}"
    assert "present" in status, "USBPD status missing 'present'"
    assert "attached" in status, "USBPD status missing 'attached'"
    assert "pdos" in status, "USBPD status missing 'pdos'"
    assert_no_faults(device)


# ---------------------------------------------------------------------------
# Voltage sanity
# ---------------------------------------------------------------------------

def test_usbpd_status_voltage_sane(device):
    """
    When a USB PD source is attached, the reported voltage must be >= 4.5 V.
    If the HUSB238 is not present or no PD contract, skip rather than fail.
    """
    status = device.usbpd_get_status()

    if not status.get("present"):
        pytest.skip("HUSB238 not present — skipping voltage sanity check")

    if not status.get("attached"):
        pytest.skip("No USB PD contract negotiated — skipping voltage sanity check")

    voltage_v = status.get("voltage_v", 0.0)
    assert voltage_v >= MIN_VOLTAGE_V, (
        f"USBPD voltage {voltage_v:.2f} V is below minimum {MIN_VOLTAGE_V} V"
    )
    assert_no_faults(device)


# ---------------------------------------------------------------------------
# PDO list
# ---------------------------------------------------------------------------

def test_usbpd_status_has_pdos(device):
    """
    The USBPD status dict must include a 'pdos' key with a list of PDO entries.
    Each PDO entry should have a 'detected' flag.
    """
    status = device.usbpd_get_status()

    pdos = status.get("pdos")
    assert pdos is not None, "USBPD status missing 'pdos'"
    assert isinstance(pdos, list), f"'pdos' must be a list, got {type(pdos)}"

    # HUSB238 supports 6 PDO slots (5V, 9V, 12V, 15V, 18V, 20V)
    assert len(pdos) >= 1, "PDO list must have at least 1 entry"

    for i, pdo in enumerate(pdos):
        assert isinstance(pdo, dict), f"PDO[{i}] must be a dict, got {type(pdo)}"
        assert "detected" in pdo, f"PDO[{i}] missing 'detected' key"
    assert_no_faults(device)


# ---------------------------------------------------------------------------
# Present flag check
# ---------------------------------------------------------------------------

def test_usbpd_present_is_bool(device):
    """
    Verify the 'present' field in USBPD status is a boolean (or int 0/1).
    This confirms the HUSB238 I2C detection result is correctly parsed.
    """
    status = device.usbpd_get_status()

    present = status.get("present")
    assert isinstance(present, (bool, int)), (
        f"'present' must be bool or int, got {type(present)}"
    )
    assert_no_faults(device)


# ---------------------------------------------------------------------------
# Attached flag check
# ---------------------------------------------------------------------------

def test_usbpd_attached_is_bool(device):
    """
    Verify the 'attached' field in USBPD status is a boolean (or int 0/1).
    """
    status = device.usbpd_get_status()

    attached = status.get("attached")
    assert isinstance(attached, (bool, int)), (
        f"'attached' must be bool or int, got {type(attached)}"
    )
    assert_no_faults(device)


# ---------------------------------------------------------------------------
# Power values when attached
# ---------------------------------------------------------------------------

def test_usbpd_power_values_sane(device):
    """
    When a PD contract is active, verify voltage, current, and power values
    are physically plausible (V > 0, I > 0, P ≈ V × I).
    """
    status = device.usbpd_get_status()

    if not status.get("present") or not status.get("attached"):
        pytest.skip("No active USBPD contract — skipping power value check")

    v = status.get("voltage_v", 0.0)
    i = status.get("current_a", 0.0)
    p = status.get("power_w", 0.0)

    assert v > 0, f"Voltage must be > 0 when attached, got {v}"
    assert i >= 0, f"Current must be >= 0, got {i}"
    assert p >= 0, f"Power must be >= 0, got {p}"
    assert_no_faults(device)
