"""
test_08_wifi.py — WiFi management tests.

Covers: STA/AP status, scan results, field validation,
        AP password set (3-way persist semantics on --sim path).
These tests verify the WiFi management API works correctly without
attempting to connect to a network (which would require credentials).
"""

import pytest
from conftest import assert_no_faults

pytestmark = [pytest.mark.timeout(30)]  # WiFi scan can take up to 5 s


# ---------------------------------------------------------------------------
# WiFi status
# ---------------------------------------------------------------------------

def test_wifi_get_status(device):
    """
    wifi_get_status() returns a dict with at least 'connected' and IP fields.
    This test works over both USB and HTTP transports.
    """
    status = device.wifi_get_status()

    assert isinstance(status, dict), f"wifi_get_status() must return dict, got {type(status)}"
    assert "connected" in status or "sta_connected" in status or "sta_ssid" in status, (
        f"WiFi status missing connection keys: {list(status.keys())}"
    )
    assert_no_faults(device)


def test_wifi_status_has_required_fields(device):
    """
    Verify wifi_get_status() includes expected keys for both STA and AP modes.
    Required: some form of connection state and IP info.
    """
    status = device.wifi_get_status()

    # The USB parser returns: connected, sta_ssid, sta_ip, rssi, ap_ssid, ap_ip, ap_mac
    # HTTP may return different key names — check for either form
    has_sta_info = any(k in status for k in ("connected", "sta_connected", "sta_ssid"))

    assert has_sta_info, f"WiFi status missing STA connection info: {list(status.keys())}"
    # AP info is optional — device may not have AP mode enabled
    assert_no_faults(device)


def test_wifi_status_ip_is_string(device):
    """
    Verify that IP address fields in WiFi status are strings (may be empty).
    """
    status = device.wifi_get_status()

    # Check STA IP
    sta_ip = status.get("sta_ip") or status.get("ip", "")
    assert isinstance(sta_ip, str), f"STA IP must be str, got {type(sta_ip)}: {sta_ip!r}"
    assert_no_faults(device)


def test_wifi_status_rssi_is_numeric(device):
    """
    When connected to STA, RSSI should be a numeric value.
    If not connected, RSSI may be 0 or absent.
    """
    status = device.wifi_get_status()

    rssi = status.get("rssi")
    if rssi is not None:
        assert isinstance(rssi, (int, float)), f"rssi must be numeric, got {type(rssi)}"
        # RSSI is typically -120 to 0 dBm; positive values indicate not connected
        assert rssi <= 0 or rssi == 0, f"RSSI {rssi} is unexpectedly positive"
    assert_no_faults(device)


# ---------------------------------------------------------------------------
# WiFi scan
# ---------------------------------------------------------------------------

@pytest.mark.slow
def test_wifi_scan(device):
    """
    wifi_scan() returns a list of nearby WiFi networks (may be empty if none nearby).
    Should not crash, even if the scan finds no networks.
    """
    results = device.wifi_scan()

    assert isinstance(results, list), f"wifi_scan() must return list, got {type(results)}"
    # Scan may return zero results in RF-shielded environment — that is OK
    assert_no_faults(device)


@pytest.mark.slow
def test_wifi_scan_entry_format(device):
    """
    If wifi_scan() returns any results, each entry should be a dict
    with 'ssid', 'rssi', and 'auth' fields.
    """
    results = device.wifi_scan()

    for i, net in enumerate(results):
        assert isinstance(net, dict), f"Scan result[{i}] must be dict, got {type(net)}"
        assert "ssid" in net, f"Scan result[{i}] missing 'ssid': {net}"
        assert "rssi" in net, f"Scan result[{i}] missing 'rssi': {net}"
        assert isinstance(net["rssi"], (int, float)), (
            f"Scan result[{i}].rssi must be numeric"
        )
    assert_no_faults(device)


# ---------------------------------------------------------------------------
# Status consistency
# ---------------------------------------------------------------------------

def test_wifi_status_consistent(device):
    """
    Call wifi_get_status() twice and verify the AP information remains stable.
    AP config should not change between calls.
    """
    s1 = device.wifi_get_status()
    s2 = device.wifi_get_status()

    # AP SSID should be stable
    ap_ssid_1 = s1.get("ap_ssid", s1.get("ap_enabled"))
    ap_ssid_2 = s2.get("ap_ssid", s2.get("ap_enabled"))
    assert ap_ssid_1 == ap_ssid_2, (
        f"AP SSID changed between calls: {ap_ssid_1!r} vs {ap_ssid_2!r}"
    )
    assert_no_faults(device)


# ---------------------------------------------------------------------------
# WIFI_SET_AP_PASSWORD — 3-way status semantics (--sim path only)
# ---------------------------------------------------------------------------

def _get_sim_device(usb_device):
    """Extract the underlying SimulatedDevice from the transport, or None."""
    try:
        return usb_device._transport._device  # noqa: SLF001
    except AttributeError:
        return None


def test_wifi_set_ap_password_persisted(usb_device):
    """
    wifi_set_ap_password() returns True when the simulator returns 0x01 (persisted OK).
    """
    sim = _get_sim_device(usb_device)
    if sim is None:
        pytest.skip("Simulator-only test — requires --sim")

    sim.wifi_ap_password_persist_result = 0x01
    result = usb_device.wifi_set_ap_password("correct-horse-battery-staple")
    assert result is True, f"Expected True (persisted), got {result!r}"


def test_wifi_set_ap_password_nvs_skip(usb_device, monkeypatch):
    """
    wifi_set_ap_password() returns False when the simulator returns 0x00 (NVS skip).
    Status 0x00 means the password matched NVS and no write was needed.
    """
    sim = _get_sim_device(usb_device)
    if sim is None:
        pytest.skip("Simulator-only test — requires --sim")

    sim.wifi_ap_password_persist_result = 0x00
    result = usb_device.wifi_set_ap_password("correct-horse-battery-staple")
    # 0x00 is falsy — client.py returns bool(resp[0]) which is False
    assert result is False, f"Expected False (NVS skip / no-op), got {result!r}"


def test_wifi_set_ap_password_persist_failed(usb_device):
    """
    wifi_set_ap_password() returns True (non-zero byte) when the simulator
    returns 0x02 (persist failed).  The client converts any non-zero byte to True;
    callers that need the exact code must inspect the raw response.
    Status 0x02 is distinct from 0x00 (NVS skip) — the write was attempted but failed.
    """
    sim = _get_sim_device(usb_device)
    if sim is None:
        pytest.skip("Simulator-only test — requires --sim")

    sim.wifi_ap_password_persist_result = 0x02
    result = usb_device.wifi_set_ap_password("correct-horse-battery-staple")
    # 0x02 is truthy — bool(0x02) == True
    assert result is True, f"Expected True (persist-failed byte 0x02 is truthy), got {result!r}"


def test_wifi_set_ap_password_roundtrip(usb_device):
    """
    After calling wifi_set_ap_password(), the simulator stores the password on the device.
    """
    sim = _get_sim_device(usb_device)
    if sim is None:
        pytest.skip("Simulator-only test — requires --sim")

    sim.wifi_ap_password_persist_result = 0x01
    password = "my-secret-pass"
    usb_device.wifi_set_ap_password(password)
    assert sim.wifi_ap_password == password, (
        f"Simulator did not store password: expected {password!r}, got {sim.wifi_ap_password!r}"
    )
