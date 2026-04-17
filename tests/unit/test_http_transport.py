"""
Unit tests for HTTPTransport admin-token threading and board endpoints.

These mock requests.Session so no real device or network is required.
"""

from unittest.mock import MagicMock, patch

import pytest

from bugbuster.transport.http import ADMIN_TOKEN_HEADER, HTTPTransport


# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------


def _mock_response(status: int = 200, payload=None):
    r = MagicMock()
    r.status_code = status
    r.json.return_value = payload if payload is not None else {}

    def raise_for_status():
        if status >= 400:
            raise RuntimeError(f"HTTP {status}")

    r.raise_for_status.side_effect = raise_for_status
    return r


@pytest.fixture
def transport():
    t = HTTPTransport("192.168.1.42", admin_token="a" * 64)
    # Replace the requests.Session with a MagicMock.
    t._session = MagicMock()
    return t


# ---------------------------------------------------------------------------
# Admin token threading
# ---------------------------------------------------------------------------


def test_post_injects_admin_header(transport):
    transport._session.post.return_value = _mock_response(200, {"ok": True})

    transport.post("/channel/0/dac", {"code": 32768})

    args, kwargs = transport._session.post.call_args
    headers = kwargs.get("headers") or {}
    assert headers[ADMIN_TOKEN_HEADER] == "a" * 64
    assert kwargs["json"] == {"code": 32768}


def test_post_without_admin_token_omits_header():
    t = HTTPTransport("192.168.1.42")
    t._session = MagicMock()
    t._session.post.return_value = _mock_response(200, {})

    t.post("/channel/0/dac", {"code": 0})

    _, kwargs = t._session.post.call_args
    headers = kwargs.get("headers")
    # Either None or empty/missing ADMIN_TOKEN_HEADER
    if headers:
        assert ADMIN_TOKEN_HEADER not in headers


def test_set_admin_token_roundtrip():
    t = HTTPTransport("10.0.0.1")
    assert not t.has_admin_token()
    t.set_admin_token("deadbeef" * 8)
    assert t.has_admin_token()
    t.set_admin_token(None)
    assert not t.has_admin_token()


# ---------------------------------------------------------------------------
# Pairing verify
# ---------------------------------------------------------------------------


def test_verify_pairing_success(transport):
    transport._session.post.return_value = _mock_response(200, {"ok": True})

    ok = transport.verify_pairing("b" * 64)

    assert ok is True
    # Cached the freshly-verified token.
    assert transport._admin_token == "b" * 64


def test_verify_pairing_failure():
    t = HTTPTransport("10.0.0.1")
    t._session = MagicMock()
    t._session.post.return_value = _mock_response(401, {"error": "Unauthorized"})

    ok = t.verify_pairing("wrong")

    assert ok is False
    assert t._admin_token is None


def test_verify_pairing_without_candidate_returns_false():
    t = HTTPTransport("10.0.0.1")  # no token
    t._session = MagicMock()

    assert t.verify_pairing() is False
    # Should not even have made a request.
    t._session.post.assert_not_called()


# ---------------------------------------------------------------------------
# Board endpoints
# ---------------------------------------------------------------------------


def test_get_board_returns_available_list(transport):
    transport._session.get.return_value = _mock_response(
        200,
        {
            "active": "new-board",
            "available": [
                {"id": "new-board", "name": "New Board"},
                {"id": "stm32f4-discovery", "name": "STM32F4 Discovery"},
            ],
        },
    )

    state = transport.get_board()

    assert state["active"] == "new-board"
    assert len(state["available"]) == 2
    transport._session.get.assert_called_once()
    url = transport._session.get.call_args[0][0]
    assert url.endswith("/api/board")


def test_set_board_posts_boardId_and_sends_admin_header(transport):
    transport._session.post.return_value = _mock_response(200, {"ok": True, "active": "stm32f4-discovery"})

    resp = transport.set_board("stm32f4-discovery")

    assert resp["active"] == "stm32f4-discovery"
    _, kwargs = transport._session.post.call_args
    assert kwargs["json"] == {"boardId": "stm32f4-discovery"}
    assert kwargs["headers"][ADMIN_TOKEN_HEADER] == "a" * 64


# ---------------------------------------------------------------------------
# MAC extraction helper
# ---------------------------------------------------------------------------


def test_get_mac_address_prefers_camelCase(transport):
    transport._session.get.return_value = _mock_response(
        200, {"macAddress": "aa:bb:cc:dd:ee:ff", "mac_address": "00:00:00:00:00:00"}
    )

    assert transport.get_mac_address() == "aa:bb:cc:dd:ee:ff"


def test_get_mac_address_falls_back_to_snake_case(transport):
    transport._session.get.return_value = _mock_response(
        200, {"mac_address": "11:22:33:44:55:66"}
    )

    assert transport.get_mac_address() == "11:22:33:44:55:66"


def test_get_mac_address_returns_none_when_missing(transport):
    transport._session.get.return_value = _mock_response(200, {"siliconRev": 1})

    assert transport.get_mac_address() is None
