"""Coverage for bugbuster_mcp.tools.daq_power: DAQ HAT power-profiling tools.

Control-plane tools run against a spec'd client; data-plane tools against a
fake stream that hands back real PowerCapture objects, so report / window /
compare / markers / export exercise the real analysis code.
"""
from __future__ import annotations

import csv
import sys
import time
import types
from types import SimpleNamespace
from unittest.mock import create_autospec, patch

import pytest

from bugbuster.daq_config import DaqAction, DaqConfig, DaqKey
from bugbuster.daq_stream import (
    MARK_KIND_FLAG, MARK_KIND_TRIGGER, META_SATURATED, META_SETTLING,
    MarkerRecord, PowerCapture,
)
from bugbuster_mcp.tools import daq_power
from tests.unit._mock_client import make_client_mock

ALL_TOOLS = {
    "daq_power_stream_status", "daq_power_setup", "daq_set_current_range",
    "daq_set_sample_rate", "daq_set_range_dwell", "daq_range_stability",
    "daq_power_capture", "daq_power_capture_start", "daq_power_capture_status",
    "daq_power_capture_result", "daq_power_report", "daq_power_window",
    "daq_power_compare", "daq_power_export", "daq_power_markers",
    "daq_power_list_captures", "daq_power_drop_capture",
}


class _DummyMCP:
    def __init__(self):
        self.tools = {}

    def tool(self):
        def decorator(func):
            self.tools[func.__name__] = func
            return func
        return decorator


@pytest.fixture(autouse=True)
def _isolated_stores():
    with patch.dict(daq_power._captures, clear=True), \
         patch.dict(daq_power._jobs, clear=True):
        yield


@pytest.fixture
def env():
    mcp = _DummyMCP()
    daq_power.register(mcp)
    bb = make_client_mock()
    bb.daq = create_autospec(DaqConfig, instance=True)
    bb.hat_status_cached.return_value = {"detected": True}
    with patch("bugbuster_mcp.session.get_client", return_value=bb):
        yield SimpleNamespace(t=mcp.tools, bb=bb)


def _sets(bb):
    return [(c.args[0], c.args[1]) for c in bb.daq.set.call_args_list]


# ---------------------------------------------------------------------------
# Capture fixtures
# ---------------------------------------------------------------------------
def _make_cap(n=1000, rate=1000.0, burst_every=100, burst_len=10,
              lo=1e-3, hi=50e-3, v=3.3, meta=0, markers=None,
              start_index=None, **kw) -> PowerCapture:
    current = [hi if (k % burst_every) < burst_len else lo for k in range(n)]
    return PowerCapture(
        sample_rate=rate,
        current=current,
        voltage=[v] * n,
        meta=bytearray([meta] * n),
        markers=list(markers or []),
        start_index=start_index,
        start_timestamp_us=0,
        **kw,
    )


def _marker(idx, ch=5, edge=1, kind=MARK_KIND_FLAG):
    return MarkerRecord(sample_index=idx, timestamp_us=idx * 1000,
                        channel=ch, edge=edge, kind=kind)


class _FakeStream:
    def __init__(self, cap=None, records=None, capture_exc=None):
        self.cap = cap
        self.records = list(records or [])
        self.capture_exc = capture_exc
        self.capture_kwargs = None
        self.calls = []

    def open(self):
        self.calls.append("open")
        return self

    def start(self):
        self.calls.append("start")

    def stop(self):
        self.calls.append("stop")

    def close(self):
        self.calls.append("close")

    def read_records(self, timeout_ms=200):
        self.calls.append("read")
        return self.records.pop(0) if self.records else []

    def capture(self, **kwargs):
        self.capture_kwargs = kwargs
        if self.capture_exc:
            raise self.capture_exc
        return self.cap


# ---------------------------------------------------------------------------
# Registration
# ---------------------------------------------------------------------------
def test_registers_every_tool(env):
    assert set(env.t) == ALL_TOOLS


# ---------------------------------------------------------------------------
# Module helpers
# ---------------------------------------------------------------------------
@pytest.mark.parametrize("sps, idx", [
    (8_000, 0), (64_000, 1), (128_000, 2), (256_000, 3), (512_000, 4),
    (10_000, 0), (50_000, 1), (100_000, 2), (250_000, 3), (1_000_000, 4),
    ("128000", 2),
])
def test_rate_index_real_and_legacy(sps, idx):
    assert daq_power._rate_index(sps) == idx


@pytest.mark.parametrize("sps", [0, 1, 9_999, 2_000_000, -8000])
def test_rate_index_rejects_unknown(sps):
    with pytest.raises(ValueError, match="sample_rate_sps must be one of"):
        daq_power._rate_index(sps)


def test_capture_bytes_uses_current_length_and_tolerates_missing():
    assert daq_power._capture_bytes(_make_cap(n=10)) == 10 * daq_power._BYTES_PER_SAMPLE
    assert daq_power._capture_bytes(SimpleNamespace()) == 0


def test_store_and_get_roundtrip():
    cap = _make_cap(n=5)
    cid = daq_power._store_capture(cap)
    assert cid.startswith("cap-") and len(cid) == len("cap-") + 8
    assert daq_power._get_capture(cid) is cap


