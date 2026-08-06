"""
BugBuster MCP — IO ownership tools.

Tools: io_claim, io_release, io_owner_status, io_force_release

Server-side lease tracking:
  _active_leases: dict[str, list[int]]  — handle → slot list

Handles are uuid4 hex strings.  On server shutdown all active leases are
released automatically via atexit so device slots are never stranded.

Coordination contract (parallel agent):
  The Python lib (python/bugbuster/) exposes:
    client.io_claim(slots, lease_seconds, purpose)  → None   [acquires]
    client.io_release(slots)                         → None   [releases]
    client.io_owner_status()                         → list[dict] (16 entries)
  These methods are landed by the bugbuster-lib agent in the same PR wave.
  MCP tests mock them; runtime calls are lazy (no hard import at module level).
"""

from __future__ import annotations

import atexit
import logging
import threading
import uuid
from typing import Optional

from .. import session

log = logging.getLogger(__name__)

# ---------------------------------------------------------------------------
# Server-side lease store
# ---------------------------------------------------------------------------

_active_leases: dict[str, list[int]] = {}
_leases_lock = threading.Lock()


def _verify_lease_covers(lease_handle: str, required_slots: list[int]) -> None:
    """Verify that *lease_handle* is known and covers every slot in *required_slots*.

    Raises:
        ValueError: If the handle is unknown or does not cover a required slot.
    """
    with _leases_lock:
        slots = _active_leases.get(lease_handle)
    if slots is None:
        raise ValueError("Unknown lease handle")
    slot_set = set(slots)
    for slot in required_slots:
        if slot not in slot_set:
            raise ValueError(f"Lease {lease_handle} does not cover slot {slot}")


def _release_all_leases() -> None:
    """Called at server shutdown — release every active lease on the device."""
    with _leases_lock:
        handles = list(_active_leases.keys())
    for handle in handles:
        try:
            with _leases_lock:
                slots = _active_leases.get(handle)
            if slots is not None:
                bb = session.get_client()
                bb.io_release(slots)
                with _leases_lock:
                    _active_leases.pop(handle, None)
                log.info("Shutdown: released lease %s (slots=%s)", handle, slots)
        except Exception as exc:
            log.warning("Shutdown: failed to release lease %s: %s", handle, exc)


atexit.register(_release_all_leases)


# ---------------------------------------------------------------------------
# Tool registration
# ---------------------------------------------------------------------------

def register(mcp) -> None:

    @mcp.tool()
    def io_claim(
        slots: list[int],
        lease_seconds: float = 30.0,
        purpose: str = "",
    ) -> str:
        """
        Acquire exclusive ownership of one or more BugBuster IO slots and
        return a lease handle.

        Use this when you need to perform multiple operations on the same IOs
        without risk of another client claiming them between calls.  Pass the
        returned handle as ``lease_handle`` to any IO-mutating tool.

        Slots 0–11 correspond to IO1–IO12; slots 12–15 correspond to analog
        channels CH0–CH3 (mapped to IO3/6/9/12).

        Parameters:
        - slots: List of slot indices to claim (0–15).
        - lease_seconds: Lease duration in seconds (default 30.0).
          Use 0 for an infinite lease (must release explicitly).
        - purpose: Short label for status display (e.g. "voltage sweep").

        Returns: lease handle string — pass to io_release or as lease_handle
        on mutating tools.
        """
        if not slots:
            raise ValueError("slots must be a non-empty list.")
        for s in slots:
            if not isinstance(s, int) or s < 0 or s > 15:
                raise ValueError(
                    f"Invalid slot {s!r}. Slots must be integers 0–15 "
                    "(0–11 = IO1–IO12, 12–15 = CH0–CH3)."
                )

        handle = uuid.uuid4().hex
        bb = session.get_client()
        # Coordinate with the bugbuster Python lib (landed by parallel agent).
        # io_claim acquires the slots for lease_seconds; 0 means infinite.
        bb.io_claim(slots, lease_seconds, purpose)

        with _leases_lock:
            _active_leases[handle] = list(slots)

        log.info("io_claim: handle=%s slots=%s lease_s=%.1f purpose=%r",
                 handle, slots, lease_seconds, purpose)
        return handle

    @mcp.tool()
    def io_release(handle: str) -> dict:
        """
        Release a previously acquired IO lease by handle.

        All slots claimed under this handle are released on the device and the
        handle is invalidated.

        Parameters:
        - handle: The string returned by io_claim.

        Returns: success, slots_released.
        """
        with _leases_lock:
            slots = _active_leases.pop(handle, None)

        if slots is None:
            raise ValueError(
                f"Unknown lease handle {handle!r}. "
                "It may have already been released or never existed."
            )

        bb = session.get_client()
        bb.io_release(slots)

        log.info("io_release: handle=%s slots=%s", handle, slots)
        return {"success": True, "slots_released": slots}

    @mcp.tool()
    def io_owner_status() -> list[dict]:
        """
        Return the current IO ownership table from the device.

        Reports which client (if any) holds each of the 16 slots.  Slots 0–11
        map to IO1–IO12; slots 12–15 map to CH0–CH3.

        Returns: list of 16 dicts, each containing:
          - slot (int)
          - owner_kind (str): "NONE", "USB", "HTTP", "SCRIPT", "CLI", "INTERNAL"
          - session_id (int)
          - lease_until_ms (int): 0 = infinite / no lease
          - purpose_tag (int): FNV-1a of caller-supplied label
        """
        bb = session.get_client()
        entries = bb.io_owner_status()
        return entries

    @mcp.tool()
    def io_force_release(
        slot: int,
        admin_token: Optional[str] = None,
    ) -> dict:
        """
        Forcibly release a single IO slot, overriding whoever currently holds it.

        Requires admin privileges.  If admin_token is omitted the server uses
        the token configured at startup (--admin-token flag).  If neither is
        available the call is rejected.

        Parameters:
        - slot: Slot index to force-release (0–15), or -1 to release all.
        - admin_token: Optional override token (never echoed in responses).

        Returns: success, slot.
        """
        if slot != -1 and (not isinstance(slot, int) or slot < 0 or slot > 15):
            raise ValueError(
                f"Invalid slot {slot!r}. Use 0–15 or -1 for all."
            )

        # Resolve token — caller-supplied takes precedence, else server default.
        # NEVER include the token value in any error message or log output.
        resolved_token: Optional[str] = admin_token or session._admin_token
        if not resolved_token:
            raise PermissionError(
                "Admin token required for io_force_release. "
                "Supply admin_token or start the server with --admin-token."
            )

        bb = session.get_client()
        # Pass resolved token to the lib; it validates against the device.
        # Catch authentication errors without leaking the token value.
        try:
            bb.io_force_release(slot, resolved_token)
        except PermissionError as exc:
            raise PermissionError("Invalid admin token — io_force_release rejected.") from exc
        except Exception as exc:
            # Scrub any token-like content from the error string before re-raising.
            msg = str(exc)
            if resolved_token and resolved_token in msg:
                msg = msg.replace(resolved_token, "<redacted>")
            raise RuntimeError(f"io_force_release failed: {msg}") from None

        # Invalidate any local lease that covers this slot.
        with _leases_lock:
            if slot == -1:
                _active_leases.clear()
            else:
                stale = [h for h, slots in _active_leases.items() if slot in slots]
                for h in stale:
                    _active_leases.pop(h, None)

        log.info("io_force_release: slot=%s (admin token NOT logged)", slot)
        return {"success": True, "slot": slot}
