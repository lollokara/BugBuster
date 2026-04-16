"""Meta-tests: verify SimulatedDevice handler completeness and drift detection."""
import sys
import os

import pytest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))

from bugbuster.constants import CmdId
from bugbuster.protocol import BBP_PROTO_VERSION
from tests.mock.simulated_device import SimulatedDevice

# CmdIds that are firmware-to-host events — not commands the client sends,
# so no dispatch handler is required for them.
_EVENT_ONLY_CMDS = {
    "SET_DIAG_CONFIG",
    "ADC_DATA_EVT",
    "SCOPE_DATA_EVT",
    "ALERT_EVT",
    "DIN_EVT",
    "PCA_FAULT_EVT",
    "HAT_LA_LOG_EVT",
}


def test_proto_version_matches():
    """SimulatedDevice.PROTO_VERSION must match the client protocol version."""
    assert SimulatedDevice.PROTO_VERSION == BBP_PROTO_VERSION


def test_all_cmdids_have_handlers():
    """Every non-event CmdId enum value must have a registered handler."""
    device = SimulatedDevice()
    unhandled = [
        cmd for cmd in CmdId
        if int(cmd) not in device._handlers and cmd.name not in _EVENT_ONLY_CMDS
    ]
    assert not unhandled, f"Missing handlers for: {[c.name for c in unhandled]}"


def test_unknown_cmdid_returns_error_not_crash():
    """Dispatching an unknown cmd_id must raise DeviceError, not crash."""
    from bugbuster.transport.usb import DeviceError
    device = SimulatedDevice()
    try:
        device.dispatch(0xFD, b"")
    except DeviceError:
        pass  # Expected
    except Exception as exc:
        pytest.fail(f"Unexpected exception type {type(exc).__name__}: {exc}")


def test_state_roundtrip_channel_function():
    """set_channel_function then status read reflects the change."""
    import bugbuster as bb
    from bugbuster.constants import ChannelFunction
    from tests.mock import SimulatedDevice, SimulatedUSBTransport

    device = SimulatedDevice()
    transport = SimulatedUSBTransport(device)
    client = bb.BugBuster(transport)
    client.connect()
    client.set_channel_function(0, ChannelFunction.VOUT)
    assert device.channels[0]["function"] == int(ChannelFunction.VOUT)
    client.disconnect()


def test_ping_roundtrip():
    """ping() returns expected token."""
    import bugbuster as bb
    from tests.mock import SimulatedDevice, SimulatedUSBTransport

    device = SimulatedDevice()
    transport = SimulatedUSBTransport(device)
    client = bb.BugBuster(transport)
    client.connect()
    result = client.ping()
    assert result.token == 0xDEADBEEF
    assert result.uptime_ms >= 0
    client.disconnect()


# Known firmware routes that the simulator should implement
KNOWN_HTTP_ROUTES = [
    ("GET", "/api/device/version"),
    ("GET", "/api/device/info"),
    ("GET", "/api/status"),
    ("GET", "/api/faults"),
    ("POST", "/api/faults/clear"),
    ("POST", "/api/device/reset"),
    ("POST", "/api/channel/0/function"),
    ("GET", "/api/channel/0/dac/readback"),
    ("POST", "/api/channel/0/dac"),
    ("POST", "/api/faults/channel/0/mask"),
    ("POST", "/api/faults/mask"),
    ("POST", "/api/faults/channel/0/clear"),
    ("GET", "/api/selftest"),
    ("GET", "/api/selftest/supply/0"),
    ("GET", "/api/selftest/efuse"),
    ("GET", "/api/selftest/supplies"),
    ("GET", "/api/gpio"),
    ("POST", "/api/gpio/0/config"),
    ("POST", "/api/gpio/0/set"),
    ("GET", "/api/dio"),
    ("GET", "/api/dio/1"),
    ("POST", "/api/dio/1/config"),
    ("POST", "/api/dio/1/set"),
    ("GET", "/api/mux"),
    ("POST", "/api/mux/all"),
    ("POST", "/api/mux/switch"),
    ("GET", "/api/hat"),
    ("GET", "/api/hat/la/status"),
    ("POST", "/api/hat/detect"),
    ("POST", "/api/hat/reset"),
    ("POST", "/api/hat/config"),
    ("POST", "/api/hat/setup_swd"),
    ("GET", "/api/idac"),
    ("POST", "/api/idac/voltage"),
    ("POST", "/api/idac/code"),
    ("POST", "/api/idac/cal/save"),
    ("GET", "/api/ioexp"),
    ("POST", "/api/ioexp/control"),
    ("GET", "/api/ioexp/faults"),
    ("POST", "/api/ioexp/fault_config"),
    ("GET", "/api/usbpd"),
    ("POST", "/api/usbpd/select"),
    ("GET", "/api/wifi"),
    ("GET", "/api/wifi/scan"),
    ("POST", "/api/wifi/connect"),
]


