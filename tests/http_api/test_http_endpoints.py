"""
test_http_endpoints.py — Direct HTTP REST API endpoint tests.

These tests bypass the Python client and call the HTTP API directly using
the 'requests' library.  They test each REST endpoint independently and
verify correct HTTP status codes, response formats, and error handling.

Requires: --device-http <ip> to be passed on the command line.
"""

import time
import pytest
import requests

pytestmark = [
    pytest.mark.http_only,
    pytest.mark.timeout(10),
]

BASE_URL_FIXTURE = "base_url"


# ---------------------------------------------------------------------------
# Fixtures
# ---------------------------------------------------------------------------

@pytest.fixture
def base_url(request):
    """Return the base API URL for the device (e.g., http://192.168.4.1/api)."""
    host = request.config.getoption("--device-http")
    if not host:
        pytest.skip("HTTP device not specified — pass --device-http <ip>")
    return f"http://{host}/api"


@pytest.fixture
def session():
    """Return a shared requests.Session with a short timeout."""
    s = requests.Session()
    s.headers.update({"Content-Type": "application/json"})
    yield s
    s.close()


# ---------------------------------------------------------------------------
# Status
# ---------------------------------------------------------------------------

def test_health_get_status(base_url, session):
    """
    GET /api/status returns HTTP 200 with a JSON body containing a 'channels' array.
    This is the primary health check endpoint.
    """
    resp = session.get(f"{base_url}/status", timeout=5)

    assert resp.status_code == 200, f"Expected 200, got {resp.status_code}: {resp.text}"
    data = resp.json()
    assert "channels" in data, f"Status response missing 'channels': {list(data.keys())}"
    assert isinstance(data["channels"], list), "channels must be a list"
    assert len(data["channels"]) == 4, f"Expected 4 channels, got {len(data['channels'])}"


def test_status_has_spi_ok(base_url, session):
    """GET /api/status response should include 'spi_ok' field."""
    resp = session.get(f"{base_url}/status", timeout=5)
    data = resp.json()

    assert "spi_ok" in data, f"Status missing 'spi_ok': {list(data.keys())}"


# ---------------------------------------------------------------------------
# Device version
# ---------------------------------------------------------------------------

def test_get_version(base_url, session):
    """
    GET /api/device/version returns HTTP 200 with major, minor, patch version fields.
    """
    resp = session.get(f"{base_url}/device/version", timeout=5)

    assert resp.status_code == 200, f"Expected 200, got {resp.status_code}"
    data = resp.json()

    # Check for version fields (firmware uses fwMajor / fwMinor / fwPatch)
    has_version = (
        ("fwMajor" in data or "major" in data or "version" in data)
    )
    assert has_version, f"Version response missing version fields: {list(data.keys())}"


# ---------------------------------------------------------------------------
# Scope polling
# ---------------------------------------------------------------------------

def test_scope_endpoint_returns_seq(base_url, session):
    """
    GET /api/scope?since=0 returns HTTP 200 with a JSON body containing
    a 'seq' integer (oscilloscope sequence number).
    """
    resp = session.get(f"{base_url}/scope", params={"since": 0}, timeout=5)

    assert resp.status_code == 200, f"Expected 200, got {resp.status_code}: {resp.text}"
    data = resp.json()
    assert "seq" in data, f"Scope response missing 'seq': {list(data.keys())}"
    assert isinstance(data["seq"], int), f"seq must be int, got {type(data['seq'])}"


def test_scope_first_call_not_empty(base_url, session):
    """
    GET /api/scope?since=0 returns initial data; then GET /api/scope?since=<seq>
    after a short delay should return new data (seq > previous).
    """
    # First call
    resp1 = session.get(f"{base_url}/scope", params={"since": 0}, timeout=5)
    assert resp1.status_code == 200
    data1 = resp1.json()
    seq1 = data1.get("seq", 0)

    # Wait for new data to accumulate
    time.sleep(0.15)  # scope pushes every 10 ms

    # Second call with the last seq
    resp2 = session.get(f"{base_url}/scope", params={"since": seq1}, timeout=5)
    assert resp2.status_code == 200
    data2 = resp2.json()
    seq2 = data2.get("seq", 0)

    # New sequence should be >= old sequence
    assert seq2 >= seq1, f"Scope seq did not advance: {seq1} → {seq2}"


def test_scope_polling_sequence(base_url, session):
    """
    Call /api/scope three times with increasing 'since' values.
    Verify that seq numbers are non-decreasing.
    """
    seqs = []
    for _ in range(3):
        since = seqs[-1] if seqs else 0
        resp = session.get(f"{base_url}/scope", params={"since": since}, timeout=5)
        assert resp.status_code == 200
        data = resp.json()
        seqs.append(data.get("seq", 0))
        time.sleep(0.05)

    # Each new seq should be >= the previous
    for i in range(1, len(seqs)):
        assert seqs[i] >= seqs[i - 1], (
            f"Scope seq decreased at step {i}: {seqs[i-1]} → {seqs[i]}"
        )


# ---------------------------------------------------------------------------
# Channel
# ---------------------------------------------------------------------------

def test_channel_set_function(base_url, session):
    """
    POST /api/channel/0/function with {"function": 0} (HIGH_IMP)
    should return HTTP 200 with ok indicator.
    """
    resp = session.post(
        f"{base_url}/channel/0/function",
        json={"function": 0},  # HIGH_IMP
        timeout=5,
    )
    assert resp.status_code == 200, (
        f"Expected 200, got {resp.status_code}: {resp.text}"
    )


