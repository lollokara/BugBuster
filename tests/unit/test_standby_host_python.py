"""Standby host integration: wire codec, logical presence session, client API.

Hardware-free. Local fakes only (no shared simulator fixtures): ``_FakeUsb`` is a
USBTransport subclass that emulates the firmware side of BBP 0x78, ``_FakeHttp``
records /api/standby/* calls.
"""

from __future__ import annotations

import struct
import threading
import time
from typing import Optional

import pytest

import bugbuster as bb
from bugbuster.constants import CmdId
from bugbuster.standby import (
    DEFAULT_TIMEOUT_S, PRESENCE_REFRESH_S, TIMEOUT_CHOICES, PresenceSession, StandbyRefusedError,
    StandbyState,
    StandbyUnsupportedError, encode_policy, encode_presence, is_busy_error,
    is_unsupported_error, new_client_id, parse_standby_status, parse_standby_status_json,
    validate_timeout,
)
from bugbuster.transport.http import HTTPLogicalError, HTTPTransport
from bugbuster.transport.usb import DeviceError, USBTransport

STATUS = struct.Struct("<BBBBIHBBIHHHHI")


def status_bytes(state=0, ready=1, stage=0, gen=7, timeout=300, clients=1, failed_stage=0,
                 inhibitors=0, completed=0, failed=0, skipped=0, idle_ms=0, schema=1) -> bytes:
    return STATUS.pack(schema, state, ready, stage, gen, timeout, clients, failed_stage,
                       inhibitors, completed, failed, skipped, 0, idle_ms)


class Clock:
    def __init__(self):
        self.t = 100.0

    def __call__(self):
        return self.t


# --------------------------------------------------------------------------------------
# codec
# --------------------------------------------------------------------------------------

def test_status_record_is_28_bytes_and_decodes_every_field():
    raw = status_bytes(state=2, ready=0, stage=5, gen=0x01020304, timeout=900, clients=3,
                       failed_stage=4, inhibitors=0x80000001, completed=9, failed=1, skipped=2,
                       idle_ms=123456)
    assert len(raw) == 28
    s = parse_standby_status(raw)
    assert (s.state, s.state_name, s.ready, s.stage) == (StandbyState.ASLEEP, "asleep", False, 5)
    assert (s.generation, s.timeout_seconds, s.clients, s.failed_stage) == (0x01020304, 900, 3, 4)
    assert (s.inhibitors, s.completed, s.failed, s.skipped) == (0x80000001, 9, 1, 2)
    assert s.idle_remaining_ms == 123456 and s.asleep


def test_status_offsets_match_the_published_layout():
    raw = status_bytes(state=3, ready=1, stage=9, gen=0xAABBCCDD, timeout=0x1234, clients=5,
                       failed_stage=6, inhibitors=0x11223344, completed=0x0102, failed=0x0304,
                       skipped=0x0506, idle_ms=0x55667788)
    assert raw[0] == 1 and raw[1] == 3 and raw[2] == 1 and raw[3] == 9
    assert struct.unpack_from("<I", raw, 4)[0] == 0xAABBCCDD
    assert struct.unpack_from("<H", raw, 8)[0] == 0x1234
    assert raw[10] == 5 and raw[11] == 6
    assert struct.unpack_from("<I", raw, 12)[0] == 0x11223344
    assert struct.unpack_from("<HHHH", raw, 16) == (0x0102, 0x0304, 0x0506, 0)
    assert struct.unpack_from("<I", raw, 24)[0] == 0x55667788


@pytest.mark.parametrize("bad", [b"", b"\x01" * 27])
def test_short_status_is_rejected(bad):
    with pytest.raises(ValueError):
        parse_standby_status(bad)


def test_unknown_schema_is_rejected_not_misread():
    with pytest.raises(ValueError, match="schema"):
        parse_standby_status(status_bytes(schema=2))


def test_unknown_state_code_is_named_not_crashed():
    assert parse_standby_status(status_bytes(state=9)).state_name == "unknown(9)"


