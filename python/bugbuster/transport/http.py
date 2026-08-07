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

See bbp-protocol.md and webserver.cpp for the full endpoint reference.
"""

import json
import logging
import struct
import threading
from typing import Any, Callable, Optional

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

    # WS stream_id for ADC DSP (mirrors WS_STREAM_ADC_DSP = 0x03 in ws_stream.h)
    _WS_STREAM_ADC_DSP = 0x03

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

        # WebSocket DSP stream state
        self._ws_thread:   Optional[threading.Thread] = None
        self._ws_stop      = threading.Event()
        self._ws_app       = None   # websocket.WebSocketApp instance
        self._host         = host
        self._port         = port

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
        self.stop_dsp_ws_stream()
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
    # ADC DSP WebSocket stream (HTTP transport only)
    # ------------------------------------------------------------------

    def start_dsp_ws_stream(
        self,
        callback: Callable[[bytes], None],
    ) -> None:
        """
        Open the WebSocket stream and subscribe to the ``adc-dsp`` stream.

        *callback* receives raw payload bytes (the stripped DSP event payload,
        without the 4-byte WS frame header). The caller is responsible for
        parsing it via ``BugBuster._parse_adc_dsp_evt()``.

        Requires ``websocket-client`` (``pip install websocket-client``).
        """
        try:
            import websocket  # type: ignore[import-untyped]
        except ImportError as exc:
            raise ImportError(
                "websocket-client is required for ADC DSP streaming over WiFi. "
                "Install it with: pip install websocket-client"
            ) from exc

        self.stop_dsp_ws_stream()
        self._ws_stop.clear()

        ws_url = f"ws://{self._host}:{self._port}/api/ws/stream"

        def _on_open(ws):
            # Auth: send admin token (or empty string if none)
            tok = self._admin_token or ""
            ws.send(tok)
            # Subscribe to adc-dsp stream
            ws.send(json.dumps({"subscribe": ["adc-dsp"]}))
            log.debug("WS stream open, subscribed to adc-dsp")

        def _on_message(ws, msg):
            if not isinstance(msg, bytes) or len(msg) < 4:
                return
            # Frame header: [opcode:1][stream_id:1][len:2 LE]
            stream_id = msg[1]
            payload_len = struct.unpack_from('<H', msg, 2)[0]
            if stream_id == self._WS_STREAM_ADC_DSP and len(msg) >= 4 + payload_len:
                try:
                    callback(msg[4:4 + payload_len])
                except Exception:
                    log.debug("DSP callback error", exc_info=True)

        def _on_error(ws, err):
            log.debug("WS stream error: %s", err)

        def _on_close(ws, code, msg):
            log.debug("WS stream closed: %s %s", code, msg)

        app = websocket.WebSocketApp(
            ws_url,
            on_open=_on_open,
            on_message=_on_message,
            on_error=_on_error,
            on_close=_on_close,
        )
        self._ws_app = app

        def _run():
            while not self._ws_stop.is_set():
                try:
                    app.run_forever()
                except Exception:
                    pass
                if not self._ws_stop.is_set():
                    import time; time.sleep(1.0)  # reconnect delay

        self._ws_thread = threading.Thread(target=_run, daemon=True,
                                           name="bb_dsp_ws")
        self._ws_thread.start()
        log.info("DSP WebSocket stream started at %s", ws_url)

    def stop_dsp_ws_stream(self) -> None:
        """Close the DSP WebSocket stream if open."""
        self._ws_stop.set()
        if self._ws_app is not None:
            try:
                self._ws_app.close()
            except Exception:
                pass
            self._ws_app = None
        if self._ws_thread is not None:
            self._ws_thread.join(timeout=2.0)
            self._ws_thread = None

    # ------------------------------------------------------------------
    # Context-manager support
    # ------------------------------------------------------------------

    def __enter__(self) -> "HTTPTransport":
        self.connect()
        return self

    def __exit__(self, *_) -> None:
        self.disconnect()
