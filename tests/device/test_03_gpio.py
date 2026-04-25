"""
test_03_gpio.py — AD74416H GPIO pin tests (pins A–F, indices 0–5).

Tests GPIO read-back, output high/low, input mode, high-impedance mode,
and cycling through all 6 pins in OUTPUT mode.
"""

import time
import pytest
from bugbuster import GpioMode
from conftest import assert_no_faults

pytestmark = [pytest.mark.timeout(10)]


# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

def _restore_gpio(device, *pins, mode=GpioMode.HIGH_IMP):
    """Restore pins to a safe mode after tests."""
    for pin in pins:
        try:
            device.set_gpio_config(pin, mode)
        except Exception:
            pass


# ---------------------------------------------------------------------------
# Read all GPIOs
# ---------------------------------------------------------------------------

def test_gpio_get_all(device):
    """
    Call get_gpio() and verify it returns a list of exactly 6 GpioStatus namedtuples.
    Each entry must have id, mode, output, input, pulldown fields.
    """
    pins = device.get_gpio()

    assert isinstance(pins, list), f"get_gpio() must return a list, got {type(pins)}"
    assert len(pins) == 6, f"Expected 6 GPIO pins, got {len(pins)}"

    for i, pin in enumerate(pins):
        assert hasattr(pin, "id"), f"pins[{i}] missing 'id'"
        assert hasattr(pin, "mode"), f"pins[{i}] missing 'mode'"
        assert hasattr(pin, "output"), f"pins[{i}] missing 'output'"
        assert hasattr(pin, "input"), f"pins[{i}] missing 'input'"
        assert hasattr(pin, "pulldown"), f"pins[{i}] missing 'pulldown'"

    assert_no_faults(device)


# ---------------------------------------------------------------------------
# GPIO output high
# ---------------------------------------------------------------------------

def test_gpio_set_output_high(device):
    """
    Configure GPIO pin 0 as OUTPUT and drive it HIGH.
    Verifies no exception is raised.
    """
    device.set_gpio_config(0, GpioMode.OUTPUT)
    device.set_gpio_value(0, True)
    time.sleep(0.02)
    _restore_gpio(device, 0)
    assert_no_faults(device)


# ---------------------------------------------------------------------------
# GPIO output low
# ---------------------------------------------------------------------------

def test_gpio_set_output_low(device):
    """
    Configure GPIO pin 0 as OUTPUT and drive it LOW.
    Verifies no exception is raised.
    """
    device.set_gpio_config(0, GpioMode.OUTPUT)
    device.set_gpio_value(0, False)
    time.sleep(0.02)
    _restore_gpio(device, 0)
    assert_no_faults(device)


# ---------------------------------------------------------------------------
# GPIO input mode
# ---------------------------------------------------------------------------

def test_gpio_set_input(device):
    """
    Configure GPIO pin 0 as INPUT mode.
    Verifies the mode can be set without error.
    """
    device.set_gpio_config(0, GpioMode.INPUT)
    time.sleep(0.02)

    # Read back and confirm the mode was accepted
    pins = device.get_gpio()
    pin0 = next((p for p in pins if p.id == 0), None)
    if pin0 is not None:
        assert pin0.mode == GpioMode.INPUT, (
            f"GPIO 0 mode should be INPUT after set, got {pin0.mode}"
        )

    _restore_gpio(device, 0)
    assert_no_faults(device)


# ---------------------------------------------------------------------------
# GPIO high-impedance
# ---------------------------------------------------------------------------

def test_gpio_set_high_imp(device):
    """
    Configure GPIO pin 0 as HIGH_IMP (input buffer off / safe floating).
    """
    device.set_gpio_config(0, GpioMode.HIGH_IMP)
    time.sleep(0.02)

    pins = device.get_gpio()
    pin0 = next((p for p in pins if p.id == 0), None)
    if pin0 is not None:
        assert pin0.mode == GpioMode.HIGH_IMP, (
            f"GPIO 0 mode should be HIGH_IMP after set, got {pin0.mode}"
        )

    assert_no_faults(device)


# ---------------------------------------------------------------------------
# Cycle through all pins
# ---------------------------------------------------------------------------

def test_gpio_all_pins_output_mode(device):
    """
    Cycle through all 6 GPIO pins (0–5), configure each as OUTPUT,
    toggle high then low, and restore to HIGH_IMP.
    """
    for pin_idx in range(6):
        device.set_gpio_config(pin_idx, GpioMode.OUTPUT)
        device.set_gpio_value(pin_idx, True)
        time.sleep(0.01)
        device.set_gpio_value(pin_idx, False)
        time.sleep(0.01)
        device.set_gpio_config(pin_idx, GpioMode.HIGH_IMP)

    assert_no_faults(device)


# ---------------------------------------------------------------------------
# GPIO toggle
# ---------------------------------------------------------------------------

def test_gpio_toggle_output(device):
    """
    Configure GPIO pin 1 as OUTPUT, toggle it 5 times, then restore.
    Verifies repeated set_gpio_value calls work reliably.
    """
    device.set_gpio_config(1, GpioMode.OUTPUT)
    for _ in range(5):
        device.set_gpio_value(1, True)
        time.sleep(0.01)
        device.set_gpio_value(1, False)
        time.sleep(0.01)
    _restore_gpio(device, 1)
    assert_no_faults(device)


# ---------------------------------------------------------------------------
# GPIO with pulldown
# ---------------------------------------------------------------------------

def test_gpio_set_input_with_pulldown(device):
    """
    Configure GPIO pin 2 as INPUT with pulldown enabled.
    Should not raise.
    """
    device.set_gpio_config(2, GpioMode.INPUT, pulldown=True)
    time.sleep(0.02)
    _restore_gpio(device, 2)
    assert_no_faults(device)


# ---------------------------------------------------------------------------
# Multiple GPIO simultaneously
# ---------------------------------------------------------------------------

def test_gpio_multiple_simultaneous(device):
    """
    Configure pins 0 and 1 as OUTPUT while pins 2 and 3 are INPUT.
    Verifies that mixed-mode GPIO configuration does not conflict.
    """
    device.set_gpio_config(0, GpioMode.OUTPUT)
    device.set_gpio_config(1, GpioMode.OUTPUT)
    device.set_gpio_config(2, GpioMode.INPUT)
    device.set_gpio_config(3, GpioMode.INPUT)

    device.set_gpio_value(0, True)
    device.set_gpio_value(1, False)
    time.sleep(0.05)

    pins = device.get_gpio()
    assert len(pins) == 6, f"Expected 6 pins after mixed config, got {len(pins)}"

    _restore_gpio(device, 0, 1, 2, 3)
    assert_no_faults(device)