def test_json_status_uses_string_state_and_bool_ready():
    s = parse_standby_status_json({
        "schema": 1, "state": "fault_safe", "ready": False, "stage": 3, "gen": 11,
        "timeoutSeconds": 60, "clients": 2, "failedStage": 4, "inhibitors": 5, "completed": 6,
        "failed": 7, "skipped": 8, "idleRemainingMs": 9})
    assert s.state == StandbyState.FAULT_SAFE and s.ready is False and s.generation == 11
    assert (s.timeout_seconds, s.clients, s.failed_stage, s.idle_remaining_ms) == (60, 2, 4, 9)


def test_json_status_accepts_generation_alias_and_case():
    s = parse_standby_status_json({"state": "Active", "ready": True, "generation": 4, "timeoutSeconds": 300})
    assert s.state == StandbyState.ACTIVE and s.generation == 4


def test_json_without_state_or_ready_is_an_error():
    with pytest.raises(ValueError):
        parse_standby_status_json({"ok": True})


def test_presence_request_layout_is_6_bytes():
    raw = encode_presence(0x01020304, True)
    assert raw == bytes([1, 4, 3, 2, 1, 1]) and len(raw) == 6
    assert encode_presence(5, False)[-1] == 0


def test_presence_id_zero_is_rejected():
    with pytest.raises(ValueError):
        encode_presence(0, True)


def test_policy_request_layout_and_choices():
    assert TIMEOUT_CHOICES == (0, 60, 300, 900) and DEFAULT_TIMEOUT_S == 300
    assert encode_policy(900) == bytes([2, 0x84, 0x03])
    for bad in (1, 30, 600, -1, True):
        with pytest.raises(ValueError):
            validate_timeout(bad)


def test_client_ids_are_random_and_never_zero():
    ids = {new_client_id() for _ in range(200)}
    assert 0 not in ids and len(ids) > 190


def test_unsupported_and_busy_classification():
    assert is_unsupported_error(DeviceError(0x01, 1))
    assert not is_unsupported_error(DeviceError(0x06, 1))
    assert is_busy_error(DeviceError(0x06, 1))

    class _R:
        def __init__(self, code):
            self.status_code = code

    assert is_unsupported_error(HTTPLogicalError("x", response=_R(404)))
    assert is_busy_error(HTTPLogicalError("x", response=_R(503)))
    assert not is_unsupported_error(TimeoutError())


# --------------------------------------------------------------------------------------
# PresenceSession
# --------------------------------------------------------------------------------------

def _session(send, clock=None, ids=None):
    clock = clock or Clock()
    it = iter(ids or range(1, 1000))
    return PresenceSession(send, clock=clock, new_id=lambda: next(it)), clock


def test_open_registers_and_refresh_waits_for_the_period():
    calls = []
    s, clock = _session(lambda cid, present: calls.append((cid, present)))
    assert s.open() and calls == [(1, True)] and s.capability == "supported"
    clock.t += PRESENCE_REFRESH_S - 0.1
    assert s.tick() is False
    clock.t += 0.2
    assert s.tick() is True and calls == [(1, True), (1, True)]


def test_refresh_period_is_ten_seconds_inside_the_45s_ttl():
    assert PRESENCE_REFRESH_S == 10.0


def test_close_releases_the_id_once():
    calls = []
    s, _ = _session(lambda cid, present: calls.append((cid, present)))
    s.open()
    s.close()
    s.close()
    assert calls == [(1, True), (1, False)] and s.client_id == 0 and not s.active


def test_release_of_an_expired_id_is_not_mistaken_for_missing_firmware():
    # The S3 answers a release of an unknown id NOT_FOUND, which travels as BBP INVALID_CMD.
    def send(cid, present):
        if not present:
            raise DeviceError(0x01, 1)

    s, _ = _session(send)
    assert s.open() is True
    s.close()
    assert s.capability == "supported"


def test_each_epoch_gets_a_new_id():
    calls = []
    s, _ = _session(lambda cid, present: calls.append((cid, present)))
    s.open()
    s.new_epoch()
    assert calls == [(1, True), (1, False), (2, True)] and s.client_id == 2


def test_old_firmware_degrades_to_one_attempt_and_silence(caplog):
    attempts = []

    def send(cid, present):
        attempts.append(present)
        raise DeviceError(0x01, 1)

    s, clock = _session(send)
    with caplog.at_level("WARNING", logger="bugbuster.standby"):
        assert s.open() is False
        for _ in range(5):
            clock.t += 60
            assert s.tick() is False
        s.close()
    assert attempts == [True] and s.capability == "unsupported" and s.client_id == 0
    assert not caplog.records