def test_get_unknown_capture_raises():
    with pytest.raises(ValueError, match="Unknown capture_id 'nope'"):
        daq_power._get_capture("nope")


def test_store_evicts_oldest_beyond_count_limit():
    ids = [daq_power._store_capture(_make_cap(n=2))
           for _ in range(daq_power.MAX_STORED_CAPTURES + 3)]
    assert list(daq_power._captures) == ids[3:]
    with pytest.raises(ValueError, match="evicted"):
        daq_power._get_capture(ids[0])


def test_store_evicts_by_byte_budget():
    with patch.object(daq_power, "MAX_STORED_BYTES", 10 * daq_power._BYTES_PER_SAMPLE):
        a = daq_power._store_capture(_make_cap(n=6))
        b = daq_power._store_capture(_make_cap(n=6))
        assert list(daq_power._captures) == [b]
        assert a not in daq_power._captures


# ---------------------------------------------------------------------------
# HAT guard
# ---------------------------------------------------------------------------
@pytest.mark.parametrize("name, kwargs", [
    ("daq_power_setup", {"voltage_mv": 3300}),
    ("daq_set_current_range", {"range_name": "ma"}),
    ("daq_set_sample_rate", {}),
    ("daq_set_range_dwell", {"dwell_us": 10}),
    ("daq_range_stability", {"dwell_us": 10}),
])
def test_control_tools_refuse_without_hat(env, name, kwargs):
    env.bb.hat_status_cached.return_value = {"detected": False}
    with pytest.raises(RuntimeError, match="No HAT"):
        env.t[name](**kwargs)
    env.bb.daq.set.assert_not_called()


# ---------------------------------------------------------------------------
# daq_power_stream_status
# ---------------------------------------------------------------------------
@pytest.fixture
def fake_usb():
    usb = types.ModuleType("usb")
    core = types.ModuleType("usb.core")
    usb.core = core
    with patch.dict(sys.modules, {"usb": usb, "usb.core": core}):
        yield


def test_stream_status_without_pyusb(env):
    with patch.dict(sys.modules, {"usb": None, "usb.core": None}):
        out = env.t["daq_power_stream_status"]()
    assert out == {"available": False,
                   "reason": "pyusb is not installed (pip install pyusb)."}


def test_stream_status_interface_not_enumerated(env, fake_usb):
    with patch("bugbuster.daq_stream.daq_stream_present", return_value=False), \
         patch("bugbuster.daq_stream.DaqStream") as ds:
        out = env.t["daq_power_stream_status"]()
    assert out["available"] is False
    assert "0x4001" in out["reason"]
    ds.assert_not_called()


def test_stream_status_returns_first_status_frame(env, fake_usb):
    status = {"sample_rate": 128000, "range": "mid"}
    stream = _FakeStream(records=[
        [],
        [SimpleNamespace(raw="not-a-dict"), SimpleNamespace(raw={"stats": {}}),
         SimpleNamespace(), SimpleNamespace(raw=status),
         SimpleNamespace(raw={"sample_rate": 1})],
    ])
    with patch("bugbuster.daq_stream.daq_stream_present", return_value=True), \
         patch("bugbuster.daq_stream.DaqStream", return_value=stream):
        out = env.t["daq_power_stream_status"]()
    assert out == {"available": True, "device_status": status}
    assert stream.calls == ["open", "start", "read", "read", "stop", "close"]


def test_stream_status_times_out_with_empty_status(env, fake_usb):
    stream = _FakeStream()
    clock = iter([0.0, 0.5, 1.0, 5.0])
    fake_time = SimpleNamespace(monotonic=lambda: next(clock))
    with patch("bugbuster.daq_stream.daq_stream_present", return_value=True), \
         patch("bugbuster.daq_stream.DaqStream", return_value=stream), \
         patch.object(daq_power, "time", fake_time):
        out = env.t["daq_power_stream_status"]()
    assert out == {"available": True, "device_status": {}}
    assert stream.calls.count("read") == 2
    assert stream.calls[-2:] == ["stop", "close"]


def test_stream_status_closes_stream_when_open_fails(env, fake_usb):
    stream = _FakeStream()

    def boom():
        stream.calls.append("open")
        raise OSError("busy")
    stream.open = boom
    with patch("bugbuster.daq_stream.daq_stream_present", return_value=True), \
         patch("bugbuster.daq_stream.DaqStream", return_value=stream), \
         pytest.raises(OSError, match="busy"):
        env.t["daq_power_stream_status"]()
    assert stream.calls == ["open", "close"]


# ---------------------------------------------------------------------------
# daq_power_setup
# ---------------------------------------------------------------------------
def test_setup_applies_everything(env):
    out = env.t["daq_power_setup"](
        voltage_mv=3300, current_limit_ma=500, enable=True, autorange=False,
        sample_rate_sps=100_000, reset_accumulators=True)
    assert out == {"voltage_mv": 3300, "current_limit_ma": 500,
                   "autorange": False, "sample_rate_sps": 128_000,
                   "source_enabled": True, "accumulators_reset": True}
    assert _sets(env.bb) == [
        (DaqKey.DUT_VOLTAGE_MV, 3300), (DaqKey.DUT_ILIMIT_MA, 500),
        (DaqKey.AUTORANGING, False), (DaqKey.SAMPLE_RATE_IDX, 2),
        (DaqKey.SOURCE_ENABLE, True),
    ]
    assert [c.args[0] for c in env.bb.daq.action.call_args_list] == [
        DaqAction.ENERGY_RESET, DaqAction.CHARGE_RESET]


