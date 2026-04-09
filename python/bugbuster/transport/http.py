"""
HTTP REST Transport for the BugBuster device.

Wraps the device's HTTP/WiFi API with a thin requests-based client.
Every endpoint that the BugBuster exposes is reachable here.

The high-level BugBuster client calls into this transport using the same
method names as the USB transport, so the rest of the code is transport-agnostic.

See BugBusterProtocol.md and webserver.cpp for the full endpoint reference.
"""

import logging
from typing import Any, Optional

import requests

log = logging.getLogger(__name__)


class HTTPTransport:
    """
    Communicates with a BugBuster device over its WiFi HTTP REST API.

    Usage::

        with HTTPTransport("192.168.4.1") as t:
            info = t.get("/device/version")
    """

    def __init__(
        self,
        host:    str,
        port:    int   = 80,
        timeout: float = 5.0,
    ):
        """
        *host* — IP address or hostname of the device.
        *port* — HTTP port (default 80).
        """
        scheme      = "http"
        self._base  = f"{scheme}://{host}:{port}/api"
        self._timeout = timeout
        self._session = requests.Session()

        # Firmware version info filled in after connect()
        self.fw_version: Optional[tuple[int, int, int]] = None

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
        """
        url = f"{self._base}{path}"
        log.debug("POST %s body=%s", url, body)
        r = self._session.post(url, json=body or {}, headers=headers, timeout=self._timeout)
        r.raise_for_status()
        try:
            return r.json()
        except ValueError:
            log.warning("Non-JSON response from %s: %s", url, r.text[:200])
            return {}

    # ------------------------------------------------------------------
    # Context-manager support
    # ------------------------------------------------------------------

    def __enter__(self) -> "HTTPTransport":
        self.connect()
        return self

    def __exit__(self, *_) -> None:
        self.disconnect()
