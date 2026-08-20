"""
Unit tests for Transport protocol conformance and script operations.

These tests verify:
1. Both USBTransport and HTTPTransport satisfy the Transport protocol
2. script_delete() works over HTTP (Fix 1)
"""

from unittest.mock import MagicMock

import pytest

from bugbuster import BugBuster
from bugbuster.transport.protocol import Transport
from bugbuster.transport.usb import USBTransport
from bugbuster.transport.http import HTTPTransport, ADMIN_TOKEN_HEADER


# ---------------------------------------------------------------------------
# Transport protocol conformance (Fix 3)
# ---------------------------------------------------------------------------


def test_usb_transport_satisfies_protocol():
    """Verify USBTransport conforms to the Transport protocol structurally."""
    # This is a static check - if it type-checks, the protocol is satisfied
    transport: Transport = USBTransport("/dev/null")
    assert hasattr(transport, "connect")
    assert hasattr(transport, "disconnect")
    assert hasattr(transport, "send_command")
    assert hasattr(transport, "on_event")
    assert hasattr(transport, "remove_event")
    assert hasattr(transport, "_timeout")
    assert hasattr(transport, "fw_version")
    assert hasattr(transport, "_port")
    assert hasattr(transport, "_serial")


def test_http_transport_satisfies_protocol():
    """Verify HTTPTransport conforms to the Transport protocol structurally."""
    # This is a static check - if it type-checks, the protocol is satisfied
    transport: Transport = HTTPTransport("192.168.1.1")
    assert hasattr(transport, "connect")
    assert hasattr(transport, "disconnect")
    assert hasattr(transport, "get")
    assert hasattr(transport, "post")
    assert hasattr(transport, "delete")
    assert hasattr(transport, "start_dsp_ws_stream")
    assert hasattr(transport, "stop_dsp_ws_stream")
    assert hasattr(transport, "_timeout")
    assert hasattr(transport, "fw_version")


def test_both_transports_have_shared_protocol_methods():
    """Verify both transports implement the shared protocol methods."""
    usb = USBTransport("/dev/null")
    http = HTTPTransport("192.168.1.1")

    # Shared lifecycle methods
    for t in [usb, http]:
        assert callable(t.connect)
        assert callable(t.disconnect)
        assert callable(t.__enter__)
        assert callable(t.__exit__)

    # Shared attributes
    for t in [usb, http]:
        assert hasattr(t, "fw_version")
        assert hasattr(t, "_timeout")


# ---------------------------------------------------------------------------
# script_delete() over HTTP (Fix 1)
# ---------------------------------------------------------------------------


def _mock_response(status: int = 200, payload=None):
    """Helper to create a mock requests.Response."""
    r = MagicMock()
    r.status_code = status
    r.json.return_value = payload if payload is not None else {}

    def raise_for_status():
        if status >= 400:
            raise RuntimeError(f"HTTP {status}")

    r.raise_for_status.side_effect = raise_for_status
    return r


def test_script_delete_http_calls_delete_method():
    """Verify script_delete() calls HTTPTransport.delete() with correct path."""
    http_transport = HTTPTransport("192.168.1.42", admin_token="a" * 64)
    http_transport._session = MagicMock()
    http_transport._session.delete.return_value = _mock_response(200, {"ok": True})

    client = BugBuster(http_transport)
    client._usb = False
    client._admin_token = "a" * 64

    # This should not raise AttributeError
    client.script_delete("test.py")

    # Verify delete() was called with the correct path
    http_transport._session.delete.assert_called_once()
    args, kwargs = http_transport._session.delete.call_args
    url = args[0]
    assert "/scripts/files" in url
    assert "name=test.py" in url

    # Verify admin token was injected
    headers = kwargs.get("headers") or {}
    assert headers[ADMIN_TOKEN_HEADER] == "a" * 64


def test_script_delete_http_raises_on_error():
    """Verify script_delete() raises RuntimeError when delete fails."""
    http_transport = HTTPTransport("192.168.1.42", admin_token="a" * 64)
    http_transport._session = MagicMock()
    http_transport._session.delete.return_value = _mock_response(
        200, {"ok": False, "err": "File not found"}
    )

    client = BugBuster(http_transport)
    client._usb = False
    client._admin_token = "a" * 64

    with pytest.raises(RuntimeError, match="script_delete failed: File not found"):
        client.script_delete("nonexistent.py")


def test_script_delete_http_without_AttributeError():
    """
    Regression test: script_delete() over HTTP should NOT raise AttributeError.

    This was the bug - HTTPTransport had no delete() method, so the call
    raised AttributeError. After the fix, it should work.
    """
    http_transport = HTTPTransport("192.168.1.42", admin_token="a" * 64)
    http_transport._session = MagicMock()
    http_transport._session.delete.return_value = _mock_response(200, {"ok": True})

    client = BugBuster(http_transport)
    client._usb = False
    client._admin_token = "a" * 64

    # This was raising AttributeError before the fix
    try:
        client.script_delete("test.py")
    except AttributeError as e:
        pytest.fail(f"script_delete() raised AttributeError: {e}")