def test_setup_single_setting_and_disable(env):
    assert env.t["daq_power_setup"](enable=False) == {"source_enabled": False}
    assert _sets(env.bb) == [(DaqKey.SOURCE_ENABLE, False)]
    env.bb.daq.action.assert_not_called()


@pytest.mark.parametrize("v", [1800, 20000])
def test_setup_voltage_bounds_inclusive(env, v):
    assert env.t["daq_power_setup"](voltage_mv=v) == {"voltage_mv": v}


@pytest.mark.parametrize("v", [1799, 20001, 0, -3300])
def test_setup_voltage_out_of_range(env, v):
    with pytest.raises(ValueError, match="voltage_mv must be 1800..20000"):
        env.t["daq_power_setup"](voltage_mv=v)
    env.bb.daq.set.assert_not_called()


@pytest.mark.parametrize("i", [99, 2501])
def test_setup_current_limit_out_of_range(env, i):
    with pytest.raises(ValueError, match="current_limit_ma must be 100..2500"):
        env.t["daq_power_setup"](current_limit_ma=i)
    env.bb.daq.set.assert_not_called()


def test_setup_bad_rate(env):
    with pytest.raises(ValueError, match="sample_rate_sps"):
        env.t["daq_power_setup"](sample_rate_sps=12345)
    env.bb.daq.set.assert_not_called()


def test_setup_nothing_requested(env):
    with pytest.raises(ValueError, match="at least one setting"):
        env.t["daq_power_setup"]()


# ---------------------------------------------------------------------------
# daq_set_current_range
# ---------------------------------------------------------------------------
def test_current_range_autorange(env):
    out = env.t["daq_set_current_range"](range_name="garbage", autorange=True)
    assert out == {"autorange": True, "message": "Hardware autoranging enabled."}
    assert _sets(env.bb) == [(DaqKey.AUTORANGING, True)]


@pytest.mark.parametrize("name, idx", [
    ("ua", 2), (" UA ", 2), ("microamp", 2), ("ma", 1), ("milliamp", 1),
    ("a", 0), ("amp", 0), ("lo", 0),
])
def test_current_range_lock(env, name, idx):
    out = env.t["daq_set_current_range"](range_name=name)
    assert out == {"autorange": False, "range": daq_power._RANGE_LABELS[idx]}
    assert _sets(env.bb) == [(DaqKey.AUTORANGING, False), (DaqKey.RANGE_IDX, idx)]


def test_current_range_unknown(env):
    with pytest.raises(ValueError, match="Unknown range 'kA'"):
        env.t["daq_set_current_range"](range_name="kA")
    env.bb.daq.set.assert_not_called()


# ---------------------------------------------------------------------------
# daq_set_sample_rate
# ---------------------------------------------------------------------------
def test_sample_rate_default(env):
    out = env.t["daq_set_sample_rate"]()
    assert out["sample_rate_sps"] == 128_000
    assert out["stream_decimation"] == 1
    assert out["effective_stream_sps"] == 128_000
    assert "note" in out
    assert _sets(env.bb) == [(DaqKey.SAMPLE_RATE_IDX, 2), (DaqKey.USB_DECIMATION, 1)]


def test_sample_rate_legacy_maps_to_real(env):
    assert env.t["daq_set_sample_rate"](sample_rate_sps=1_000_000)["sample_rate_sps"] == 512_000


def test_sample_rate_decimation_with_consent(env):
    out = env.t["daq_set_sample_rate"](sample_rate_sps=64_000, stream_decimation=4,
                                       i_understand_aliasing=True)
    assert out["effective_stream_sps"] == 16_000
    assert _sets(env.bb) == [(DaqKey.SAMPLE_RATE_IDX, 1), (DaqKey.USB_DECIMATION, 4)]


def test_sample_rate_decimation_without_consent(env):
    with pytest.raises(ValueError, match="i_understand_aliasing"):
        env.t["daq_set_sample_rate"](stream_decimation=2)
    env.bb.daq.set.assert_not_called()


@pytest.mark.parametrize("dec", [0, -1, 1001])
def test_sample_rate_decimation_range(env, dec):
    with pytest.raises(ValueError, match="stream_decimation must be 1..1000"):
        env.t["daq_set_sample_rate"](stream_decimation=dec, i_understand_aliasing=True)
    env.bb.daq.set.assert_not_called()


def test_sample_rate_bad_rate(env):
    with pytest.raises(ValueError, match="sample_rate_sps"):
        env.t["daq_set_sample_rate"](sample_rate_sps=7)
    env.bb.daq.set.assert_not_called()


# ---------------------------------------------------------------------------
# daq_set_range_dwell / daq_range_stability
# ---------------------------------------------------------------------------
def test_range_dwell_read_only(env):
    env.bb.daq.get.return_value = 2500
    out = env.t["daq_set_range_dwell"]()
    assert out == {"dwell_us": 2500, "dwell_ms": 2.5, "enabled": True,
                   "persisted": True}
    env.bb.daq.set.assert_not_called()
    env.bb.daq.get.assert_called_once_with(DaqKey.RANGE_DWELL_US)


