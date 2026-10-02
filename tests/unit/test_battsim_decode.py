"""Battery simulator host decoder: struct layouts, tier merge, statistics, transport paging."""
import struct

import pytest

from bugbuster import battsim as bs


def _params(cap=2000, soc10=1000):
    return struct.pack("<BBBBB3xIHHIHHI", 0, 1, 0, 0, 0, cap, soc10, 3000, 50000, 1000, 300, 0)


def _meta(version, run=7, name=b"bench"):
    return struct.pack("<IHHI24s28sI", 0x4E525342, version, run, 1_759_000_000,
                       name.ljust(24, b"\0"), _params(), 0)


def _rec_v2(t, dt, v_mv, soc, imin, imax, q_nc, e_uj, flags=0):
    return struct.pack("<IHHHHiiHHqqq", t, v_mv, v_mv - 5, v_mv + 5, soc, imin, imax,
                       flags, dt, q_nc, q_nc, e_uj)


def _rec_v1(t, v_mv, soc, i_na, q_nc):
    return struct.pack("<IHHHHiiiq", t, v_mv, v_mv, v_mv, soc, i_na, i_na, i_na, q_nc)


def test_sizes_match_firmware():
    assert bs.STATUS_SIZE == 88 and bs.META_SIZE == 68
    assert bs.REC_SIZE == {1: 32, 2: 48}


def test_v2_exact_current_and_energy():
    # 10 mA for 60 s per record = 0.6 C; 3.7 V -> 2.22 J per record.
    recs = b"".join(_rec_v2(60 * k, 60, 3700, 10000 - k, 9_000_000, 11_000_000,
                            600_000_000 * k, 2_220_000 * k) for k in range(1, 6))
    h = bs.history_from_blobs({"meta.bin": _meta(2), "m0000.bin": recs})
    assert len(h.records) == 5
    assert all(r.i_avg == pytest.approx(0.010) for r in h.records)
    st = h.stats()
    assert st["charge_c"] == pytest.approx(3.0)
    assert st["energy_j"] == pytest.approx(11.1)
    assert st["energy_estimated"] is False
    assert st["i_min"] == pytest.approx(0.009) and st["i_max"] == pytest.approx(0.011)
    window = h.stats(120, 240)
    assert window["points"] == 2 and window["energy_j"] == pytest.approx(4.44)


def test_v1_energy_is_estimated_and_dt_inferred():
    recs = b"".join(_rec_v1(60 * k, 4000, 9000, 5_000_000, 300_000_000 * k) for k in range(1, 4))
    h = bs.history_from_blobs({"meta.bin": _meta(1), "m0000.bin": recs})
    assert [r.dt_s for r in h.records] == [60.0, 60.0, 60.0]
    st = h.stats()
    assert st["energy_estimated"] is True
    assert st["energy_j"] == pytest.approx(4.0 * 0.005 * 180)


def test_merge_prefers_finest_tier():
    q15 = b"".join(_rec_v2(900 * k, 900, 3700, 9000, 0, 0, k, k) for k in range(1, 5))     # 0..3600
    m1 = b"".join(_rec_v2(2700 + 60 * k, 60, 3700, 9000, 0, 0, k, k) for k in range(1, 16))  # 2700..3600
    h = bs.history_from_blobs({"meta.bin": _meta(2), "q15.bin": q15, "m0000.bin": m1})
    tiers = [r.tier for r in h.records]
    assert tiers[:3] == ["q15"] * 3 and tiers[3:] == ["m1"] * 15
    assert all(a.t_s < b.t_s for a, b in zip(h.records, h.records[1:], strict=False))


def test_status_decode():
    raw = struct.pack("<BBBBHBBIHHIIffffqqqqqII", 1, 2, bs.FLAG_REMAIN_OK | bs.FLAG_OUTPUT_ON, 0,
                      3, 0, 2, 2500, 8123, 0b101, 3600, 7200, 7.4, 7.39, 0.12, 0.11,
                      1_000_000_000, 900_000_000, 0, 100_000_000, 0, 4 << 20, 1 << 20)
    st = bs.parse_status(raw)
    assert st.state_name == "ACTIVE" and st.soc_pct == pytest.approx(81.23)
    assert st.remaining_s == 7200 and st.q_used_c == pytest.approx(1.0)


class _FakeClient:
    _usb = True

    def __init__(self, files):
        self.files = files    # {file_id: bytes}
        self.calls = []

    def _usb_cmd(self, cmd, payload):
        assert payload[0] == bs.DAQ_CFG_BATTSIM
        op, args = payload[1], payload[2:]
        self.calls.append(op)
        if op == bs.BsOp.LIST_RUNS:
            start = struct.unpack("<H", args)[0]
            ids = list(range(1, 151))[start:start + 100]
            return struct.pack("<HH", 150, 150) + b"".join(struct.pack("<H", i) for i in ids)
        if op == bs.BsOp.RUN_DIR:
            items = sorted(self.files.items())[args[2]:]
            return struct.pack("<HBB", 7, len(self.files), args[2]) + b"".join(
                struct.pack("<HI", fid, len(b)) for fid, b in items)
        if op == bs.BsOp.READ:
            _run, fid, off, n = struct.unpack("<HHIB", args)
            return self.files[fid][off:off + n]
        raise AssertionError(op)


def test_transport_pages_and_reads(tmp_path):
    m1 = b"".join(_rec_v2(60 * k, 60, 3700, 9000, 0, 0, k, k) for k in range(1, 40))
    cli = _FakeClient({0: _meta(2), 0x100: m1})
    b = bs.BattSim(cli)
    ids, active = b.list_runs()
    assert len(ids) == 150 and active == 150
    h = b.history(7)
    assert len(h.records) == 39
    # Incremental sync: second pass only re-reads the head of the unchanged day file.
    d = b.sync_run(7, str(tmp_path))
    cli.calls.clear()
    b.sync_run(7, str(tmp_path))
    assert cli.calls.count(bs.BsOp.READ) <= 2
    assert bs.load_history_dir(d).records[-1].t_s == 39 * 60
