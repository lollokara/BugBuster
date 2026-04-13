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