def test_range_dwell_set_zero_disables(env):
    env.bb.daq.get.return_value = 0
    out = env.t["daq_set_range_dwell"](dwell_us=0)
    assert out["enabled"] is False and out["dwell_us"] == 0
    assert _sets(env.bb) == [(DaqKey.RANGE_DWELL_US, 0)]


@pytest.mark.parametrize("d", [-1, 1_000_001])
def test_range_dwell_out_of_range(env, d):
    with pytest.raises(ValueError, match="dwell_us must be 0..1000000"):
        env.t["daq_set_range_dwell"](dwell_us=d)
    env.bb.daq.set.assert_not_called()


def _stability_reads(env, dwell=100, settle=50, flap=1):
    vals = {DaqKey.RANGE_DWELL_US: dwell, DaqKey.RANGE_LOCK_US: settle,
            DaqKey.RANGE_FLAP: flap}
    env.bb.daq.get.side_effect = lambda k: vals[k]


def test_range_stability_read_only(env):
    _stability_reads(env)
    out = env.t["daq_range_stability"]()
    assert out == {"dwell_us": 100, "settle_us": 50, "anti_flap": True,
                   "persisted": True}
    env.bb.daq.set.assert_not_called()


def test_range_stability_sets_all(env):
    _stability_reads(env, 1_000_000, 65535, 0)
    out = env.t["daq_range_stability"](dwell_us=1_000_000, settle_us=65535,
                                       anti_flap=False)
    assert out["anti_flap"] is False
    assert _sets(env.bb) == [(DaqKey.RANGE_DWELL_US, 1_000_000),
                             (DaqKey.RANGE_LOCK_US, 65535),
                             (DaqKey.RANGE_FLAP, False)]


@pytest.mark.parametrize("kwargs, msg", [
    ({"dwell_us": -1}, "dwell_us must be 0..1000000"),
    ({"dwell_us": 1_000_001}, "dwell_us must be 0..1000000"),
    ({"settle_us": -1}, "settle_us must be 0..65535"),
    ({"settle_us": 65536}, "settle_us must be 0..65535"),
])
def test_range_stability_validation(env, kwargs, msg):
    with pytest.raises(ValueError, match=msg):
        env.t["daq_range_stability"](**kwargs)
    env.bb.daq.set.assert_not_called()


# ---------------------------------------------------------------------------
# daq_power_capture (blocking)
# ---------------------------------------------------------------------------
def test_capture_happy_path(env):
    cap = _make_cap(n=500, rate=1000.0, markers=[_marker(10)], dropped_samples=3,
                    trigger_offset=0)
    stream = _FakeStream(cap=cap)
    with patch.object(daq_power, "_open_stream", return_value=stream):
        out = env.t["daq_power_capture"](
            duration_s=0.5, wait_for_trigger=True, trigger_timeout_s=3.0,
            pre_trigger_s=0.1, trigger_current_a=0.02, trigger_edge="falling")
    cid = out["capture_id"]
    assert out == {
        "capture_id": cid, "sample_count": 500, "sample_rate_sps": 1000.0,
        "duration_s": 0.5, "dropped_samples": 3, "markers": 1,
        "trigger_offset": 0,
        "next": f"Call daq_power_report(capture_id='{cid}') for the "
                f"energy/state analysis.",
    }
    assert daq_power._captures[cid] is cap
    assert stream.capture_kwargs == {
        "duration_s": 0.5, "max_samples": daq_power.MAX_CAPTURE_SAMPLES,
        "wait_for_trigger": True, "trigger_timeout_s": 3.0,
        "pre_trigger_s": 0.1, "trigger_current_a": 0.02,
        "trigger_edge": "falling",
    }
    assert stream.calls[-1] == "close"


def test_capture_closes_stream_on_failure(env):
    stream = _FakeStream(capture_exc=TimeoutError("no trigger"))
    with patch.object(daq_power, "_open_stream", return_value=stream), \
         pytest.raises(TimeoutError):
        env.t["daq_power_capture"](duration_s=1.0)
    assert stream.calls == ["close"]
    assert daq_power._captures == {}


@pytest.mark.parametrize("d", [0.0, 0.0009, -1.0, 901.0, float("nan")])
def test_capture_bad_duration(env, d):
    with patch.object(daq_power, "_open_stream") as op, \
         pytest.raises(ValueError, match="duration_s must be"):
        env.t["daq_power_capture"](duration_s=d)
    op.assert_not_called()


def test_capture_over_10s_points_to_async(env):
    with patch.object(daq_power, "_open_stream") as op, \
         pytest.raises(ValueError, match="daq_power_capture_start"):
        env.t["daq_power_capture"](duration_s=10.5)
    op.assert_not_called()


@pytest.mark.parametrize("pre", [-0.1, 10.01])
def test_capture_bad_pre_trigger_never_opens_stream(env, pre):
    with patch.object(daq_power, "_open_stream") as op, \
         pytest.raises(ValueError, match="pre_trigger_s must be 0..10"):
        env.t["daq_power_capture"](duration_s=1.0, pre_trigger_s=pre)
    op.assert_not_called()


