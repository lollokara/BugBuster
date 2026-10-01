"""USB-RX-LAT (found on the board during M5): the reader thread called
`serial.read(512)` with a 100 ms port timeout. pyserial only returns early
once 512 bytes have arrived, so every short response (almost all of them)
waited out the full timeout: ~100 ms per command on top of the device time
(PING measured 107 ms median on COM6).

B: read whatever is waiting (at least one byte), so a response is dispatched
as soon as its terminating 0x00 arrives.
"""

import threading
import time

import pytest

from bugbuster.protocol import build_frame
from bugbuster.transport.usb import USBTransport


class _PyserialLikePort:
    """read(n) returns early only if n bytes are available, else waits the
    port timeout - the pyserial contract the reader has to live with."""

    timeout = 0.1

    def __init__(self, data: bytes):
        self._data = bytearray(data)
        self.is_open = True

    @property
    def in_waiting(self):
        return len(self._data)

    def read(self, n):
        if len(self._data) < n:
            time.sleep(self.timeout)
        out, self._data = bytes(self._data[:n]), self._data[n:]
        return out

    def write(self, d):
        return len(d)

    def flush(self):
        pass

    def close(self):
        self.is_open = False


@pytest.mark.xfail(strict=True, reason="USB-RX-LAT")
def test_short_response_is_dispatched_without_waiting_the_read_timeout():
    frame = build_frame(0x1234, 0xFE, b"")   # a short PING-sized frame
    t = USBTransport("COM_TEST")
    t._serial = _PyserialLikePort(frame)
    t._running = True
    got = threading.Event()
    t._dispatch_frame = lambda raw: got.set()
    th = threading.Thread(target=t._reader_body, daemon=True)
    t0 = time.perf_counter()
    th.start()
    assert got.wait(1.0)
    elapsed = time.perf_counter() - t0
    t._running = False
    th.join(1.0)
    assert elapsed < 0.05, f"dispatched after {elapsed * 1000:.0f} ms"
