"""
SimulatedUSBTransport and SimulatedHTTPTransport.

These bypass all real serial/network I/O and route directly to SimulatedDevice.
Neither class calls super().__init__() to avoid importing serial/requests.
"""

import threading
from bugbuster.transport.usb import USBTransport
from bugbuster.transport.http import HTTPTransport


class SimulatedUSBTransport(USBTransport):
    """USB transport backed by a SimulatedDevice — no serial port required."""

    def __init__(self, device, hat: bool = False):
        # Do NOT call super().__init__() — it imports serial and opens ports.
        self._timeout = 5.0
        self._event_handlers = {}
        self._pending = {}
        self._pending_lock = threading.Lock()
        self._seq = 0
        self._seq_lock = threading.Lock()

        self._device = device
        device._transport = self
        device.hat_present = hat

        # Attributes that client.py reads off the transport
        self.proto_version = device.PROTO_VERSION
        self.fw_version = device.fw_version   # (major, minor, patch)
        # Mirror the 14-byte BBP v4 handshake MAC so get_mac_address() works
        # against the simulator. Use a stable dummy MAC unless the device
        # overrides it.
        mac_str = getattr(device, "mac_address", "aa:bb:cc:00:11:22")
        self.mac = bytes(int(b, 16) for b in mac_str.split(":"))

    # ------------------------------------------------------------------
    # Connection lifecycle
    # ------------------------------------------------------------------

    def connect(self):
        """No-op for simulated transport."""
        return self.proto_version, self.fw_version

    def disconnect(self):
        """Stop any active streams."""
        self._device.stop_all_streams()

    def __enter__(self):
        return self

    def __exit__(self, *_):
        self.disconnect()

    # ------------------------------------------------------------------
    # Command execution
    # ------------------------------------------------------------------

    def send_command(self, cmd_id: int, payload: bytes = b'') -> bytes:
        """Dispatch directly to SimulatedDevice; propagate DeviceError."""
        return self._device.dispatch(cmd_id, payload)

    # ------------------------------------------------------------------
    # Event subscription
    # ------------------------------------------------------------------

    def on_event(self, evt_id: int, handler) -> None:
        self._event_handlers[int(evt_id)] = handler

    def remove_event(self, evt_id: int) -> None:
        self._event_handlers.pop(int(evt_id), None)

    def _fire_event(self, evt_id: int, payload: bytes) -> None:
        """Called by streaming handlers to deliver EVT frames."""
        handler = self._event_handlers.get(int(evt_id))
        if handler:
            try:
                handler(payload)
            except Exception:
                pass


class SimulatedHTTPTransport(HTTPTransport):
    """HTTP transport backed by a SimulatedDevice — no network required."""

    def __init__(self, device, hat: bool = False):
        # Do NOT call super().__init__() — it imports requests.
        self._base_url = "sim://device"
        self._device = device
        device._transport = self
        device.hat_present = hat

        self.fw_version = device.fw_version

    # ------------------------------------------------------------------
    # Connection lifecycle
    # ------------------------------------------------------------------

    def connect(self):
        return {"fwMajor": self._device.fw_version[0],
                "fwMinor": self._device.fw_version[1],
                "fwPatch": self._device.fw_version[2]}

    def disconnect(self):
        self._device.stop_all_streams()

    def __enter__(self):
        return self

    def __exit__(self, *_):
        self.disconnect()

    # ------------------------------------------------------------------
    # HTTP methods
    # ------------------------------------------------------------------

    def _auth_headers(self, headers) -> dict:
        """Inject the device's admin token if the caller didn't already set it.

        The real firmware enforces an admin token on all mutating routes
        (webserver.cpp:check_admin_auth).  The Python HTTPTransport does not
        implement the pairing flow (desktop does it via Tauri), so the
        simulator grants access by trusting the in-process token directly.
        """
        token = getattr(self._device, "admin_token", None)
        if token is None:
            return headers or {}
        merged = dict(headers or {})
        merged.setdefault("X-BugBuster-Admin-Token", token)
        return merged

    def get(self, path: str, params=None, headers=None) -> dict:
        return self._device.http_dispatch(
            "GET", path, params or {}, {}, self._auth_headers(headers),
        )

    def post(self, path: str, body=None, headers=None) -> dict:
        return self._device.http_dispatch(
            "POST", path, {}, body or {}, self._auth_headers(headers),
        )
