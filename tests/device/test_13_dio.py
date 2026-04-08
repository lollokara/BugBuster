"""
test_13_dio.py — ESP32 GPIO digital I/O subsystem tests.

Covers: DIO configure (output/input/disabled), write, read, and get_all.

DIO mode values: 0=disabled, 1=input, 2=output.
IO numbering: 1–12 (logical IOs routed through MUX).
"""

import pytest
import bugbuster as bb
from conftest import assert_no_faults

pytestmark = [pytest.mark.timeout(10)]


# ---------------------------------------------------------------------------
# DIO configure output and write
# ---------------------------------------------------------------------------

def test_dio_configure_output(device):
    """
    Configure a DIO as output and write high then low.
    IO 2 is a safe choice for testing output mode.
    """
    device.dio_configure(2, 2)  # mode=2 → output
    device.dio_write(2, True)
    device.dio_write(2, False)

    # Restore to disabled
    device.dio_configure(2, 0)
    assert_no_faults(device)


# ---------------------------------------------------------------------------
# DIO configure input and read
# ---------------------------------------------------------------------------

def test_dio_configure_input(device):
    """
    Configure a DIO as input and read its level.
    IO 3 is used for input testing.
    """
    device.dio_configure(3, 1)  # mode=1 → input
    result = device.dio_read(3)

    assert isinstance(result, dict), f"dio_read() must return dict, got {type(result)}"
    assert "value" in result, f"dio_read() result missing 'value': {result}"
    assert isinstance(result["value"], bool), (
        f"dio_read() value must be bool, got {type(result['value'])}"
    )

    # Restore to disabled
    device.dio_configure(3, 0)
    assert_no_faults(device)


# ---------------------------------------------------------------------------
# DIO get all
# ---------------------------------------------------------------------------

def test_dio_get_all(device):
    """
    dio_get_all() returns a list of all DIO states.
    Each entry should have 'io', 'mode', 'output', and 'input' keys.
    """
    result = device.dio_get_all()

    assert isinstance(result, list), f"dio_get_all() must return list, got {type(result)}"
    assert len(result) > 0, "dio_get_all() returned empty list"

    for entry in result:
        assert isinstance(entry, dict), f"Each DIO entry must be dict, got {type(entry)}"
        assert "io" in entry, f"DIO entry missing 'io': {entry}"
        assert "mode" in entry, f"DIO entry missing 'mode': {entry}"
    assert_no_faults(device)


# ---------------------------------------------------------------------------
# DIO cleanup — restore disabled state
# ---------------------------------------------------------------------------

def test_dio_restore_disabled(device):
    """
    Ensure IOs 2 and 3 are set back to disabled (mode=0) after tests.
    """
    device.dio_configure(2, 0)
    device.dio_configure(3, 0)
    assert_no_faults(device)