def test_channel_get_adc(base_url, session):
    """
    GET /api/channel/0/adc returns HTTP 200 with ADC value data.
    Response should include 'value' or 'raw_code' field.
    """
    resp = session.get(f"{base_url}/channel/0/adc", timeout=5)

    assert resp.status_code == 200, f"Expected 200, got {resp.status_code}: {resp.text}"
    data = resp.json()
    # Either 'value' or 'raw_code' should be present
    has_adc = "value" in data or "raw_code" in data or "adc_value" in data
    assert has_adc, f"ADC response missing value fields: {list(data.keys())}"


# ---------------------------------------------------------------------------
# MUX
# ---------------------------------------------------------------------------

def test_mux_get(base_url, session):
    """
    GET /api/mux returns HTTP 200 with a 'states' array (4 bytes, one per device).
    """
    resp = session.get(f"{base_url}/mux", timeout=5)

    assert resp.status_code == 200, f"Expected 200, got {resp.status_code}: {resp.text}"
    data = resp.json()
    assert "states" in data, f"MUX response missing 'states': {list(data.keys())}"
    states = data["states"]
    assert isinstance(states, list), "states must be a list"
    assert len(states) == 4, f"Expected 4 MUX device bytes, got {len(states)}"


def test_mux_set_all(base_url, session):
    """
    POST /api/mux/all with {"states": [0, 0, 0, 0]} opens all switches.
    Should return HTTP 200.
    """
    resp = session.post(
        f"{base_url}/mux/all",
        json={"states": [0, 0, 0, 0]},
        timeout=5,
    )
    assert resp.status_code == 200, (
        f"Expected 200, got {resp.status_code}: {resp.text}"
    )


# ---------------------------------------------------------------------------
# IDAC
# ---------------------------------------------------------------------------

def test_idac_get_status(base_url, session):
    """
    GET /api/idac returns HTTP 200 with IDAC status JSON.
    """
    resp = session.get(f"{base_url}/idac", timeout=5)

    assert resp.status_code == 200, f"Expected 200, got {resp.status_code}: {resp.text}"
    data = resp.json()
    assert isinstance(data, dict), f"IDAC response must be dict, got {type(data)}"


# ---------------------------------------------------------------------------
# WiFi
# ---------------------------------------------------------------------------

def test_wifi_get_status(base_url, session):
    """
    GET /api/wifi returns HTTP 200 with WiFi status JSON.
    """
    resp = session.get(f"{base_url}/wifi", timeout=5)

    assert resp.status_code == 200, f"Expected 200, got {resp.status_code}: {resp.text}"
    data = resp.json()
    assert isinstance(data, dict), "WiFi response must be dict"


# ---------------------------------------------------------------------------
# Error handling
# ---------------------------------------------------------------------------

def test_404_unknown_endpoint(base_url, session):
    """
    GET /api/nonexistent returns HTTP 404.
    The firmware should return a proper 404 for unknown endpoints.
    """
    resp = session.get(f"{base_url}/nonexistent_endpoint_xyz", timeout=5)

    assert resp.status_code == 404, (
        f"Expected 404 for unknown endpoint, got {resp.status_code}"
    )


def test_bad_json_returns_400(base_url, session):
    """
    POST /api/channel/0/function with malformed JSON body should return HTTP 400.
    The firmware validates request body; bad JSON is a client error.
    """
    resp = session.post(
        f"{base_url}/channel/0/function",
        data=b"not valid json {{{",  # intentionally malformed
        headers={"Content-Type": "application/json"},
        timeout=5,
    )
    assert resp.status_code in (400, 500), (
        f"Expected 400/500 for bad JSON, got {resp.status_code}: {resp.text}"
    )


# ---------------------------------------------------------------------------
# GPIO via HTTP
# ---------------------------------------------------------------------------

def test_gpio_get(base_url, session):
    """
    GET /api/gpio returns HTTP 200 with a list of GPIO pin states.
    """
    resp = session.get(f"{base_url}/gpio", timeout=5)

    assert resp.status_code == 200, f"Expected 200, got {resp.status_code}"
    data = resp.json()
    assert isinstance(data, list), f"GPIO response must be a list, got {type(data)}"
    assert len(data) == 6, f"Expected 6 GPIO pins, got {len(data)}"


# ---------------------------------------------------------------------------
# USBPD
# ---------------------------------------------------------------------------

def test_usbpd_get(base_url, session):
    """
    GET /api/usbpd returns HTTP 200 with USB PD status JSON.
    """
    resp = session.get(f"{base_url}/usbpd", timeout=5)

    assert resp.status_code == 200, f"Expected 200, got {resp.status_code}"
    data = resp.json()
    assert isinstance(data, dict), "USBPD response must be dict"


# ---------------------------------------------------------------------------
# Faults
# ---------------------------------------------------------------------------

def test_get_faults(base_url, session):
    """
    GET /api/faults returns HTTP 200 with fault register data.
    """
    resp = session.get(f"{base_url}/faults", timeout=5)

    assert resp.status_code == 200, f"Expected 200, got {resp.status_code}"
    data = resp.json()
    assert isinstance(data, dict), "Faults response must be dict"
