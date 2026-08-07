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

_transport: str = "auto"
_port:      Optional[str] = None
_host:      str = "192.168.4.1"
_vlogic:    float = 3.3   # Fixed at startup — not changeable by AI tools
_admin_token: Optional[str] = None

_bb   = None   # bugbuster.BugBuster instance
_hal  = None   # bugbuster.BugBusterHAL instance
_init = False  # True after hal.begin() has been called

_active_board: Optional[str] = None  # Path or name of the active board profile

def configure(
    transport:    str,
    port:         Optional[str] = None,
    host:         str   = "192.168.4.1",
    vlogic:       float = 3.3,
    admin_token:  Optional[str] = None,
) -> None:
    """
    Set connection parameters.  Must be called before any tool uses
    ``get_client()`` or ``get_hal()``.

    vlogic is fixed here and cannot be changed by AI tools at runtime.
    """
    global _transport, _port, _host, _vlogic, _admin_token
    _transport   = transport
    _port        = port
    _host        = host
    _vlogic      = vlogic
    _admin_token = admin_token
    log.info("Session configured: transport=%s port=%s host=%s vlogic=%.1fV admin_token=%s",
             transport, port, host, vlogic, "set" if admin_token else "unset")


def get_vlogic() -> float:
    """Return the VLOGIC voltage fixed at startup."""
    return _vlogic


def get_transport() -> str:
    """Configured transport, resolved to 'usb'/'http' once a client is made."""
    return _transport


def get_port() -> Optional[str]:
    """Resolved USB port, or None before the first connection (or on HTTP)."""
    return _port


def _transport_obj():
    """The live transport object on the client (``BugBuster._t``)."""
    if _bb is None:
        return None
    return getattr(_bb, "_t", None)


def link_healthy() -> Optional[bool]:
    """True/False for a USB link, None when not applicable or not yet opened."""
    if _bb is None or _transport != "usb":
        return None
    checker = getattr(_transport_obj(), "is_healthy", None)
    return bool(checker()) if callable(checker) else None


def reconnect() -> dict:
    """Force the transport down and back up, keeping the same client object.

    The USB reader thread can die on a device re-enumeration (a P4 reset during
    OTA does it); writes then keep succeeding while nothing reads the replies,
    so every command times out until the link is rebuilt.
    """
    global _bb, _hal, _init
    before = link_healthy()
    if _bb is not None and _transport == "usb":
        reconnector = getattr(_transport_obj(), "reconnect", None)
        if callable(reconnector):
            reconnector()
            log.info("USB transport reconnected")
            return {"method": "transport_reconnect",
                    "healthy_before": before,
                    "healthy_after": link_healthy()}
    # No live transport to repair - drop everything and reconnect lazily.
    reset_session()
    get_client()
    return {"method": "session_reset",
            "healthy_before": before,
            "healthy_after": link_healthy()}


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
    global _bb, _hal, _init, _active_board
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
    _active_board = None
    log.info("Session reset.")


def set_active_board(name: str) -> None:
    """Set the name of the active board profile."""
    global _active_board
    _active_board = name


def get_active_board_profile() -> Optional[dict]:
    """
    Return the parsed active board profile, or None if none set.
    """
    if _active_board is None:
        return None
    
    import os
    import json
    
    # Locate profile file (expecting .json in board_profiles/)
    profile_dir = os.path.join(os.path.dirname(__file__), "board_profiles")
    profile_path = os.path.join(profile_dir, f"{_active_board}.json")
    
    if not os.path.exists(profile_path):
        log.warning("Board profile not found: %s", profile_path)
        return None
        
    try:
        with open(profile_path, "r") as f:
            return json.load(f)
    except Exception as e:
        log.error("Failed to load board profile %s: %s", profile_path, e)
        return None


def is_usb() -> bool:
    """True if the current transport is USB (binary BBP)."""
    return _transport == "usb"


# ---------------------------------------------------------------------------
# Internal
# ---------------------------------------------------------------------------

def _resolve_usb_port() -> str:
    """Find the mainboard's BBP port, or raise with what was actually seen."""
    from bugbuster.discovery import find_usb_port, list_usb_ports

    port = find_usb_port()
    if port:
        log.info("Auto-detected BugBuster on %s", port)
        return port

    seen = list_usb_ports(all_ports=True)
    if not seen:
        detail = "no serial ports are present on this host"
    else:
        detail = "ports seen: " + ", ".join(
            f"{p.device} ({p.vid:04X}:{p.pid:04X})" if p.vid else f"{p.device} (non-USB)"
            for p in seen)
    raise RuntimeError(
        f"No BugBuster found on USB - {detail}. Check the cable, close any "
        f"other app holding the port (CDC0 is single-client), or start the "
        f"server with an explicit --port."
    )


def _create_client():
    global _transport, _port

    if _transport == "auto":
        from bugbuster.discovery import find_usb_port
        found = find_usb_port()
        if found:
            _transport, _port = "usb", found
            log.info("Transport auto-selected: USB on %s", found)
        else:
            _transport = "http"
            log.info("No USB board found; falling back to HTTP at %s", _host)

    if _transport == "usb":
        from bugbuster import connect_usb
        if _port in (None, "", "auto"):
            _port = _resolve_usb_port()
        log.info("Connecting via USB: %s", _port)
        return connect_usb(_port)
    elif _transport == "http":
        from bugbuster import connect_http
        log.info("Connecting via HTTP: %s", _host)
        return connect_http(_host, admin_token=_admin_token)
    else:
        raise RuntimeError(
            f"Unknown transport: {_transport!r}. Use 'auto', 'usb' or 'http'.")
