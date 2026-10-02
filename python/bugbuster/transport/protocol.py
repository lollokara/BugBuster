"""
Transport protocol interface for BugBuster client.

Defines the common interface that both USB and HTTP transports must satisfy.
This enables the client to be transport-agnostic while maintaining type safety.

Note: This protocol only defines methods and attributes that BOTH transports
implement. Transport-specific methods (USB-only or HTTP-only) are accessed
through runtime checks in the client code using _require_usb() or similar guards.
"""

from typing import Any, Callable, Optional, Protocol, runtime_checkable


@runtime_checkable
class Transport(Protocol):
    """
    Protocol defining the interface that all BugBuster transports must implement.

    The :class:`BugBusterClient` uses this protocol to type its transport
    attribute, enabling it to work with either :class:`USBTransport` or
    :class:`HTTPTransport` without explicit type checking or unions.

    This protocol only includes the shared interface. Methods that are
    USB-only (like send_command, on_event) or HTTP-only (like get, post,
    delete, WebSocket streams) are not part of the protocol and should be
    guarded by runtime type checks in the client code.
    """

    # ---- Connection lifecycle ----

    def connect(self) -> Any:
        """
        Establish connection to the device.

        Returns connection metadata (type varies by transport).
        For USB: ``(proto_version, fw_version_tuple)``
        For HTTP: ``dict`` with version info
        """
        ...

    def disconnect(self) -> None:
        """Close the connection and release resources."""
        ...

    def __enter__(self) -> Any:
        """Context manager entry. Returns self or compatible transport."""
        ...

    def __exit__(self, *args: Any) -> None:
        """Context manager exit."""
        ...

    # ---- Shared attributes ----

    fw_version: Any
    """Firmware version tuple (major, minor, patch), or None if not connected."""

    _timeout: float
    """Default timeout in seconds for operations."""


@runtime_checkable
class UsbTransportProtocol(Transport, Protocol):
    """USB (CDC0 BBP) members the client uses behind ``_require_usb`` (FEAT-4)."""

    _port: str
    _serial: Any

    def send_command(self, cmd_id: int, payload: bytes = b"",
                     timeout: Optional[float] = None) -> bytes:
        """Send a BBP command and return the response payload."""
        ...

    def on_event(self, evt_id: int, callback: Callable[[bytes], None]) -> None:
        """Register a callback for an unsolicited event."""
        ...

    def remove_event(self, evt_id: int) -> None:
        """Remove the callback for ``evt_id``."""
        ...


@runtime_checkable
class HttpTransportProtocol(Transport, Protocol):
    """HTTP REST members the client uses on the HTTP path (FEAT-4)."""

    def get(self, path: str, params: Optional[dict] = None,
            headers: Optional[dict] = None) -> Any:
        ...

    def post(self, path: str, body: Optional[dict | bytes | str] = None,
             headers: Optional[dict] = None) -> Any:
        ...

    def delete(self, path: str, params: Optional[dict] = None,
               headers: Optional[dict] = None) -> Any:
        ...

    def start_dsp_ws_stream(self, callback: Callable[[bytes], None]) -> None:
        ...

    def stop_dsp_ws_stream(self) -> None:
        ...
