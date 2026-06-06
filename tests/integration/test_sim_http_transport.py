"""
Transport-layer tests for --sim-full mode.

These tests specifically probe HTTP transport behaviors invisible to in-process
--sim dispatch: admin-token header injection, HTTP status code mapping, 404
handling, and full GET/POST roundtrips via real HTTP requests.

All tests are skipped automatically unless --sim-full is passed.
"""

import pytest
import requests

from tests.mock import SimulatedDevice
from tests.mock.sim_http_server import SimHTTPServer


_SIM_FULL_ADMIN_TOKEN = "aa" * 32


@pytest.fixture(autouse=True)
def require_sim_full(request):
    if not request.config.getoption("--sim-full", default=False):
        pytest.skip("--sim-full not set")


@pytest.fixture
def server_and_transport():
    """Start a SimHTTPServer + HTTPTransport pair; tear down after test."""
    from bugbuster.transport.http import HTTPTransport

    device = SimulatedDevice()
    server = SimHTTPServer(device, admin_token=_SIM_FULL_ADMIN_TOKEN)
    server.start()

    transport = HTTPTransport(
        host="127.0.0.1",
        port=server.port,
        admin_token=_SIM_FULL_ADMIN_TOKEN,
    )
    yield server, transport, device
    server.stop()


# ---------------------------------------------------------------------------
# GET roundtrip
# ---------------------------------------------------------------------------

def test_get_device_version_roundtrip(server_and_transport):
    """Full GET path: real HTTP request, real JSON parse, fw_version populated."""
    server, transport, device = server_and_transport
    info = transport.connect()
    assert info["fwMajor"] == device.fw_version[0]
    assert info["fwMinor"] == device.fw_version[1]
    assert info["fwPatch"] == device.fw_version[2]
    assert transport.fw_version == device.fw_version


# ---------------------------------------------------------------------------
# Admin token enforcement
# ---------------------------------------------------------------------------

def test_post_with_correct_admin_token_succeeds(server_and_transport):
    """POST with correct admin token header returns 200 (not 401)."""
    server, transport, _ = server_and_transport
    transport.connect()
    # set_dac_voltage is a POST; if admin token is wrong it returns 401 → raises
    result = transport.post("/channel/0/dac", {"voltage": 1.0})
    assert "error" not in result


def test_post_missing_admin_token_raises_401(server_and_transport):
    """POST without admin token raises HTTPError with 401 status."""
    from bugbuster.transport.http import HTTPTransport

    server, _, _ = server_and_transport
    # Create a transport without a token
    no_token_transport = HTTPTransport(
        host="127.0.0.1",
        port=server.port,
        admin_token=None,
    )
    with pytest.raises(requests.HTTPError) as exc_info:
        no_token_transport.post("/channel/0/dac/voltage", {"voltage": 1.0})
    assert exc_info.value.response.status_code == 401


def test_post_wrong_admin_token_raises_401(server_and_transport):
    """POST with an incorrect admin token raises HTTPError with 401 status."""
    from bugbuster.transport.http import HTTPTransport

    server, _, _ = server_and_transport
    wrong_transport = HTTPTransport(
        host="127.0.0.1",
        port=server.port,
        admin_token="bb" * 32,
    )
    with pytest.raises(requests.HTTPError) as exc_info:
        wrong_transport.post("/channel/0/dac/voltage", {"voltage": 1.0})
    assert exc_info.value.response.status_code == 401


# ---------------------------------------------------------------------------
# 404 handling
# ---------------------------------------------------------------------------

def test_unknown_route_returns_404(server_and_transport):
    """GET on an unimplemented path raises HTTPError with 404."""
    server, transport, _ = server_and_transport
    with pytest.raises(requests.HTTPError) as exc_info:
        transport.get("/nonexistent/route/that/does/not/exist")
    assert exc_info.value.response.status_code == 404


# ---------------------------------------------------------------------------
# Full BugBuster client roundtrip
# ---------------------------------------------------------------------------

def test_bugbuster_client_over_real_http(server_and_transport):
    """BugBuster client works end-to-end over a real localhost HTTP server."""
    import bugbuster as bb

    server, transport, _ = server_and_transport
    transport.connect()
    dev = bb.BugBuster(transport)

    info = dev.get_device_info()
    assert info is not None