def test_open_stream_uses_daqstream():
    inst = _FakeStream()
    with patch("bugbuster.daq_stream.DaqStream", return_value=inst) as ds:
        assert daq_power._open_stream() is inst
    ds.assert_called_once_with()
    assert inst.calls == ["open"]


# ---------------------------------------------------------------------------
# Async capture lifecycle
# ---------------------------------------------------------------------------
class _ManualThread:
    instances: list = []

    def __init__(self, target, daemon=False):
        self.target = target
        self.daemon = daemon
        self.started = False
        _ManualThread.instances.append(self)

    def start(self):
        self.started = True


@pytest.fixture
def manual_threads():
    _ManualThread.instances = []
    with patch.object(daq_power, "threading",
                      SimpleNamespace(Thread=_ManualThread)):
        yield _ManualThread.instances


def test_async_capture_full_lifecycle(env, manual_threads):
    cap = _make_cap(n=200)
    stream = _FakeStream(cap=cap)
    out = env.t["daq_power_capture_start"](duration_s=0.2)
    job = out["job_id"]
    assert out == {"job_id": job, "status": "running", "expected_duration_s": 0.2}
    (th,) = manual_threads
    assert th.started and th.daemon

    st = env.t["daq_power_capture_status"](job)
    assert st["status"] == "running" and st["job_id"] == job
    assert st["expected_duration_s"] == 0.2 and st["elapsed_s"] >= 0
    assert "error" not in st
    with pytest.raises(RuntimeError, match="still running"):
        env.t["daq_power_capture_result"](job)

    with patch.object(daq_power, "_open_stream", return_value=stream):
        th.target()

    assert env.t["daq_power_capture_status"](job)["status"] == "done"
    res = env.t["daq_power_capture_result"](job)
    assert res["sample_count"] == 200
    assert daq_power._captures[res["capture_id"]] is cap
    # consumed
    with pytest.raises(ValueError, match="Unknown job_id"):
        env.t["daq_power_capture_result"](job)
    with pytest.raises(ValueError, match="Unknown job_id"):
        env.t["daq_power_capture_status"](job)


def test_async_capture_error_is_reported(env, manual_threads):
    stream = _FakeStream(capture_exc=TimeoutError("trigger never fired"))
    job = env.t["daq_power_capture_start"](duration_s=60.0)["job_id"]
    with patch.object(daq_power, "_open_stream", return_value=stream):
        manual_threads[0].target()
    st = env.t["daq_power_capture_status"](job)
    assert st["status"] == "error"
    assert st["error"] == "trigger never fired"
    with pytest.raises(RuntimeError, match="Capture failed: trigger never fired"):
        env.t["daq_power_capture_result"](job)


def test_async_bad_pre_trigger_surfaces_as_job_error(env, manual_threads):
    job = env.t["daq_power_capture_start"](duration_s=1.0, pre_trigger_s=11.0)["job_id"]
    with patch.object(daq_power, "_open_stream") as op:
        manual_threads[0].target()
    op.assert_not_called()
    assert "pre_trigger_s" in env.t["daq_power_capture_status"](job)["error"]


@pytest.mark.parametrize("d", [0.0, 900.5, -2.0])
def test_async_bad_duration(env, manual_threads, d):
    with pytest.raises(ValueError, match="duration_s must be"):
        env.t["daq_power_capture_start"](duration_s=d)
    assert manual_threads == []
    assert daq_power._jobs == {}


def test_async_accepts_long_duration(env, manual_threads):
    out = env.t["daq_power_capture_start"](duration_s=900.0)
    assert out["status"] == "running"


def test_async_passes_trigger_args(env, manual_threads):
    stream = _FakeStream(cap=_make_cap(n=10))
    env.t["daq_power_capture_start"](duration_s=5.0, wait_for_trigger=True,
                                     trigger_timeout_s=7.0, pre_trigger_s=1.0,
                                     trigger_current_a=0.5, trigger_edge="falling")
    with patch.object(daq_power, "_open_stream", return_value=stream):
        manual_threads[0].target()
    assert stream.capture_kwargs["trigger_timeout_s"] == 7.0
    assert stream.capture_kwargs["pre_trigger_s"] == 1.0
    assert stream.capture_kwargs["trigger_current_a"] == 0.5
    assert stream.capture_kwargs["trigger_edge"] == "falling"
    assert stream.capture_kwargs["wait_for_trigger"] is True


def test_async_runs_on_real_thread(env):
    stream = _FakeStream(cap=_make_cap(n=10))
    with patch.object(daq_power, "_open_stream", return_value=stream):
        job = env.t["daq_power_capture_start"](duration_s=0.01)["job_id"]
        for _ in range(500):
            if env.t["daq_power_capture_status"](job)["status"] != "running":
                break
            time.sleep(0.01)
    assert env.t["daq_power_capture_result"](job)["sample_count"] == 10


@pytest.mark.parametrize("tool", ["daq_power_capture_status", "daq_power_capture_result"])
def test_unknown_job(env, tool):
    with pytest.raises(ValueError, match="Unknown job_id 'zzz'"):
        env.t[tool]("zzz")