def test_transient_failures_retry_without_log_spam(caplog):
    results = iter([TimeoutError("t"), TimeoutError("t"), None])
    calls = []

    def send(cid, present):
        calls.append(cid)
        r = next(results)
        if isinstance(r, Exception):
            raise r

    s, clock = _session(send)
    with caplog.at_level("DEBUG", logger="bugbuster.standby"):
        assert s.open() is False
        for _ in range(2):
            clock.t += PRESENCE_REFRESH_S + 1
            s.tick()
    assert calls == [1, 1, 1] and s.capability == "supported"
    failures = [r for r in caplog.records if "refresh failed" in r.getMessage()]
    assert len(failures) == 1


def test_detach_holds_closed_until_attach():
    calls = []
    s, clock = _session(lambda cid, present: calls.append((cid, present)))
    s.open()
    s.detach()
    clock.t += 1000
    assert s.tick() is False and s.open() is False
    assert calls == [(1, True), (1, False)] and s.detached
    assert s.attach() is True and calls[-1] == (2, True) and not s.detached


# --------------------------------------------------------------------------------------
# client over USB (local firmware emulation)
# --------------------------------------------------------------------------------------

class _FakeUsb(USBTransport):
    """USBTransport with a minimal BBP 0x78 firmware; records every standby frame."""

    def __init__(self, supported=True):
        self._timeout = 5.0
        self._event_handlers = {}
        self.proto_version = 13
        self.fw_version = (3, 6, 1)
        self.mac = bytes(6)
        self.connect_gen = 0
        self.supported = supported
        self.frames: list[tuple[int, bytes]] = []
        self.clients: dict[int, bool] = {}
        self.timeout = 300
        self.state = StandbyState.ACTIVE
        self.hook = None
        self.hook_set = 0

    def connect(self):
        self.connect_gen += 1
        return 13, (3, 6, 1)

    def disconnect(self):
        pass

    def is_healthy(self):
        return True

    def set_presence_hook(self, hook):
        self.hook = hook
        self.hook_set += 1

    def send_command(self, cmd_id, payload=b"", timeout=None):
        if cmd_id == CmdId.GET_ADMIN_TOKEN:
            return bytes([4]) + b"abcd"
        assert cmd_id == CmdId.STANDBY, f"unexpected command 0x{cmd_id:02X}"
        self.frames.append((cmd_id, bytes(payload)))
        if not self.supported:
            raise DeviceError(0x01, 1)
        op = payload[0]
        if op == 1:
            cid, present = struct.unpack_from("<IB", payload, 1)
            if present:
                self.clients[cid] = True
                if self.state == StandbyState.ASLEEP:
                    self.state = StandbyState.WAKING
            else:
                self.clients.pop(cid, None)
        elif op == 2:
            self.timeout = struct.unpack_from("<H", payload, 1)[0]
        elif op == 3:
            self.state = StandbyState.WAKING
        elif op == 4:
            if self.clients:
                raise DeviceError(0x06, 1)
            self.state = StandbyState.ASLEEP
        return status_bytes(state=int(self.state), ready=int(self.state == StandbyState.ACTIVE),
                            timeout=self.timeout, clients=len(self.clients))

    def ops(self):
        return [p[0] for _, p in self.frames]


def _usb_client(**kw):
    t = _FakeUsb(**kw)
    return bb.BugBuster(t), t


def test_connect_registers_presence_and_disconnect_releases_it():
    client, t = _usb_client()
    client.connect()
    assert len(t.clients) == 1 and client.standby_client_id in t.clients
    assert client.standby_capability == "supported" and t.hook is not None
    client.disconnect()
    assert not t.clients and t.hook is None


def test_old_firmware_connect_is_quiet_and_installs_no_heartbeat():
    client, t = _usb_client(supported=False)
    client.connect()
    assert client.standby_capability == "unsupported" and t.hook_set == 0
    assert len(t.frames) == 1
    client.disconnect()
    assert len(t.frames) == 1


