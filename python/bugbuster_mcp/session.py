"""
BugBuster MCP — Session manager.

Maintains a singleton BugBuster client and HAL, lazily initialized on first
tool call.  Call ``configure()`` once at startup (from __main__) before any
tool is invoked.
"""

from __future__ import annotations
import logging
from typing import Optional

log = logging.getLogger(__name__)

# ---------------------------------------------------------------------------
# Session state
# ---------------------------------------------------------------------------

_transport: str = "usb"
_port:      Optional[str] = None
_host:      str = "192.168.4.1"
_vlogic:    float = 3.3   # Fixed at startup — not changeable by AI tools

_bb   = None   # bugbuster.BugBuster instance
_hal  = None   # bugbuster.BugBusterHAL instance
_init = False  # True after hal.begin() has been called


def configure(
    transport: str,
    port:      str   = None,
    host:      str   = "192.168.4.1",
    vlogic:    float = 3.3,
) -> None:
    """
    Set connection parameters.  Must be called before any tool uses
    ``get_client()`` or ``get_hal()``.

    vlogic is fixed here and cannot be changed by AI tools at runtime.
    """
    global _transport, _port, _host, _vlogic
    _transport = transport
    _port      = port
    _host      = host
    _vlogic    = vlogic
    log.info("Session configured: transport=%s port=%s host=%s vlogic=%.1fV",
             transport, port, host, vlogic)


def get_vlogic() -> float:
    """Return the VLOGIC voltage fixed at startup."""
    return _vlogic


def get_client():
    """
    Return a connected BugBuster client, connecting lazily on first call.
    """
    global _bb
    if _bb is None:
        _bb = _create_client()
    if not _bb._connected:
        _bb.connect()
    return _bb


def get_hal():
    """
    Return an initialized BugBusterHAL, running begin() lazily on first call.
    """
    global _hal, _init
    bb = get_client()
    if _hal is None:
        _hal = bb.hal
    if not _init:
        log.info("HAL begin() — VLOGIC=%.1f V (user-configured)", _vlogic)
        _hal.begin(vlogic=_vlogic)
        _init = True
    return _hal


def reset_session() -> None:
    """
    Disconnect and drop all state.  The next tool call will reconnect.
    """
    global _bb, _hal, _init
    if _bb is not None:
        try:
            if _hal is not None:
                _hal.shutdown()
        except Exception:
            pass
        try:
            _bb.disconnect()
        except Exception:
            pass
    _bb   = None
    _hal  = None
    _init = False
    log.info("Session reset.")


def is_usb() -> bool:
    """True if the current transport is USB (binary BBP)."""
    return _transport == "usb"


# ---------------------------------------------------------------------------
# Internal
# ---------------------------------------------------------------------------

def _create_client():
    if _transport == "usb":
        from bugbuster import connect_usb
        if _port is None:
            raise RuntimeError(
                "USB transport requires --port (e.g. /dev/ttyACM0 or /dev/cu.usbmodem...)"
            )
        log.info("Connecting via USB: %s", _port)
        return connect_usb(_port)
    elif _transport == "http":
        from bugbuster import connect_http
        log.info("Connecting via HTTP: %s", _host)
        return connect_http(_host)
    else:
        raise RuntimeError(f"Unknown transport: {_transport!r}. Use 'usb' or 'http'.")
