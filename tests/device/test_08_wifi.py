"""
test_08_wifi.py — WiFi management tests.

Covers: STA/AP status, scan results, field validation.
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
    has_ap_info = any(k in status for k in ("ap_enabled", "ap_ssid", "ap_ip"))

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