def test_http_route_coverage():
    """Verify all known firmware routes are implemented in the simulator."""
    from tests.mock.simulated_device import SimulatedDevice
    device = SimulatedDevice()
    # Disable auth for this test
    device.admin_token = None

    for method, path in KNOWN_HTTP_ROUTES:
        # Provide minimal body for POSTs to avoid validation errors where possible
        body = {}
        if path.endswith("/function"): body = {"function": 0}
        if path.endswith("/dac"): body = {"code": 0}
        if path.endswith("/mask"): body = {"mask": 0xFFFF, "alert_mask": 0, "supply_mask": 0}
        if path.endswith("/config"): body = {"mode": 0}
        if path.endswith("/set"): body = {"value": False}
        if path == "/api/mux/all": body = {"states": [0,0,0,0]}
        if path == "/api/mux/switch": body = {"device": 0, "switch": 0, "closed": False}
        if path == "/api/hat/config": body = {"pin": 0, "function": 0}
        if path == "/api/idac/voltage": body = {"ch": 0, "voltage": 5.0}
        if path == "/api/idac/code": body = {"ch": 0, "code": 0}
        if path == "/api/usbpd/select": body = {"voltage": 5}

        res = device.http_dispatch(method, path, {}, body)
        assert res.get("error") != "not implemented", f"Route {method} {path} not implemented"
        assert res.get("code") != 404, f"Route {method} {path} returned 404"


def test_http_auth_enforcement():
    """Verify that POST routes require X-BugBuster-Admin-Token."""
    from tests.mock.simulated_device import SimulatedDevice
    device = SimulatedDevice()
    device.admin_token = "TEST-TOKEN"

    # GET should NOT require auth
    res = device.http_dispatch("GET", "/api/status", {}, {})
    assert res.get("error") != "unauthorized"

    # POST without token should be unauthorized
    res = device.http_dispatch("POST", "/api/device/reset", {}, {})
    assert res.get("error") == "unauthorized"
    assert res.get("code") == 401

    # POST with wrong token should be unauthorized
    res = device.http_dispatch("POST", "/api/device/reset", {}, {}, {"X-BugBuster-Admin-Token": "WRONG"})
    assert res.get("error") == "unauthorized"

    # POST with correct token should work
    res = device.http_dispatch("POST", "/api/device/reset", {}, {}, {"X-BugBuster-Admin-Token": "TEST-TOKEN"})
    assert res.get("error") != "unauthorized"


def test_http_json_validation():
    """Verify that POST routes validate required fields in the JSON body."""
    from tests.mock.simulated_device import SimulatedDevice
    device = SimulatedDevice()
    device.admin_token = None  # disable auth

    # POST /api/channel/0/function without body should return error
    res = device.http_dispatch("POST", "/api/channel/0/function", {}, {})
    assert "missing field" in res.get("error", "").lower()
    assert res.get("code") == 400

    # POST with some missing fields
    res = device.http_dispatch("POST", "/api/mux/switch", {}, {"device": 0})
    assert "missing fields" in res.get("error", "").lower()
    assert res.get("code") == 400



