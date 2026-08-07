"""The USB reader thread must never die silently.

Regression for a real bench failure: the reader loop caught OSError and simply
`break`-ed, leaving `_running` True and the port open. `send_command()` kept
writing successfully while nobody read the replies, so every command timed out
forever and the sequence number kept climbing - the device looked dead while
HTTP and the DAQ data plane were both fine.
"""
import queue
import threading
import time
from unittest.mock import patch

import pytest

from bugbuster.transport.usb import LinkDownError, USBTransport


class _FakeSerial:
    """Minimal pyserial stand-in whose read() can be made to explode."""

    def __init__(self, fail_with=None):
        self.is_open = True
        self.written = bytearray()
        self._fail_with = fail_with
        self._released = threading.Event()

    def read(self, _n):
        if self._fail_with is not None:
            exc, self._fail_with = self._fail_with, None
            raise exc
        self._released.wait(0.01)
        return b""

    def write(self, data):
        self.written.extend(data)
        return len(data)

    def flush(self):
        pass

    def close(self):
        self.is_open = False


def _transport_with(fake, *, running=True):
    t = USBTransport("COM_TEST")
    t._serial = fake
    t._running = running
    t.auto_reconnect = False
    return t


def _run_reader(t):
    th = threading.Thread(target=t._reader_loop, daemon=True)
    t._reader_thread = th
    th.start()
    th.join(timeout=2.0)
    return th


def test_reader_death_marks_link_unhealthy():
    t = _transport_with(_FakeSerial(fail_with=OSError("device vanished")))
    _run_reader(t)
    assert not t.is_healthy()
    assert t._link_error is not None
    assert not t._running, "a dead reader must not leave _running True"


def test_reader_death_releases_waiting_commands():
    t = _transport_with(_FakeSerial(fail_with=OSError("device vanished")))
    waiter: queue.Queue = queue.Queue()
    t._pending[1234] = waiter
    _run_reader(t)
    result = waiter.get(timeout=1.0)
    assert isinstance(result, LinkDownError), (
        "pending commands must fail fast, not sit until their timeout")


def test_typeerror_from_pyserial_race_is_also_fatal():
    # Windows pyserial can raise TypeError when the port closes mid-read.
    t = _transport_with(_FakeSerial(fail_with=TypeError("byref on freed OVERLAPPED")))
    _run_reader(t)
    assert not t.is_healthy()


def test_send_command_refuses_a_dead_link_instead_of_timing_out():
    t = _transport_with(_FakeSerial(fail_with=OSError("boom")))
    _run_reader(t)
    t._timeout = 0.2
    start = time.monotonic()
    with pytest.raises(LinkDownError):
        t.send_command(0x01)
    assert time.monotonic() - start < 0.15, "should fail fast, not wait the timeout"


def test_send_command_auto_reconnects_when_enabled():
    t = _transport_with(_FakeSerial(fail_with=OSError("boom")))
    _run_reader(t)
    t.auto_reconnect = True
    called = {"n": 0}

    def _fake_reconnect():
        called["n"] += 1
        t._serial = _FakeSerial()
        t._link_error = None
        t._running = True
        t._reader_thread = threading.Thread(target=lambda: time.sleep(0.5), daemon=True)
        t._reader_thread.start()

    with patch.object(t, "reconnect", _fake_reconnect):
        t._timeout = 0.05
        with pytest.raises(TimeoutError):   # reconnected, then genuinely no reply
            t.send_command(0x01)
    assert called["n"] == 1


def test_graceful_stop_is_not_reported_as_a_failure():
    fake = _FakeSerial()
    t = _transport_with(fake)
    th = threading.Thread(target=t._reader_loop, daemon=True)
    t._reader_thread = th
    th.start()
    time.sleep(0.05)
    t._running = False          # what disconnect() does
    th.join(timeout=2.0)
    assert t._link_error is None, "a clean shutdown must not set a link error"


def test_healthy_requires_a_live_reader_thread():
    fake = _FakeSerial()
    t = _transport_with(fake)
    t._reader_thread = None
    assert not t.is_healthy(), "an open port alone must not count as healthy"
