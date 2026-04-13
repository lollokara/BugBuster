"""Core round-trip tests for SimulatedDevice via USB and HTTP transports."""
import sys
import os

import pytest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))

import bugbuster as bb
from tests.mock import SimulatedDevice, SimulatedUSBTransport, SimulatedHTTPTransport


def _usb_client():
    device = SimulatedDevice()
    transport = SimulatedUSBTransport(device)
    client = bb.BugBuster(transport)
    client.connect()
    return client, device


def _http_client(hat=False):
    device = SimulatedDevice()
    transport = SimulatedHTTPTransport(device, hat=hat)
    client = bb.BugBuster(transport)
    client.connect()
    return client, device


# ---------------------------------------------------------------------------
# get_status
# ---------------------------------------------------------------------------

@pytest.mark.parametrize("make_client", [_usb_client, _http_client])
def test_get_status_structure(make_client):
    """get_status() returns dict with required keys regardless of transport."""
    client, _ = make_client()
    status = client.get_status()
    assert isinstance(status, dict)
    assert "channels" in status
    assert isinstance(status["channels"], list)
    assert len(status["channels"]) == 4
    client.disconnect()


# ---------------------------------------------------------------------------
# get_device_info
# ---------------------------------------------------------------------------

@pytest.mark.parametrize("make_client", [_usb_client, _http_client])
def test_get_device_info_fields(make_client):
    """get_device_info() returns a DeviceInfo namedtuple with spi_ok field."""
    client, _ = make_client()
    info = client.get_device_info()
    assert hasattr(info, "spi_ok")
    assert info.spi_ok is True
    client.disconnect()


# ---------------------------------------------------------------------------
# hat_detect
# ---------------------------------------------------------------------------

def test_hat_detect_no_hat():
    """hat_detect() returns detected=False when hat_present=False."""
    client, _ = _http_client(hat=False)
    result = client.hat_detect()
    detected = result.detected if hasattr(result, "detected") else result.get("detected")
    assert not detected
    client.disconnect()


def test_hat_detect_with_hat():
    """hat_detect() returns detected=True when hat_present=True."""
    client, _ = _http_client(hat=True)
    result = client.hat_detect()
    detected = result.detected if hasattr(result, "detected") else result.get("detected")
    assert detected
    client.disconnect()


# ---------------------------------------------------------------------------
# IDAC status
# ---------------------------------------------------------------------------

def test_idac_get_status_usb():
    """idac_get_status() over USB returns present=True and 4 channels."""
    client, _ = _usb_client()
    status = client.idac_get_status()
    assert status["present"] is True
    assert len(status["channels"]) == 4
    client.disconnect()


def test_idac_get_status_http():
    """idac_get_status() over HTTP returns present=True and channels list."""
    client, _ = _http_client()
    status = client.idac_get_status()
    assert status["present"] is True
    assert isinstance(status["channels"], list)
    assert len(status["channels"]) >= 2
    client.disconnect()


# ---------------------------------------------------------------------------
# USB PD status
# ---------------------------------------------------------------------------

def test_usbpd_status_http():
    """usbpd_get_status() over HTTP returns required fields."""
    client, _ = _http_client()
    status = client.usbpd_get_status()
    assert "present" in status
    assert "attached" in status
    assert "pdos" in status
    assert isinstance(status["pdos"], list)
    client.disconnect()


# ---------------------------------------------------------------------------
# WiFi scan
# ---------------------------------------------------------------------------

def test_wifi_scan_http_returns_list():
    """wifi_scan() over HTTP returns a list."""
    client, _ = _http_client()
    results = client.wifi_scan()
    assert isinstance(results, list)
    client.disconnect()


# ---------------------------------------------------------------------------
# Power status
# ---------------------------------------------------------------------------

def test_power_get_status_http():
    """power_get_status() over HTTP returns dict with 'present' key."""
    client, _ = _http_client()
    status = client.power_get_status()
    assert isinstance(status, dict)
    assert "present" in status
    client.disconnect()
