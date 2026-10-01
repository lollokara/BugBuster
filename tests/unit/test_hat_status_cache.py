"""TR-9: every HAT/DAQ MCP tool sent an uncached HAT_GET_STATUS before doing
its own work, so an n-frame tool cost n+1 frames (and HAT_GET_STATUS itself
makes the S3 query the HAT over UART).

B: a positive HAT status is cached per connection for HAT_CACHE_TTL_S;
reconnect, hat_reset/hat_detect and expiry invalidate it, and a "not
detected" result is never cached (a HAT plugged in later is noticed).
Simulator frame counter.
"""

import pytest

import bugbuster as bb
from bugbuster.constants import CmdId
from bugbuster_mcp.safety import require_hat
from tests.mock import SimulatedDevice, SimulatedUSBTransport


def _client():
    dev = SimulatedDevice()
    c = bb.BugBuster(SimulatedUSBTransport(dev))
    c.connect()
    dev.hat.present = True
    sent = []
    orig = c._t.send_command

    def counting(cmd, payload=b"", timeout=None):
        sent.append(int(cmd))
        return orig(cmd, payload) if timeout is None else orig(cmd, payload, timeout)

    c._t.send_command = counting
    return c, sent


def _hat_frames(sent):
    return sum(1 for s in sent if s == int(CmdId.HAT_GET_STATUS))


@pytest.mark.xfail(strict=True, reason="TR-9")
def test_repeated_hat_checks_cost_one_status_frame():
    c, sent = _client()
    if not c.hat_get_status().get("detected"):
        pytest.skip("simulator has no HAT")
    sent.clear()
    for _ in range(5):
        require_hat(c)
    assert _hat_frames(sent) == 1


@pytest.mark.xfail(strict=True, reason="TR-9")
def test_hat_reset_invalidates_the_cache():
    c, sent = _client()
    require_hat(c)
    c.hat_invalidate_cache()
    sent.clear()
    require_hat(c)
    assert _hat_frames(sent) == 1
