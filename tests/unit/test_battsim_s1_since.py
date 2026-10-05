"""Host decoder for BS_HOP_S1_SINCE stays in lock-step with the P4 source."""

import re
import struct

from bugbuster import battsim as bs
from tests.lib.srcread import read_source

HOST_H = "Firmware/DAQ_HAT/ESP32P4/src/battsim/battsim_host.h"
S1_H = "Firmware/DAQ_HAT/ESP32P4/src/battsim/battsim_s1.h"


def test_opcode_and_limits_match_firmware():
    m = re.search(r"BS_HOP_S1_SINCE\s*=\s*(\d+)", read_source(HOST_H))
    assert m and int(m.group(1)) == bs.BsOp.S1_SINCE
    m = re.search(r"#define BS_S1_SINCE_MAX\s+(\d+)u", read_source(S1_H))
    assert m and int(m.group(1)) == bs.S1_SINCE_MAX
    assert bs.S1_SIZE == 16


def _reply(run, samples, more):
    raw = struct.pack("<HBB", run, len(samples), int(more))
    for s in samples:
        raw += struct.pack(bs._S1_FMT, *s)
    return raw


def test_parse_s1_since():
    run, recs, more = bs.parse_s1_since(_reply(7, [(5, 3005, 9995, 1000, 1, 1)], True))
    assert (run, more) == (7, True)
    r = recs[0]
    assert (r.t_s, r.v, r.soc_pct, r.flags, r.dt_s) == (5, 3.005, 99.95, 1, 1)
    assert abs(r.i - 0.001) < 1e-12 and abs(r.p - 3.005e-3) < 1e-12


class _Client:
    _usb = False

    def __init__(self, pages):
        self.pages = list(pages)
        self.sent = []

    def _http_post(self, path, body):
        self.sent.append(body)
        import base64
        return {"ok": True, "data": base64.b64encode(self.pages.pop(0)).decode()}


def test_samples_since_pages_until_no_more():
    c = _Client([_reply(7, [(t, 3000, 9000, 10, 0, 1) for t in (5, 6)], True),
                 _reply(7, [(7, 3000, 9000, 10, 0, 1)], False)])
    out = bs.BattSim(c).samples_since(7, since_s=4, max_samples=600)
    assert [s.t_s for s in out] == [5, 6, 7]
    # second request resumes after the last t_s received
    assert bytes.fromhex(c.sent[1]["args"]) == struct.pack("<HIB", 7, 6, 14)
