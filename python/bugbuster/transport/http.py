"""
HTTP REST Transport for the BugBuster device.

Wraps the device's HTTP/WiFi API with a thin requests-based client.
Every endpoint that the BugBuster exposes is reachable here.

The high-level BugBuster client calls into this transport using the same
method names as the USB transport, so the rest of the code is transport-agnostic.

Mutating endpoints (all POSTs) require the admin token, derived during
USB pairing and passed to the constructor via ``admin_token`` or
``set_admin_token``. The matching firmware constant is
``ADMIN_TOKEN_HEADER`` in ``Firmware/ESP32/src/config.h``.

See BugBusterProtocol.md and webserver.cpp for the full endpoint reference.
"""

import logging
from typing import Any, Optional

import requests

log = logging.getLogger(__name__)

# Must match #define ADMIN_TOKEN_HEADER in Firmware/ESP32/src/config.h
ADMIN_TOKEN_HEADER = "X-BugBuster-Admin-Token"


class HTTPTransport:
    """
    Communicates with a BugBuster device over its WiFi HTTP REST API.

    Usage::

        with HTTPTransport("192.168.4.1", admin_token=tok) as t:
            info = t.get("/device/version")
            boards = t.get_board()
    """

    def __init__(
        self,
        host:        str,
        port:        int = 80,
        timeout:     float = 5.0,
        admin_token: Optional[str] = None,
    ):
        """
        *host*        — IP address or hostname of the device.
        *port*        — HTTP port (default 80).
        *timeout*     — Per-request timeout in seconds.
        *admin_token* — 64-char hex admin token (from USB pairing). Required
                        for any mutating POST; reads work without it.
        """
        scheme        = "http"
        self._base    = f"{scheme}://{host}:{port}/api"
        self._timeout = timeout
        self._session = requests.Session()
        self._admin_token: Optional[str] = admin_token

        # Firmware version info filled in after connect()
        self.fw_version: Optional[tuple[int, int, int]] = None

    # ------------------------------------------------------------------
    # Admin token management
    # ------------------------------------------------------------------

    def set_admin_token(self, token: Optional[str]) -> None:
        """Set or clear the admin token used for mutating endpoints."""
        self._admin_token = token

    def has_admin_token(self) -> bool:
        return bool(self._admin_token)

    # ------------------------------------------------------------------
    # Connection management
    # ------------------------------------------------------------------

    def connect(self) -> dict:
        """Verify connectivity by fetching /api/device/version."""
        info = self.get("/device/version")
        self.fw_version = (info.get("fwMajor", 0), info.get("fwMinor", 0), info.get("fwPatch", 0))
        log.info(
            "Connected to BugBuster fw=%d.%d.%d via HTTP %s",
            *self.fw_version, self._base,
        )
        return info

    def disconnect(self) -> None:
        self._session.close()

    # ------------------------------------------------------------------
    # Low-level HTTP helpers
    # ------------------------------------------------------------------

    def get(self, path: str, params: Optional[dict] = None) -> Any:
        """
        HTTP GET ``/api{path}``.
        Returns parsed JSON.  Raises on HTTP errors.
        """
        url = f"{self._base}{path}"
        log.debug("GET %s params=%s", url, params)
        r = self._session.get(url, params=params, timeout=self._timeout)
        r.raise_for_status()
        return r.json()

    def post(self, path: str, body: Optional[dict] = None, headers: Optional[dict] = None) -> Any:
        """
        HTTP POST ``/api{path}`` with a JSON body.
        Returns parsed JSON (or ``{}`` for empty responses).

        Injects the admin token header automatically if one has been set.
        """
        url = f"{self._base}{path}"
        merged_headers: dict[str, str] = {}
        if self._admin_token:
            merged_headers[ADMIN_TOKEN_HEADER] = self._admin_token
        if headers:
            merged_headers.update(headers)
        log.debug("POST %s body=%s", url, body)
        r = self._session.post(
            url,
            json=body or {},
            headers=merged_headers or None,
            timeout=self._timeout,
        )
        r.raise_for_status()
        try:
            return r.json()
        except ValueError:
            log.warning("Non-JSON response from %s: %s", url, r.text[:200])
            return {}

    # ------------------------------------------------------------------
    # Pairing / device identity
    # ------------------------------------------------------------------

    def get_pairing_info(self) -> dict:
        """Return ``{macAddress, tokenFingerprint, transport}`` — safe to read
        before pairing; fingerprint lets callers confirm they hold the right
        token before trying to use it."""
        return self.get("/pairing/info")

    def verify_pairing(self, token: Optional[str] = None) -> bool:
        """POST /api/pairing/verify with *token* (or the cached one) and
        return True on 200, False on 401. Other errors propagate."""
        candidate = token if token is not None else self._admin_token
        if not candidate:
            return False
        url = f"{self._base}/pairing/verify"
        r = self._session.post(
            url,
            json={},
            headers={ADMIN_TOKEN_HEADER: candidate},
            timeout=self._timeout,
        )
        if r.status_code == 200:
            self._admin_token = candidate
            return True
        if r.status_code == 401:
            return False
        r.raise_for_status()
        return False

    def get_mac_address(self) -> Optional[str]:
        """Convenience: pull ``macAddress`` out of /api/device/info."""
        info = self.get("/device/info")
        mac = info.get("macAddress") or info.get("mac_address")
        return mac if isinstance(mac, str) else None

    # ------------------------------------------------------------------
    # Board profile
    # ------------------------------------------------------------------

    def get_board(self) -> dict:
        """Return ``{active, available: [BoardProfile]}``."""
        return self.get("/board")

    def set_board(self, board_id: str) -> dict:
        """Select a board profile. Requires admin token."""
        return self.post("/board/select", {"boardId": board_id})

    # ------------------------------------------------------------------
    # Context-manager support
    # ------------------------------------------------------------------

    def __enter__(self) -> "HTTPTransport":
        self.connect()
        return self

    def __exit__(self, *_) -> None:
        self.disconnect()
