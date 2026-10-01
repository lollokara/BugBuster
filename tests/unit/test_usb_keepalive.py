"""TR-3: the firmware leaves BBP binary mode after 60 s without a frame
(`bbp.cpp` BBP_IDLE_TIMEOUT_MS) and the host never sent a keepalive, so the
first command after a long idle went to the text CLI and timed out.

B: the transport sends a PING when nothing has been sent for KEEPALIVE_S
(25 s), well inside the 60 s firmware window. Fake clock, no hardware.
"""


from bugbuster.constants import CmdId
from bugbuster.transport.usb import USBTransport

PING = CmdId.PING


class _Clock:
    def __init__(self):
        self.t = 1000.0

    def __call__(self):
        return self.t


def _transport():
    t = USBTransport("COM_TEST")
    clock = _Clock()
    t._clock = clock
    sent = []
    t.send_command = lambda cmd, payload=b"", timeout=None: sent.append(cmd) or b""
    return t, clock, sent


def test_keepalive_pings_after_idle():
    t, clock, sent = _transport()
    t._last_tx = clock()
    clock.t += 26.0
    assert t.keepalive_tick() is True
    assert sent == [PING]


def test_no_ping_while_traffic_is_recent():
    t, clock, sent = _transport()
    t._last_tx = clock()
    clock.t += 10.0
    assert t.keepalive_tick() is False
    assert sent == []


def test_keepalive_period_is_inside_the_firmware_idle_window():
    assert 0 < USBTransport.KEEPALIVE_S <= 30.0
