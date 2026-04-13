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
    # Enable HAT simulation
    transport = SimulatedHTTPTransport(sim_device, hat=True)
    return BugBuster(transport)

@pytest.fixture
def usb_client(sim_device):
    # Enable HAT simulation
    transport = SimulatedUSBTransport(sim_device, hat=True)
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

def test_dio_hal_read_uses_correct_io_index_analog(usb_client, sim_device):
    """hal.read_digital(io=1) should use AD74416H DIN when in DIGITAL_IN mode."""
    usb_client.connect()
    hal = usb_client.hal
    hal.begin()
    
    # Configure IO 1 as digital input (routed to AD74416H ch 0)
    from bugbuster.hal import PortMode
    hal.configure(1, PortMode.DIGITAL_IN)
    
    # Set simulated DIN state for channel 0
    sim_device.channels[0]["din_state"] = True
    
    val = hal.read_digital(1)
    assert val is True
    
    sim_device.channels[0]["din_state"] = False
    assert hal.read_digital(1) is False

def test_dio_hal_read_uses_correct_io_index_digital_only(usb_client, sim_device):
    """hal.read_digital(io=2) should call dio_read(2)."""
    usb_client.connect()
    hal = usb_client.hal
    hal.begin()
    
    # Configure IO 2 as digital input (routed to ESP GPIO via MUX)
    from bugbuster.hal import PortMode
    hal.configure(2, PortMode.DIGITAL_IN)
    
    # Set simulated GPIO state for IO 2 (index 1 in sim_device.dio)
    sim_device.dio[1]["output"] = True 
    
    val = hal.read_digital(2)
    assert val is True
    
    sim_device.dio[1]["output"] = False
    assert hal.read_digital(2) is False

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
    from unittest import mock
    
    with mock.patch("bugbuster_mcp.session.get_client", return_value=usb_client), \
         mock.patch("bugbuster_mcp.session.get_hal", return_value=usb_client.hal):
        
        # We need to mock hat_la_read_all and decode because we haven't set up actual data
        with mock.patch.object(usb_client, "hat_la_read_all", return_value=b"\x00\x00\x00\x00"), \
             mock.patch.object(usb_client, "hat_la_decode", return_value=[[0]*100]*4):
            
            # This should finish immediately because state is DONE
            res = waveform.capture_logic_analyzer(channels=4, rate_hz=1000000, depth=100)
            assert res["channels"] == 4

def test_la_status_http_phase0_mapping(http_client, sim_device):
    """LA status over HTTP should correctly map Phase 0 fields."""
    http_client.connect()
    sim_device.la_state = "DONE"
    sim_device.la_config["sample_rate"] = 25000000
    
    status = http_client.hat_la_get_status()
    assert status["state_name"] == "done"
    assert status["state"] == 3
    assert status["actual_rate_hz"] == 25000000

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