# ---------------------------------------------------------------------------
# daq_power_report
# ---------------------------------------------------------------------------
def test_report_real_analysis(env):
    cid = daq_power._store_capture(_make_cap(n=1000, rate=1000.0))
    rep = env.t["daq_power_report"](cid, battery_capacity_mah=225.0)
    assert rep["capture_id"] == cid
    assert rep["capture"]["sample_count"] == 1000
    assert rep["capture"]["duration_s"] == pytest.approx(1.0)
    tot = rep["totals"]
    expected_mean = (0.1 * 50e-3) + (0.9 * 1e-3)
    assert tot["current_mean_a"] == pytest.approx(expected_mean)
    assert tot["current_max_a"] == pytest.approx(50e-3)
    assert tot["energy_j"] == pytest.approx(expected_mean * 3.3, rel=0.02)
    assert len(rep["states"]) == 2
    assert {s["label"] for s in rep["states"]} == {"sleep", "peak"}
    assert rep["periodicity"]["periodic"] is True
    assert rep["periodicity"]["period_s"] == pytest.approx(0.1)
    assert rep["battery"]["battery_capacity_mah"] == 225.0
    assert rep["battery"]["estimated_hours"] > 0
    assert 0 < len(rep["preview"]) <= 200
    assert "warnings" in rep


def test_report_clamps_output_limits(env):
    cid = daq_power._store_capture(_make_cap(n=2000))
    rep = env.t["daq_power_report"](cid, preview_points=-5, max_segments=0)
    assert rep["preview"] == []
    assert len(rep["segments"]) == 1
    assert rep["segments_truncated"] is True
    rep = env.t["daq_power_report"](cid, preview_points=10_000, max_segments=10_000)
    assert len(rep["preview"]) <= 1000
    assert "segments_truncated" not in rep


def test_report_includes_marker_windows(env):
    cap = _make_cap(n=1000, markers=[_marker(100), _marker(300), _marker(600)])
    cid = daq_power._store_capture(cap)
    rep = env.t["daq_power_report"](cid)
    assert len(rep["marker_windows"]) == 2
    assert len(rep["marker_list"]) == 3


@pytest.mark.parametrize("ms", [1, 11])
def test_report_max_states_validation(env, ms):
    cid = daq_power._store_capture(_make_cap(n=10))
    with pytest.raises(ValueError, match="max_states must be 2..10"):
        env.t["daq_power_report"](cid, max_states=ms)


def test_report_unknown_capture(env):
    with pytest.raises(ValueError, match="Unknown capture_id"):
        env.t["daq_power_report"]("cap-missing")


# ---------------------------------------------------------------------------
# daq_power_window
# ---------------------------------------------------------------------------
def test_window_slices_and_rebases_markers(env):
    cap = _make_cap(n=1000, rate=1000.0, start_index=5000,
                    markers=[_marker(5050), _marker(5250), _marker(5350),
                             _marker(5900)])
    cid = daq_power._store_capture(cap)
    rep = env.t["daq_power_window"](cid, start_s=0.2, end_s=0.4, preview_points=50)
    assert rep["capture_id"] == cid
    assert rep["window"] == {"start_s": 0.2, "end_s": 0.4, "samples": 200}
    assert rep["capture"]["sample_count"] == 200
    assert rep["capture"]["markers"] == 2
    offsets = [m["sample_offset"] for m in rep["marker_list"]]
    assert offsets == [50, 150]
    assert rep["totals"]["current_max_a"] == pytest.approx(50e-3)
    assert len(rep["preview"]) <= 50


def test_window_defaults_to_whole_capture_and_clamps(env):
    cid = daq_power._store_capture(_make_cap(n=300, rate=100.0))
    rep = env.t["daq_power_window"](cid, start_s=-5.0)
    assert rep["window"] == {"start_s": 0.0, "end_s": 3.0, "samples": 300}
    rep = env.t["daq_power_window"](cid, start_s=1.0, end_s=999.0)
    assert rep["window"]["samples"] == 200


@pytest.mark.parametrize("start, end", [(0.5, 0.5), (0.6, 0.4), (5.0, None), (0.0, -1.0)])
def test_window_empty(env, start, end):
    cid = daq_power._store_capture(_make_cap(n=100, rate=100.0))
    with pytest.raises(ValueError, match="Empty window"):
        env.t["daq_power_window"](cid, start_s=start, end_s=end)


def test_window_requires_sample_rate(env):
    cid = daq_power._store_capture(_make_cap(n=10, rate=0.0))
    with pytest.raises(ValueError, match="no valid sample rate"):
        env.t["daq_power_window"](cid)


def test_window_unknown_capture(env):
    with pytest.raises(ValueError, match="Unknown capture_id"):
        env.t["daq_power_window"]("nope")


@pytest.mark.xfail(strict=True, reason="BUG: daq_power_window builds the slice "
                   "without device_status, so a window over an uncalibrated "
                   "range is not flagged uncalibrated while the full report is")
def test_window_keeps_calibration_standing(env):
    cap = _make_cap(n=100, rate=100.0, meta=0,
                    device_status={"cal_have_hi": False, "cal_have_mid": True,
                                   "cal_have_lo": True})
    cid = daq_power._store_capture(cap)
    assert env.t["daq_power_report"](cid)["totals"].get("uncalibrated") is True
    win = env.t["daq_power_window"](cid, start_s=0.1, end_s=0.5)
    assert win["totals"].get("uncalibrated") is True


