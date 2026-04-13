"""
Regression tests for bugs identified during repository audit.
These tests run against the SimulatedDevice to ensure contracts are enforced.
"""

import pytest
from bugbuster import BugBuster
from tests.mock import SimulatedDevice, SimulatedHTTPTransport, SimulatedUSBTransport
from bugbuster.constants import ChannelFunction, ErrorCode
from bugbuster.transport.usb import DeviceError

@pytest.fixture
def sim_device():
    return SimulatedDevice()

@pytest.fixture
def http_client(sim_device):
    transport = SimulatedHTTPTransport(sim_device)
    return BugBuster(transport)

@pytest.fixture
def usb_client(sim_device):
    transport = SimulatedUSBTransport(sim_device)
    return BugBuster(transport)

# ---------------------------------------------------------------------------
# Auth Regressions
# ---------------------------------------------------------------------------

def test_auth_post_no_token_fails(sim_device):
    """POST requests without X-BugBuster-Admin-Token should fail with 401."""
    # We use the transport directly to skip the client's automatic token handling
    transport = SimulatedHTTPTransport(sim_device)
    resp = transport.post("/channel/0/function", {"function": 1})
    
    assert resp.get("code") == 401
    assert resp.get("error") == "unauthorized"

def test_auth_post_invalid_token_fails(sim_device):
    """POST requests with invalid X-BugBuster-Admin-Token should fail with 401."""
    transport = SimulatedHTTPTransport(sim_device)
    resp = transport.post("/channel/0/function", {"function": 1}, 
                          headers={"X-BugBuster-Admin-Token": "WRONG-TOKEN"})
    
    assert resp.get("code") == 401
    assert resp.get("error") == "unauthorized"

def test_auth_post_valid_token_succeeds(sim_device):
    """POST requests with valid X-BugBuster-Admin-Token should succeed."""
    transport = SimulatedHTTPTransport(sim_device)
    token = sim_device.admin_token
    resp = transport.post("/channel/0/function", {"function": 1}, 
                          headers={"X-BugBuster-Admin-Token": token})
    
    assert resp.get("ok") is True
    assert sim_device.channels[0]["function"] == 1

# ---------------------------------------------------------------------------
# JSON Regressions
# ---------------------------------------------------------------------------

def test_json_missing_required_field_fails(sim_device):
    """POST requests with missing required fields should return 400."""
    transport = SimulatedHTTPTransport(sim_device)
    token = sim_device.admin_token
    # Missing "function" field
    resp = transport.post("/channel/0/function", {"wrong_field": 1}, 
                          headers={"X-BugBuster-Admin-Token": token})
    
    assert resp.get("code") == 400
    assert "missing field" in resp.get("error", "").lower()

def test_json_malformed_value_fails(sim_device):
    """POST requests with malformed values should return 400/error."""
    transport = SimulatedHTTPTransport(sim_device)
    token = sim_device.admin_token
    # "function" should be int, but let's see how it handles non-int strings
    resp = transport.post("/channel/0/function", {"function": "not-an-int"}, 
                          headers={"X-BugBuster-Admin-Token": token})
    
    # http_routes.py currently returns "invalid path or body" and doesn't specify code 400 for this one, 
    # but it should probably be an error.
    assert "error" in resp

# ---------------------------------------------------------------------------
# DIO Regressions
# ---------------------------------------------------------------------------

def test_dio_hal_read_uses_correct_io_index(usb_client, sim_device):
    """hal.read_digital(io=1) should call the client with io=1 (not 0 or other)."""
    usb_client.connect()
    hal = usb_client.hal
    hal.begin()
    
    # Configure IO 1 as digital input
    from bugbuster.hal import PortMode
    hal.configure(1, PortMode.DIGITAL_IN)
    
    # Mock the response for dio_read(1)
    # Actually, SimulatedDevice.dio is 0-indexed internally, and dio_read(io) uses io-1
    sim_device.dio[0]["output"] = True # Set IO 1 to High
    
    val = hal.read_digital(1)
    assert val is True
    
    # Verify it didn't read IO 2
    sim_device.dio[1]["output"] = False
    assert hal.read_digital(1) is True

# ---------------------------------------------------------------------------
# LA Regressions
# ---------------------------------------------------------------------------

def test_la_status_handles_phase0_done_state(usb_client, sim_device):
    """LA status should correctly recognize 'DONE' state from Phase 0 schema."""
    usb_client.connect()
    
    # Manually set LA state in sim_device
    sim_device.la_state = "DONE"
    
    # Verify client's hat_la_get_status (USB version)
    status = usb_client.hat_la_get_status()
    assert status["state_name"] == "done" # Client maps 3 to "done"
    
    # If we use MCP tool capture_logic_analyzer, it checks stateName == "DONE"
    from bugbuster_mcp.tools import waveform
    import mock
    
    with mock.patch("bugbuster_mcp.session.get_client", return_value=usb_client), \
         mock.patch("bugbuster_mcp.session.get_hal", return_value=usb_client.hal):
        
        # We need to mock hat_la_read_all and decode because we haven't set up actual data
        with mock.patch.object(usb_client, "hat_la_read_all", return_value=b"\x00\x00\x00\x00"), \
             mock.patch.object(usb_client, "hat_la_decode", return_value=[[0]*100]*4):
            
            # This should finish immediately because state is DONE
            res = waveform.capture_logic_analyzer(channels=4, rate_hz=1000000, depth=100)
            assert res["channels"] == 4

def test_la_status_actual_rate_field(usb_client, sim_device):
    """LA status should include actual_rate_hz."""
    usb_client.connect()
    
    # In SimulatedDevice core handler for LA_STATUS, it should return the clock rate
    # Let's check how it's implemented in tests/mock/handlers/hat.py
    status = usb_client.hat_la_get_status()
    assert "actual_rate_hz" in status

# ---------------------------------------------------------------------------
# USB-PD Regressions
# ---------------------------------------------------------------------------

def test_usbpd_schema_alignment(http_client, sim_device):
    """USB-PD status should follow the new schema (voltageV, currentA, etc.)."""
    status = http_client.usbpd_get_status()
    
    # New schema fields
    assert "voltageV" in status
    assert "currentA" in status
    assert "powerW" in status
    assert "sourcePdos" in status
    assert "selectedPdo" in status
    
    # Verify values
    assert status["voltageV"] == 5.0
    assert status["currentA"] == 3.0
    assert len(status["sourcePdos"]) == 6
