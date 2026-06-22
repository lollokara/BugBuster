import struct
from unittest.mock import MagicMock

import pytest

from bugbuster import BugBuster
from bugbuster.constants import CmdId
from bugbuster.transport.usb import DeviceError
from bugbuster.transport.usb import USBTransport


def _make_usb_transport(responses):
    transport = MagicMock(spec=USBTransport)
    transport.send_command.side_effect = lambda cmd_id, payload=b"": responses.get(cmd_id, b"")
    return transport


def _hat_status_payload(*, detected: bool) -> bytes:
    return (
        bytes([1 if detected else 0])
        + bytes([1 if detected else 0])
        + bytes([0])
        + struct.pack('<f', 3.3 if detected else 0.0)
        + bytes([1, 0])
        + bytes([1 if detected else 0])
        + bytes([0, 0, 0, 0])
    )


def test_selftest_supplies_cached_parses_payload():
    payload = (
        bytes([1])
        + struct.pack("<I", 12345)
        + struct.pack("<fff", 12.0, 5.0, 3.3)
    )
    client = BugBuster(_make_usb_transport({
        CmdId.SELFTEST_SUPPLY_VOLTAGES_CACHED: payload,
    }))

    result = client.selftest_supplies_cached()

    assert result["available"] is True
    assert result["timestamp_ms"] == 12345
    assert len(result["rails"]) == 3
    assert result["rails"][0]["name"] == "VADJ1"
    assert result["rails"][2]["voltage_v"] == pytest.approx(3.3, abs=1e-6)


def test_hat_la_status_parses_optional_stream_diagnostics():
    payload = bytearray()
    payload += bytes([5, 4])                 # state=error, channels=4
    payload += struct.pack("<I", 0)          # samples_captured
    payload += struct.pack("<I", 100000)     # total_samples
    payload += struct.pack("<I", 500000)     # actual_rate_hz
    payload += bytes([1, 1])                 # usb_connected, usb_mounted
    payload += bytes([3])                    # stream_stop_reason=dma_overrun
    payload += struct.pack("<I", 2)          # overrun_count
    payload += struct.pack("<I", 1)          # short_write_count
    payload += bytes([1])                    # usb_rearm_pending
    payload += bytes([4, 3])                 # request, complete counts

    client = BugBuster(_make_usb_transport({
        CmdId.HAT_GET_STATUS: _hat_status_payload(detected=True),
        CmdId.HAT_LA_STATUS: bytes(payload),
    }))
    result = client.hat_la_get_status()

    assert result["state_name"] == "error"
    assert result["usb_connected"] is True
    assert result["usb_mounted"] is True
    assert result["stream_stop_reason"] == 3
    assert result["stream_stop_reason_name"] == "dma_overrun"
    assert result["stream_overrun_count"] == 2
    assert result["stream_short_write_count"] == 1
    assert result["usb_rearm_pending"] is True
    assert result["usb_rearm_request_count"] == 4
    assert result["usb_rearm_complete_count"] == 3