# ---------------------------------------------------------------------------
# daq_power_compare
# ---------------------------------------------------------------------------
def test_compare_power_and_current_deltas(env):
    a = daq_power._store_capture(_make_cap(n=1000, lo=2e-3, hi=2e-3))
    b = daq_power._store_capture(_make_cap(n=500, lo=1e-3, hi=1e-3))
    out = env.t["daq_power_compare"](a, b)
    assert out["a"]["capture_id"] == a and out["b"]["capture_id"] == b
    assert out["a"]["mean_current_a"] == pytest.approx(2e-3)
    assert out["b"]["mean_current_a"] == pytest.approx(1e-3)
    assert out["a"]["mean_power_w"] == pytest.approx(2e-3 * 3.3)
    d = out["delta"]
    assert d["mean_current_a"] == pytest.approx(-1e-3)
    assert d["mean_current_a_pct"] == pytest.approx(-50.0)
    assert d["mean_power_w_pct"] == pytest.approx(-50.0)
    assert d["peak_current_a_pct"] == pytest.approx(-50.0)
    for k in ("mean_power_w", "mean_current_a", "peak_current_a",
              "energy_per_second_j"):
        assert k in d and f"{k}_pct" in d


def test_compare_zero_baseline_gives_none_pct(env):
    a = daq_power._store_capture(_make_cap(n=0))
    b = daq_power._store_capture(_make_cap(n=10))
    out = env.t["daq_power_compare"](a, b)
    assert out["a"]["mean_power_w"] == 0.0
    assert out["delta"]["mean_power_w_pct"] is None
    assert out["delta"]["mean_current_a"] > 0


def test_compare_unknown_capture(env):
    a = daq_power._store_capture(_make_cap(n=10))
    with pytest.raises(ValueError, match="Unknown capture_id 'missing'"):
        env.t["daq_power_compare"](a, "missing")


@pytest.mark.xfail(strict=True, reason="BUG: daq_power_compare reads "
                   "integrate()['duration_s'], a key integrate never returns "
                   "(it has duration_total_s), so duration_s and "
                   "energy_per_second_j are always 0")
def test_compare_normalises_by_duration(env):
    a = daq_power._store_capture(_make_cap(n=1000, rate=1000.0, lo=2e-3, hi=2e-3))
    b = daq_power._store_capture(_make_cap(n=500, rate=1000.0, lo=1e-3, hi=1e-3))
    out = env.t["daq_power_compare"](a, b)
    assert out["a"]["duration_s"] == pytest.approx(1.0, rel=0.01)
    assert out["b"]["duration_s"] == pytest.approx(0.5, rel=0.01)
    assert out["a"]["energy_per_second_j"] == pytest.approx(2e-3 * 3.3, rel=0.01)
    assert out["delta"]["energy_per_second_j_pct"] == pytest.approx(-50.0, rel=0.02)


# ---------------------------------------------------------------------------
# daq_power_markers
# ---------------------------------------------------------------------------
def _marker_cap():
    return _make_cap(n=1000, rate=1000.0, start_index=100, markers=[
        _marker(200, ch=5, edge=1),
        _marker(400, ch=5, edge=0),
        _marker(500, ch=7, edge=1, kind=MARK_KIND_TRIGGER),
        _marker(700, ch=5, edge=1),
    ])


def test_markers_all_channels(env):
    cid = daq_power._store_capture(_marker_cap())
    out = env.t["daq_power_markers"](cid)
    assert out["capture_id"] == cid
    assert out["marker_count"] == 4
    assert out["markers_truncated"] is False
    first = out["markers"][0]
    assert first["sample_offset"] == 100 and first["t_s"] == pytest.approx(0.1)
    assert first["edge"] == "rising" and first["kind"] == "flag"
    assert out["per_channel"] == {
        5: {"rising": 2, "falling": 1, "trigger": 0},
        7: {"rising": 1, "falling": 0, "trigger": 1},
    }
    assert len(out["windows"]) == 3
    w0 = out["windows"][0]
    assert w0["duration_total_s"] == pytest.approx(0.2)
    assert w0["energy_j"] > 0


def test_markers_filtered_by_channel(env):
    cid = daq_power._store_capture(_marker_cap())
    out = env.t["daq_power_markers"](cid, channel=5)
    assert out["marker_count"] == 3
    assert set(out["per_channel"]) == {5}
    assert len(out["windows"]) == 2
    out = env.t["daq_power_markers"](cid, channel=12)
    assert out == {"capture_id": cid, "marker_count": 0, "markers": [],
                   "markers_truncated": False, "per_channel": {}, "windows": []}


def test_markers_truncated(env):
    cap = _make_cap(n=1000, markers=[_marker(k % 1000) for k in range(600)])
    cid = daq_power._store_capture(cap)
    out = env.t["daq_power_markers"](cid)
    assert out["marker_count"] == 600
    assert len(out["markers"]) == 500
    assert out["markers_truncated"] is True
    assert len(out["windows"]) <= 200


