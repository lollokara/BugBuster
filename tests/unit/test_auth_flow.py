import pytest
from unittest.mock import MagicMock, patch
from bugbuster.client import BugBuster
from bugbuster.constants import CmdId
from bugbuster.transport.usb import USBTransport
from bugbuster.transport.http import HTTPTransport

def test_token_fetch_on_connect():
    """Verify that connect() fetches the token if using USB."""
    mock_usb = MagicMock(spec=USBTransport)
    # Mock GET_ADMIN_TOKEN response: [len, 't', 'o', 'k', 'e', 'n']
    mock_usb.send_command.return_value = b'\x05token'
    
    bb = BugBuster(mock_usb)
    bb.connect()
    
    assert bb._admin_token == "token"
    mock_usb.send_command.assert_any_call(CmdId.GET_ADMIN_TOKEN, b'')

def test_token_injection_in_http_post():
    """Verify that HTTP POST includes the X-BugBuster-Admin-Token header."""
    mock_http = MagicMock(spec=HTTPTransport)
    bb = BugBuster(mock_http)
    
    # Manually set token (as if fetched via USB earlier)
    bb._admin_token = "secure_token_123"
    
    bb._http_post("/test/path", {"data": 1})
    
    # Check that post was called with the correct header
    args, kwargs = mock_http.post.call_args
    assert args[0] == "/test/path"
    assert args[1] == {"data": 1}
    assert kwargs["headers"] == {"X-BugBuster-Admin-Token": "secure_token_123"}

def test_http_only_no_token():
    """Verify that an HTTP-only client (no USB) has no token by default."""
    mock_http = MagicMock(spec=HTTPTransport)
    bb = BugBuster(mock_http)
    bb.connect()
    
    assert bb._admin_token is None
    
    bb._http_post("/test/path", {"data": 1})
    args, kwargs = mock_http.post.call_args
    assert kwargs["headers"] == {}

def test_get_admin_token_caching():
    """Verify that the token is only fetched once and cached."""
    mock_usb = MagicMock(spec=USBTransport)
    mock_usb.send_command.return_value = b'\x04abcd'
    
    bb = BugBuster(mock_usb)
    t1 = bb.get_admin_token()
    t2 = bb.get_admin_token()
    
    assert t1 == "abcd"
    assert t2 == "abcd"
    # Should only call USB once
    assert mock_usb.send_command.call_count == 1
