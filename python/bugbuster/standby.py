"""
System standby: wire codec, policy constants and the logical presence session.

The device (ESP32-S3) owns the standby policy. A host announces that a control
client is *actually connected* by registering a random non-zero client id
("presence"); the device keeps it for ``PRESENCE_TTL_S`` and drops the oldest
of ``PRESENCE_MAX_CLIENTS`` slots when full. A refresh is NOT activity: it keeps
the client counted but never resets the inactivity timer.

Wire format (BBP ``STANDBY`` 0x78, payload[0] = sub-op):

========  ====  ====================================================
sub-op    len   request payload
========  ====  ====================================================
0 status  1     ``[op]``
1 presence 6    ``[op][u32 id LE][u8 present]``
2 policy  3     ``[op][u16 seconds LE]``  (0 = off, 60, 300, 900)
3 wake    1     ``[op]``
4 sleep   1     ``[op]``  (still refused while a real owner is active)
========  ====  ====================================================

Every response is the same 28-byte status record (schema 1); the HTTP routes
(``/api/standby/{status,presence,policy,wake,sleep}``) return the same fields as
JSON with ``state`` as a string.

BBP command I/O lives in ``client.py``; this module is transport-free so the
codec and the presence state machine are testable without a device.
"""

from __future__ import annotations

import logging
import secrets
import struct
import threading
import time
from dataclasses import asdict, dataclass
from enum import IntEnum
from typing import Any, Callable, Mapping, Optional

log = logging.getLogger(__name__)

SUBOP_STATUS = 0
SUBOP_PRESENCE = 1
SUBOP_POLICY = 2
SUBOP_WAKE = 3
SUBOP_SLEEP = 4

TIMEOUT_CHOICES = (0, 60, 300, 900)
DEFAULT_TIMEOUT_S = 300

STATUS_SCHEMA = 1
STATUS_LEN = 28
_STATUS_FMT = struct.Struct("<BBBBIHBBIHHHHI")

PRESENCE_REFRESH_S = 10.0
PRESENCE_TTL_S = 45.0
PRESENCE_MAX_CLIENTS = 8


class StandbyState(IntEnum):
    ACTIVE = 0
    PREPARING = 1
    ASLEEP = 2
    WAKING = 3
    FAULT_SAFE = 4


STATE_NAMES = {
    StandbyState.ACTIVE: "active",
    StandbyState.PREPARING: "preparing",
    StandbyState.ASLEEP: "asleep",
    StandbyState.WAKING: "waking",
    StandbyState.FAULT_SAFE: "fault_safe",
}
_STATE_BY_NAME = {name: int(state) for state, name in STATE_NAMES.items()}


class StandbyUnsupportedError(NotImplementedError):
    """The connected firmware has no system standby (older than the feature)."""


class StandbyRefusedError(RuntimeError):
    """The device refused to enter standby (a client or running work still owns it)."""

    def __init__(self, message: str, status: Optional["StandbyStatus"] = None):
        super().__init__(message)
        self.status = status


@dataclass(frozen=True)
class StandbyStatus:
    """The 28-byte status record every standby reply carries."""

    schema: int
    state: int                  # StandbyState value, -1 when the name is unknown
    state_name: str
    ready: bool                 # hardware commands are accepted (state ACTIVE and settled)
    stage: int
    generation: int
    timeout_seconds: int
    clients: int
    failed_stage: int
    inhibitors: int
    completed: int
    failed: int
    skipped: int
    reserved: int
    idle_remaining_ms: int

    def as_dict(self) -> dict[str, Any]:
        return asdict(self)

    @property
    def asleep(self) -> bool:
        return self.state == StandbyState.ASLEEP


def parse_standby_status(data: bytes) -> StandbyStatus:
    """Decode the binary status record. Raises ``ValueError`` on a bad record."""
    if len(data) < STATUS_LEN:
        raise ValueError(f"STANDBY status is {len(data)} bytes, need {STATUS_LEN}")
    (schema, state, ready, stage, gen, timeout, clients, failed_stage, inhibitors,
     completed, failed, skipped, reserved, idle_ms) = _STATUS_FMT.unpack_from(data)
    if schema != STATUS_SCHEMA:
        raise ValueError(f"unsupported STANDBY status schema {schema} (library knows {STATUS_SCHEMA})")
    try:
        name = STATE_NAMES[StandbyState(state)]
    except ValueError:
        name = f"unknown({state})"
    return StandbyStatus(schema, state, name, bool(ready), stage, gen, timeout, clients,
                         failed_stage, inhibitors, completed, failed, skipped, reserved, idle_ms)


def _norm_state(name: str) -> str:
    return name.strip().lower().replace("-", "_").replace(" ", "_")


def parse_standby_status_json(obj: Mapping[str, Any]) -> StandbyStatus:
    """Decode the HTTP/BLE JSON form (same fields; ``state`` is a string)."""
    if not isinstance(obj, Mapping) or "state" not in obj or "ready" not in obj:
        raise ValueError("standby JSON status needs 'state' and 'ready'")
    raw_state = obj["state"]
    if isinstance(raw_state, str):
        name = _norm_state(raw_state)
        state = _STATE_BY_NAME.get(name, -1)
    else:
        state = int(raw_state)
        known = state in _STATE_BY_NAME.values()
        name = STATE_NAMES[StandbyState(state)] if known else f"unknown({state})"

    def num(*keys: str) -> int:
        for k in keys:
            if k in obj and obj[k] is not None:
                return int(obj[k])
        return 0

    return StandbyStatus(
        schema=num("schema") or STATUS_SCHEMA, state=state, state_name=name,
        ready=bool(obj["ready"]), stage=num("stage"), generation=num("gen", "generation"),
        timeout_seconds=num("timeoutSeconds", "timeout_seconds"), clients=num("clients"),
        failed_stage=num("failedStage", "failed_stage"), inhibitors=num("inhibitors"),
        completed=num("completed"), failed=num("failed"), skipped=num("skipped"),
        reserved=num("reserved"), idle_remaining_ms=num("idleRemainingMs", "idle_remaining_ms"),
    )