def test_markers_unknown_capture(env):
    with pytest.raises(ValueError, match="Unknown capture_id"):
        env.t["daq_power_markers"]("nope")


# ---------------------------------------------------------------------------
# daq_power_export
# ---------------------------------------------------------------------------
def _read_csv(path):
    with open(path, newline="", encoding="utf-8") as fh:
        return list(csv.reader(fh))


def test_export_writes_all_columns(env, tmp_path):
    cap = PowerCapture(
        sample_rate=1000.0,
        current=[0.001, 0.002, float("nan"), 0.004],
        voltage=[3.3, 3.3, 3.3],
        meta=bytearray([0x00, 0x01 | (1 << 2) | META_SATURATED,
                        0x02 | (2 << 2) | META_SETTLING]),
    )
    cid = daq_power._store_capture(cap)
    path = tmp_path / "out.csv"
    out = env.t["daq_power_export"](cid, str(path))
    assert out == {"path": str(path), "rows": 4, "truncated": False}
    rows = _read_csv(path)
    assert rows[0] == ["t_s", "current_a", "voltage_v", "power_w", "range",
                       "source", "saturated", "settling"]
    assert len(rows) == 5
    assert rows[1] == ["0.000000000", "0.001", "3.3", str(0.001 * 3.3),
                       "hi", "fine", "0", "0"]
    assert rows[2][0] == "0.001000000"
    assert rows[2][4:] == ["mid", "coarse", "1", "0"]
    # NaN current: no power value
    assert rows[3][1] == "nan" and rows[3][3] == ""
    assert rows[3][4:] == ["lo", "blend", "0", "1"]
    # shorter voltage/meta arrays: blank voltage, meta defaults to 0
    assert rows[4][2] == "" and rows[4][3] == ""
    assert rows[4][4:] == ["hi", "fine", "0", "0"]


def test_export_unknown_range_and_source(env, tmp_path):
    cap = PowerCapture(sample_rate=10.0, current=[0.1], voltage=[1.0],
                       meta=bytearray([0x03 | (3 << 2)]))
    cid = daq_power._store_capture(cap)
    path = tmp_path / "x.csv"
    env.t["daq_power_export"](cid, str(path))
    assert _read_csv(path)[1][4:6] == ["unknown", "?"]


def test_export_truncates_to_max_rows(env, tmp_path):
    cid = daq_power._store_capture(_make_cap(n=50))
    path = tmp_path / "t.csv"
    out = env.t["daq_power_export"](cid, str(path), max_rows=10)
    assert out == {"path": str(path), "rows": 10, "truncated": True}
    assert len(_read_csv(path)) == 11


def test_export_requires_sample_rate(env, tmp_path):
    cid = daq_power._store_capture(_make_cap(n=5, rate=0.0))
    path = tmp_path / "r.csv"
    with pytest.raises(ValueError, match="no valid sample rate"):
        env.t["daq_power_export"](cid, str(path))
    assert not path.exists()


def test_export_unknown_capture(env, tmp_path):
    with pytest.raises(ValueError, match="Unknown capture_id"):
        env.t["daq_power_export"]("nope", str(tmp_path / "n.csv"))
    assert not (tmp_path / "n.csv").exists()


@pytest.mark.xfail(strict=True, reason="BUG: daq_power_export reports "
                   "rows=min(sample_count, max_rows) unclamped, so a negative "
                   "max_rows writes 0 rows but returns a negative row count")
def test_export_negative_max_rows_reports_zero_rows(env, tmp_path):
    cid = daq_power._store_capture(_make_cap(n=20))
    path = tmp_path / "neg.csv"
    try:
        out = env.t["daq_power_export"](cid, str(path), max_rows=-5)
    except ValueError:
        return
    assert len(_read_csv(path)) == 1
    assert out["rows"] == 0


# ---------------------------------------------------------------------------
# list / drop
# ---------------------------------------------------------------------------
def test_list_captures_empty(env):
    assert env.t["daq_power_list_captures"]() == {
        "captures": [], "max_stored": daq_power.MAX_STORED_CAPTURES}


def test_list_captures_oldest_first(env):
    a = daq_power._store_capture(_make_cap(n=100, rate=100.0))
    b = daq_power._store_capture(_make_cap(n=10, rate=1000.0,
                                           markers=[_marker(1)], dropped_samples=2))
    out = env.t["daq_power_list_captures"]()
    assert out["captures"] == [
        {"capture_id": a, "sample_count": 100, "sample_rate_sps": 100.0,
         "duration_s": 1.0, "markers": 0, "dropped_samples": 0},
        {"capture_id": b, "sample_count": 10, "sample_rate_sps": 1000.0,
         "duration_s": 0.01, "markers": 1, "dropped_samples": 2},
    ]


def test_drop_capture(env):
    cid = daq_power._store_capture(_make_cap(n=10))
    assert env.t["daq_power_drop_capture"](cid) == {"success": True, "capture_id": cid}
    assert env.t["daq_power_drop_capture"](cid) == {"success": False, "capture_id": cid}
    assert env.t["daq_power_list_captures"]()["captures"] == []
    with pytest.raises(ValueError, match="Unknown capture_id"):
        env.t["daq_power_report"](cid)
