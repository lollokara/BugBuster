"""
Transport protocol interface for BugBuster client.

Defines the common interface that both USB and HTTP transports must satisfy.
This enables the client to be transport-agnostic while maintaining type safety.

Note: This protocol only defines methods and attributes that BOTH transports
implement. Transport-specific methods (USB-only or HTTP-only) are accessed
through runtime checks in the client code using _require_usb() or similar guards.
"""

from typing import Any, Protocol, runtime_checkable


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