def test_standby_commands_raise_an_explicit_unsupported_error_on_old_firmware():
    client, _ = _usb_client(supported=False)
    client.connect()
    with pytest.raises(StandbyUnsupportedError):
        client.standby_status()
    with pytest.raises(StandbyUnsupportedError):
        client.standby_set_timeout(60)


def test_set_timeout_validates_before_any_frame_is_sent():
    client, t = _usb_client()
    client.connect()
    t.frames.clear()
    with pytest.raises(ValueError):
        client.standby_set_timeout(45)
    assert t.frames == []
    assert client.standby_set_timeout(900).timeout_seconds == 900 and t.timeout == 900


def test_status_and_wake_use_the_documented_subops():
    client, t = _usb_client()
    client.connect()
    t.frames.clear()
    client.standby_status()
    client.standby_wake()
    assert t.ops() == [0, 3]


def test_presence_refresh_is_driven_by_the_transport_hook():
    client, t = _usb_client()
    clock = Clock()
    client.connect()
    client._presence._clock = clock
    client._presence._next_due = clock() + PRESENCE_REFRESH_S
    t.frames.clear()
    t.hook()
    assert t.frames == []
    clock.t += PRESENCE_REFRESH_S + 0.1
    t.hook()
    assert t.ops() == [1]


def test_reconnect_starts_a_new_epoch_and_releases_the_old_id():
    client, t = _usb_client()
    client.connect()
    first = client.standby_client_id
    t.connect_gen += 1
    t.hook()
    second = client.standby_client_id
    assert second not in (0, first)
    assert first not in t.clients and second in t.clients


def test_sleep_releases_the_client_and_stays_released_until_wake():
    client, t = _usb_client()
    client.connect()
    clock = Clock()
    client._presence._clock = clock
    status = client.standby_sleep()
    assert status.state == StandbyState.ASLEEP and not t.clients
    assert client.standby_client_id == 0
    t.frames.clear()
    clock.t += 1000
    t.hook()
    assert t.frames == [], "heartbeat must not reopen presence while detached"
    woke = client.standby_wake()
    assert woke.state == StandbyState.WAKING
    assert len(t.clients) == 1 and client.standby_client_id in t.clients


def test_sleep_without_release_is_refused_by_our_own_presence():
    client, t = _usb_client()
    client.connect()
    with pytest.raises(StandbyRefusedError) as info:
        client.standby_sleep(release_client=False)
    assert info.value.status.state == StandbyState.ACTIVE and info.value.status.clients == 1
    assert len(t.clients) == 1


def test_refused_sleep_keeps_the_release_until_wake():
    client, t = _usb_client()
    client.connect()
    t.clients[0xBEEF] = True            # another control client
    with pytest.raises(StandbyRefusedError):
        client.standby_sleep()
    assert client.standby_client_id == 0 and set(t.clients) == {0xBEEF}


def test_presence_can_be_disabled_per_client():
    t = _FakeUsb()
    client = bb.BugBuster(t, presence=False)
    client.connect()
    assert t.frames == [] and client.standby_capability == "disabled"
    client.disconnect()


def test_wait_ready_polls_until_ready_and_times_out_truthfully():
    client, t = _usb_client()
    client.connect()
    t.state = StandbyState.WAKING
    states = iter([StandbyState.WAKING, StandbyState.WAKING, StandbyState.ACTIVE])
    real = t.send_command

    def send(cmd_id, payload=b"", timeout=None):
        if payload[:1] == bytes([0]):
            t.state = next(states)
        return real(cmd_id, payload)

    t.send_command = send
    assert client.standby_wait_ready(timeout_s=5, poll_s=0.001).ready
    t.state = StandbyState.WAKING
    t.send_command = lambda cmd_id, payload=b"", timeout=None: status_bytes(state=3, ready=0, stage=2)
    with pytest.raises(TimeoutError, match="waking"):
        client.standby_wait_ready(timeout_s=0.02, poll_s=0.001)


# --------------------------------------------------------------------------------------
# client over HTTP
# --------------------------------------------------------------------------------------

JSON_OK = {"schema": 1, "state": "active", "ready": True, "stage": 0, "gen": 1,
           "timeoutSeconds": 300, "clients": 1, "failedStage": 0, "inhibitors": 0,
           "completed": 0, "failed": 0, "skipped": 0, "reserved": 0, "idleRemainingMs": 0}