def validate_timeout(seconds: int) -> int:
    """Return ``seconds`` if it is one of the persisted policy choices."""
    if isinstance(seconds, bool) or seconds not in TIMEOUT_CHOICES:
        raise ValueError(f"standby timeout must be one of {TIMEOUT_CHOICES} seconds (0 = off), got {seconds!r}")
    return int(seconds)


def encode_status_request() -> bytes:
    return bytes([SUBOP_STATUS])


def encode_presence(client_id: int, present: bool) -> bytes:
    if not 0 < client_id <= 0xFFFFFFFF:
        raise ValueError("presence client id must be a non-zero u32")
    return struct.pack("<BIB", SUBOP_PRESENCE, client_id, 1 if present else 0)


def encode_policy(seconds: int) -> bytes:
    return struct.pack("<BH", SUBOP_POLICY, validate_timeout(seconds))


def encode_wake() -> bytes:
    return bytes([SUBOP_WAKE])


def encode_sleep() -> bytes:
    return bytes([SUBOP_SLEEP])


def new_client_id() -> int:
    """A random non-zero u32, fresh for every connection epoch."""
    return secrets.randbelow(0xFFFFFFFF) + 1


def is_unsupported_error(exc: BaseException) -> bool:
    """True when ``exc`` means 'this firmware does not know standby'."""
    from .transport.usb import DeviceError
    if isinstance(exc, NotImplementedError):
        return True
    if isinstance(exc, DeviceError):
        return exc.code == 0x01                # BBP_ERR_INVALID_CMD
    status = getattr(getattr(exc, "response", None), "status_code", None)
    return status in (404, 405)


def is_busy_error(exc: BaseException) -> bool:
    """True for BBP BUSY / HTTP 503: the device is preparing, asleep or waking."""
    from .transport.usb import DeviceError
    if isinstance(exc, DeviceError):
        return exc.code == 0x06
    return getattr(getattr(exc, "response", None), "status_code", None) == 503


def is_refusal_error(exc: BaseException) -> bool:
    """True when a sleep request was refused: BBP BUSY/INVALID_STATE or HTTP 409/503."""
    from .transport.usb import DeviceError
    if isinstance(exc, DeviceError):
        return exc.code in (0x06, 0x07)
    return getattr(getattr(exc, "response", None), "status_code", None) in (409, 503)


# (client_id, present) -> status or None. May raise.
PresenceSender = Callable[[int, bool], Optional[StandbyStatus]]


class PresenceSession:
    """One logical connection epoch registered with the device.

    ``open()`` starts an epoch with a fresh random id; ``tick()`` refreshes it
    every ``refresh_s`` and is meant to be driven by the transport's existing
    keepalive owner. ``close()`` releases it. ``detach()``/``attach()`` let a
    caller (MCP sleep) hold the epoch closed until an explicit wake action.

    Failures never raise: an old firmware flips ``capability`` to
    ``"unsupported"`` once and the session goes quiet; transient errors are
    logged once per failure streak and retried on the next due tick.
    """

    def __init__(self, send: PresenceSender, *, refresh_s: float = PRESENCE_REFRESH_S,
                 clock: Callable[[], float] = time.monotonic,
                 new_id: Callable[[], int] = new_client_id):
        self._send = send
        self._refresh_s = refresh_s
        self._clock = clock
        self._new_id = new_id
        self._lock = threading.RLock()
        self.client_id = 0
        self.capability = "unknown"        # unknown | supported | unsupported
        self.detached = False
        self.last_status: Optional[StandbyStatus] = None
        self._next_due = 0.0
        self._failures = 0

    @property
    def active(self) -> bool:
        """An epoch is wanted (it may not have been acknowledged yet)."""
        return self.client_id != 0 and not self.detached

    def open(self) -> bool:
        with self._lock:
            if self.capability == "unsupported" or self.detached:
                return False
            if self.client_id == 0:
                self.client_id = self._new_id()
            return self._attempt()

    def tick(self) -> bool:
        """Refresh when due. Returns True when a frame was attempted."""
        with self._lock:
            if not self.active or self.capability == "unsupported":
                return False
            if self._clock() < self._next_due:
                return False
            self._attempt()
            return True

    def close(self) -> None:
        with self._lock:
            cid, self.client_id = self.client_id, 0
            if cid and self.capability != "unsupported":
                try:
                    self._send(cid, False)
                except Exception as exc:
                    log.debug("standby presence release failed: %s", exc)

    def new_epoch(self) -> bool:
        """Release the old id and register a fresh one (transport reconnected)."""
        with self._lock:
            self.close()
            return self.open()

    def detach(self) -> None:
        with self._lock:
            self.close()
            self.detached = True

    def attach(self) -> bool:
        with self._lock:
            self.detached = False
            return self.open()

    def _attempt(self) -> bool:
        self._next_due = self._clock() + self._refresh_s
        try:
            status = self._send(self.client_id, True)
        except Exception as exc:
            if is_unsupported_error(exc):
                self.capability = "unsupported"
                self.client_id = 0
                log.info("firmware has no system standby; presence heartbeat disabled")
                return False
            self._failures += 1
            if self._failures == 1:
                log.debug("standby presence refresh failed: %s", exc)
            return False
        if self._failures:
            log.debug("standby presence recovered after %d failure(s)", self._failures)
        self._failures = 0
        self.capability = "supported"
        if status is not None:
            self.last_status = status
        return True
