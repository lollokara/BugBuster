"""WEB-24: GET /api/daq/wifi_stream/status returned the DAQ hotspot SSID and
WPA password to any LAN host while the stream was READY. The shared
api_core_handle() has no request context (BLE uses it too), so the HTTP
handler must strip the credentials unless the admin token checks out."""

import pytest

from tests.firmware_host.fwhost import extract_function

WEBSERVER = "Firmware/ESP32/src/web/webserver.cpp"


@pytest.mark.xfail(strict=True, reason="WEB-24")
def test_wifi_stream_status_hides_credentials_without_auth():
    body = extract_function(WEBSERVER, r"^static esp_err_t handle_get_daq_wifi_stream_status\(")
    assert "check_admin_auth(req)" in body
    assert 'cJSON_DeleteItemFromObject' in body and '"password"' in body