class _FakeHttp(HTTPTransport):
    def __init__(self, unsupported=False):
        self._admin_token = None
        self.calls: list[tuple[str, str, Optional[dict]]] = []
        self.unsupported = unsupported
        self.hook = None

    def connect(self):
        return {}

    def disconnect(self):
        self.set_presence_hook(None)

    def set_presence_hook(self, hook, period_s=1.0):
        self.hook = hook

    def get(self, path, params=None, headers=None):
        self.calls.append(("GET", path, None))
        return self._reply()

    def post(self, path, body=None, headers=None):
        self.calls.append(("POST", path, body))
        return self._reply()

    def _reply(self):
        if self.unsupported:
            class _R:
                status_code = 404
            raise HTTPLogicalError("HTTP 404", response=_R())
        return dict(JSON_OK)


def test_http_routes_and_bodies():
    t = _FakeHttp()
    client = bb.BugBuster(t)
    client.connect()
    cid = client.standby_client_id
    assert cid != 0
    t.calls.clear()
    client.standby_status()
    client.standby_set_timeout(60)
    client.standby_wake()
    client.standby_sleep()
    assert t.calls == [
        ("GET", "/standby/status", None),
        ("POST", "/standby/policy", {"timeoutSeconds": 60}),
        ("POST", "/standby/wake", {}),
        ("POST", "/standby/presence", {"clientId": cid, "present": False}),
        ("POST", "/standby/sleep", {}),
    ]


def test_http_connect_and_disconnect_send_presence():
    t = _FakeHttp()
    client = bb.BugBuster(t)
    client.connect()
    cid = client.standby_client_id
    client.disconnect()
    assert t.calls == [("POST", "/standby/presence", {"clientId": cid, "present": True}),
                       ("POST", "/standby/presence", {"clientId": cid, "present": False})]


def test_http_404_means_unsupported_not_an_error_storm():
    t = _FakeHttp(unsupported=True)
    client = bb.BugBuster(t)
    client.connect()
    assert client.standby_capability == "unsupported" and t.hook is None
    with pytest.raises(StandbyUnsupportedError):
        client.standby_status()


# --------------------------------------------------------------------------------------
# transport heartbeat owners (real classes, no I/O)
# --------------------------------------------------------------------------------------

def test_usb_keepalive_thread_drives_the_presence_hook_and_stops_bounded():
    t = USBTransport("COM_TEST")
    t.is_healthy = lambda: True
    t.keepalive_tick = lambda: False
    hit = threading.Event()
    t.set_presence_hook(hit.set)
    t._running = True
    t._keepalive_stop.wait = lambda timeout=None: False if not hit.is_set() else True
    th = threading.Thread(target=t._keepalive_loop, daemon=True)
    t._keepalive_thread = th
    th.start()
    assert hit.wait(2.0)
    t._running = False
    t._keepalive_stop.set()
    th.join(2.0)
    assert not th.is_alive()


def test_usb_keepalive_survives_a_failing_hook():
    t = USBTransport("COM_TEST")
    t.is_healthy = lambda: True
    t.keepalive_tick = lambda: False
    calls = []

    def hook():
        calls.append(1)
        if len(calls) == 1:
            raise RuntimeError("boom")

    t.set_presence_hook(hook)
    t._running = True
    ticks = iter([False, False, True])
    t._keepalive_stop.wait = lambda timeout=None: next(ticks)
    t._keepalive_loop()
    assert len(calls) == 2


def test_http_presence_thread_ticks_and_joins_on_clear():
    t = HTTPTransport("192.0.2.1")
    n = []
    t.set_presence_hook(lambda: n.append(1), period_s=0.01)
    deadline = time.monotonic() + 2.0
    while len(n) < 3 and time.monotonic() < deadline:
        time.sleep(0.01)
    assert len(n) >= 3
    thread = t._presence_thread
    t.set_presence_hook(None)
    assert thread is not None and not thread.is_alive() and t._presence_thread is None
    seen = len(n)
    time.sleep(0.05)
    assert len(n) == seen
    t.disconnect()
