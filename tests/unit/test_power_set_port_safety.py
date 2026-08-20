"""Unit tests for power_set_port() safety guards (2026-08-20)."""
import pytest
from unittest.mock import MagicMock

from bugbuster import BugBuster
from bugbuster.transport.usb import USBTransport


@pytest.fixture
def mock_client():
    """A BugBuster client with a mocked USB transport."""
    mock_transport = MagicMock(spec=USBTransport)
    # Default response for PCA_SET_PORT: echo port=0, value=0x55
    mock_transport.send_command.return_value = bytes([0, 0x55])
    
    client = BugBuster(mock_transport)
    yield client


def test_power_set_port_requires_confirmation(mock_client):
    """power_set_port() refuses to run without confirm=True."""
    with pytest.raises(ValueError, match="requires confirm=True"):
        mock_client.power_set_port(0, 0xFF)


def test_power_set_port_rejects_invalid_port(mock_client):
    """power_set_port() rejects port numbers outside 0-1."""
    with pytest.raises(ValueError, match="port must be 0 or 1"):
        mock_client.power_set_port(2, 0xFF, confirm=True)
    
    with pytest.raises(ValueError, match="port must be 0 or 1"):
        mock_client.power_set_port(-1, 0xFF, confirm=True)


def test_power_set_port_refuses_clearing_logic_en(mock_client):
    """Clearing bit 0 (LOGIC_EN) on port 0 is rejected even with confirm=True."""
    # 0xFE = 11111110 - bit 0 is cleared.
    with pytest.raises(ValueError, match="bit 0 \\(LOGIC_EN\\) must be set"):
        mock_client.power_set_port(0, 0xFE, confirm=True)


def test_power_set_port_refuses_clearing_usb_hub(mock_client):
    """Clearing bit 7 (EN_USB_HUB) on port 0 is rejected even with confirm=True."""
    # 0x7F = 01111111 - bit 7 is cleared.
    with pytest.raises(ValueError, match="bit 7 \\(EN_USB_HUB\\) must be set"):
        mock_client.power_set_port(0, 0x7F, confirm=True)


def test_power_set_port_refuses_clearing_both_critical_bits(mock_client):
    """Clearing both critical bits (0 and 7) on port 0 is rejected."""
    # 0x7E = 01111110 - both bit 0 and bit 7 are cleared.
    with pytest.raises(ValueError, match="bit 0 \\(LOGIC_EN\\).*bit 7 \\(EN_USB_HUB\\)"):
        mock_client.power_set_port(0, 0x7E, confirm=True)


def test_power_set_port_allows_safe_port0_value(mock_client):
    """A port 0 value with bits 0 and 7 set is allowed."""
    # 0x81 = 10000001 - bits 0 and 7 are both set, all else cleared.
    mock_client._t.send_command.return_value = bytes([0, 0x81])
    port, value = mock_client.power_set_port(0, 0x81, confirm=True)
    assert port == 0
    assert value == 0x81
    # Verify the command was sent.
    assert mock_client._t.send_command.called


def test_power_set_port_allows_any_port1_value(mock_client):
    """Port 1 has no critical bits - any value is allowed with confirm=True."""
    mock_client._t.send_command.return_value = bytes([1, 0x00])
    port, value = mock_client.power_set_port(1, 0x00, confirm=True)
    assert port == 1
    assert value == 0x00
    
    mock_client._t.send_command.return_value = bytes([1, 0xFF])
    port, value = mock_client.power_set_port(1, 0xFF, confirm=True)
    assert port == 1
    assert value == 0xFF


def test_power_set_port_error_message_points_to_power_set(mock_client):
    """Error messages recommend power_set() as the safer alternative."""
    try:
        mock_client.power_set_port(0, 0xFF)
    except ValueError as e:
        assert "power_set()" in str(e)
    
    try:
        mock_client.power_set_port(0, 0x00, confirm=True)
    except ValueError as e:
        assert "power_set()" in str(e)


def test_power_set_port_docstring_documents_bit_layout():
    """The docstring must document the PCA9535 bit layout."""
    from bugbuster.client import BugBuster
    doc = BugBuster.power_set_port.__doc__
    assert doc is not None
    # Check for key bit names.
    assert "LOGIC_EN" in doc
    assert "EN_USB_HUB" in doc
    assert "VADJ1_EN" in doc
    assert "EFUSE_EN" in doc
    # Check that it points to the safer API.
    assert "power_set" in doc
    # Check that it warns about danger.
    assert "dangerous" in doc.lower() or "critical" in doc.lower()
