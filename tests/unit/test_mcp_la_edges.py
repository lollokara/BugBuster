"""LA-04: capture_logic_analyzer must report real edges and frequency.

hat_la_decode() returns one 0/1 value per SAMPLE, but the tool treated that
list as edges: the "transition" count was the sample count, so frequency_hz
was always rate/2 and "edges" were raw samples.
"""

from unittest.mock import patch

import pytest

from bugbuster import BugBuster
from bugbuster_mcp.tools import waveform
from tests.unit._mock_client import make_client_mock

RATE = 100_000
DEPTH = 1000          # 10 ms
SQUARE_HZ = 1000      # 100-sample period


def _square_capture() -> bytes:
    period = RATE // SQUARE_HZ
    samples = [1 if (i % period) < period // 2 else 0 for i in range(DEPTH)]
    out = bytearray()
    for i in range(0, DEPTH, 8):  # 1 channel: bit k of each byte = sample k (LSB first)
        b = 0
        for k, s in enumerate(samples[i:i + 8]):
            b |= s << k
        out.append(b)
    return bytes(out)


def _run():
    bb = make_client_mock()
    bb.hat_la_get_status.return_value = {"state": 3, "actual_rate_hz": RATE}
    bb.hat_la_read_all.return_value = _square_capture()
    bb.hat_la_decode.side_effect = BugBuster.hat_la_decode
    with patch.object(waveform.session, "get_client", return_value=bb), \
         patch.object(waveform, "require_la_ready"), \
         patch.object(waveform, "check_faults_post", return_value=[]):
        return waveform.capture_logic_analyzer(channels=1, rate_hz=RATE, depth=DEPTH)


def test_frequency_of_square_wave():
    res = _run()
    assert res["frequency_hz"][0] == pytest.approx(SQUARE_HZ, rel=0.01)


def test_edges_are_transitions_with_timestamps():
    ch = _run()["channel_edges"][0]
    assert ch["transitions"] == 2 * SQUARE_HZ * DEPTH // RATE - 1
    t0, level0 = ch["edges"][0]
    assert t0 == pytest.approx((RATE // SQUARE_HZ // 2) / RATE)
    assert level0 == 0
    assert [lv for _t, lv in ch["edges"][:4]] == [0, 1, 0, 1]
