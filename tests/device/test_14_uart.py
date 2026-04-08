"""
test_14_uart.py — UART bridge configuration tests (USB only).

Covers: get_uart_config, get_uart_pins, set_uart_config.

UART bridge functions require USB transport (binary protocol).
"""

import pytest
import bugbuster as bb
from conftest import assert_no_faults

pytestmark = [
    pytest.mark.usb_only,
    pytest.mark.timeout(10),
]


# ---------------------------------------------------------------------------
# Get UART config
# ---------------------------------------------------------------------------

def test_get_uart_config(usb_device):
    """
    get_uart_config() returns a list of UART bridge configurations.
    Each entry should have 'baudrate' and 'enabled' keys.
    """
    configs = usb_device.get_uart_config()

    assert isinstance(configs, list), (
        f"get_uart_config() must return list, got {type(configs)}"
    )
    # There should be at least one bridge configured
    assert len(configs) >= 1, "get_uart_config() returned empty list"

    for i, cfg in enumerate(configs):
        assert isinstance(cfg, dict), f"Bridge {i} config must be dict, got {type(cfg)}"
        assert "baudrate" in cfg, f"Bridge {i} config missing 'baudrate': {cfg}"
        assert "enabled" in cfg, f"Bridge {i} config missing 'enabled': {cfg}"
    assert_no_faults(usb_device)


# ---------------------------------------------------------------------------
# Get UART pins
# ---------------------------------------------------------------------------

def test_get_uart_pins(usb_device):
    """
    get_uart_pins() returns a list of available ESP32 GPIO pin numbers
    that can be used for UART routing.
    """
    pins = usb_device.get_uart_pins()

    assert isinstance(pins, list), f"get_uart_pins() must return list, got {type(pins)}"
    # Should have at least a few available pins
    assert len(pins) >= 1, "get_uart_pins() returned empty list"

    for pin in pins:
        assert isinstance(pin, int), f"Each pin must be int, got {type(pin)}"
    assert_no_faults(usb_device)


# ---------------------------------------------------------------------------
# Set UART config (non-destructive)
# ---------------------------------------------------------------------------

def test_set_uart_config(usb_device):
    """
    Set a UART bridge configuration with the bridge disabled to avoid
    interfering with other IO.  Verifies the command completes without error.
    """
    # Read current config to know the original state
    original = usb_device.get_uart_config()

    # Set a known config with bridge disabled
    usb_device.set_uart_config(
        bridge_id=0,
        uart_num=1,
        tx_pin=17,
        rx_pin=18,
        baudrate=115200,
        data_bits=8,
        parity=0,
        stop_bits=0,
        enabled=False,  # disabled to avoid interfering
    )

    # Read back to verify the command was accepted
    configs = usb_device.get_uart_config()
    assert isinstance(configs, list), "get_uart_config() must return list after set"
    assert len(configs) >= 1, "Expected at least one bridge config after set"

    # Restore original config if there was one
    if original:
        orig = original[0]
        usb_device.set_uart_config(
            bridge_id=orig["bridge_id"],
            uart_num=orig["uart_num"],
            tx_pin=orig["tx_pin"],
            rx_pin=orig["rx_pin"],
            baudrate=orig["baudrate"],
            data_bits=orig["data_bits"],
            parity=orig["parity"],
            stop_bits=orig["stop_bits"],
            enabled=orig["enabled"],
        )
    assert_no_faults(usb_device)
