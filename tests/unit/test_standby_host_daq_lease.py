"""DaqStream holds a P4 client lease (USB_CMD_CLIENT_LEASE 0x94) while open."""

from __future__ import annotations

import struct

from bugbuster.daq_stream import CMD_CLIENT_LEASE, LEASE_REFRESH_S, LEASE_TTL_MS, DaqStream


class _Dev:
    def __init__(self):
        self.writes = []

    def write(self, ep, data, timeout):
        self.writes.append(bytes(data))


def _frames(dev):
    out = []
    for raw in dev.writes:
        # build_frame: magic0 magic1 ver type flags.. seq len payload crc  (12-byte header)
        ptype = raw[3]
        plen = struct.unpack_from("<H", raw, 10)[0]
        out.append((ptype, raw[12:12 + plen]))
    return out


def _open(lease=True):
    s = DaqStream(lease=lease)
    dev = _Dev()
    s._dev = dev
    s._lease_start()
    return s, dev


def test_lease_constants_match_the_p4_contract():
    assert CMD_CLIENT_LEASE == 0x94
    assert LEASE_REFRESH_S == 10.0 and LEASE_TTL_MS == 45_000


def test_acquire_frame_layout():
    s, dev = _open()
    try:
        (ptype, payload), = _frames(dev)
        assert ptype == 0x94 and len(payload) == 12
        op, flags, reserved, cid, ttl = struct.unpack("<BBHII", payload)
        assert (op, flags, reserved) == (1, 0, 0)
        assert cid != 0 and cid == s.lease_id and ttl == 45_000
    finally:
        s._lease_stop_and_release()


def test_close_sends_release_with_the_same_id_and_stops_the_thread():
    s, dev = _open()
    thread = s._lease_thread
    cid = s.lease_id
    s._lease_stop_and_release()
    frames = _frames(dev)
    assert [f[0] for f in frames] == [0x94, 0x94]
    op, _, _, rel_id, _ = struct.unpack("<BBHII", frames[1][1])
    assert op == 0 and rel_id == cid
    assert not thread.is_alive() and s.lease_id == 0


def test_lease_can_be_disabled():
    s, dev = _open(lease=False)
    assert dev.writes == [] and s.lease_id == 0 and s._lease_thread is None


def test_each_open_uses_a_fresh_id():
    ids = set()
    for _ in range(20):
        s, _ = _open()
        ids.add(s.lease_id)
        s._lease_stop_and_release()
    assert 0 not in ids and len(ids) > 15


def test_a_failing_write_never_raises_out_of_open():
    class _Bad(_Dev):
        def write(self, *a):
            raise OSError("pipe")

    s = DaqStream()
    s._dev = _Bad()
    s._lease_start()
    s._lease_stop_and_release()
    assert s.lease_id == 0
