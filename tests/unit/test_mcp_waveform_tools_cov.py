"""Coverage for bugbuster_mcp.tools.waveform: wavegen, ADC snapshot, LA capture
and the async job-id variants. No hardware: client/HAL are autospec mocks and
time/threading are replaced with deterministic fakes."""
from __future__ import annotations

import math
import time
from types import SimpleNamespace
from unittest.mock import patch

import pytest

from bugbuster import BugBuster
from bugbuster.constants import OutputMode, WaveformType
from bugbuster.hal import PortMode
from bugbuster_mcp import session
from bugbuster_mcp.config import LA_CAPTURE_TIMEOUT_S, MAX_SNAPSHOT_SAMPLES, WAVEFORM_PREVIEW_POINTS
from bugbuster_mcp.tools import io_owner, waveform
from tests.unit._mock_client import make_client_mock, make_hal_mock


class _DummyMCP:
    def __init__(self):
        self.tools = {}

    def tool(self):
        def deco(fn):
            self.tools[fn.__name__] = fn
            return fn
        return deco


class _FakeClock:
    """monotonic() only advances when sleep() is called."""

    def __init__(self, step: float = 0.0):
        self.now = 0.0
        self.step = step

    def monotonic(self):
        self.now += self.step
        return self.now

    def sleep(self, s):
        self.now += s


class _SyncThread:
    """Runs the job body inline so async-job tests are deterministic."""

    def __init__(self, target, daemon=None):
        self._target = target

    def start(self):
        self._target()


@pytest.fixture(autouse=True)
def _session_state():
    saved = (session._transport, session._port, session._host, session._vlogic,
             session._admin_token, session._active_board)
    session.configure(transport="usb", port="/dev/null", vlogic=3.3)
    yield
    (session._transport, session._port, session._host, session._vlogic,
     session._admin_token, session._active_board) = saved
    with waveform._jobs_lock:
        waveform._jobs.clear()


@pytest.fixture
def env():
    bb = make_client_mock()
    bb.power_get_status.return_value = {}
    bb.hat_status_cached.return_value = {"detected": True}
    bb.hat_la_get_status.return_value = {"state": 3}
    hal = make_hal_mock()
    hal._io_mode = {}
    with patch("bugbuster_mcp.session.get_client", return_value=bb), \
         patch("bugbuster_mcp.session.get_hal", return_value=hal):
        yield bb, hal


def test_register_exposes_all_ten_tools():
    mcp = _DummyMCP()
    waveform.register(mcp)
    assert set(mcp.tools) == {
        "start_waveform", "stop_waveform", "capture_adc_snapshot", "capture_logic_analyzer",
        "capture_adc_snapshot_start", "capture_adc_snapshot_status", "capture_adc_snapshot_result",
        "capture_logic_analyzer_start", "capture_logic_analyzer_status", "capture_logic_analyzer_result",
    }


# ---------------------------------------------------------------------------
# start_waveform / stop_waveform
# ---------------------------------------------------------------------------

@pytest.mark.parametrize("io,ch", [(3, 0), (6, 1), (9, 2), (12, 3)])
def test_start_waveform_maps_io_to_channel(env, io, ch):
    bb, hal = env
    hal._io_mode = {io: PortMode.ANALOG_OUT}
    res = waveform.start_waveform(io, "Triangle", 10.0, 2.0, offset=3.0)
    bb.start_waveform.assert_called_once_with(
        channel=ch, waveform=WaveformType(2), freq_hz=10.0, amplitude=2.0,
        offset=3.0, mode=OutputMode.VOLTAGE,
    )
    assert res == {"io": io, "channel": ch, "waveform": "triangle", "freq_hz": 10.0,
                   "amplitude": 2.0, "offset": 3.0, "success": True}


def test_start_waveform_appends_fault_warnings(env):
    bb, hal = env
    hal._io_mode = {3: PortMode.ANALOG_OUT}
    bb.power_get_status.return_value = {"efuse_faults": [False, True]}
    res = waveform.start_waveform(3, "sine", 1.0, 1.0, offset=1.0)
    assert len(res["warnings"]) == 1 and "E-fuse 2" in res["warnings"][0]


@pytest.mark.parametrize("kwargs,match", [
    (dict(io=1, waveform="sine", freq_hz=1.0, amplitude=1.0), "does not support"),
    (dict(io=13, waveform="sine", freq_hz=1.0, amplitude=1.0), "not valid"),
    (dict(io=3, waveform="noise", freq_hz=1.0, amplitude=1.0), "Unknown waveform"),
    (dict(io=3, waveform="sine", freq_hz=0.05, amplitude=1.0), "out of range"),
    (dict(io=3, waveform="sine", freq_hz=100.5, amplitude=1.0), "out of range"),
    (dict(io=3, waveform="sine", freq_hz=1.0, amplitude=-0.1), "non-negative"),
    (dict(io=3, waveform="sine", freq_hz=1.0, amplitude=7.0, offset=-5.5), "exceeds DAC maximum"),
])
def test_start_waveform_validation(env, kwargs, match):
    bb, hal = env
    hal._io_mode = {kwargs["io"]: PortMode.ANALOG_OUT}
    with pytest.raises(ValueError, match=match):
        waveform.start_waveform(**kwargs)
    bb.start_waveform.assert_not_called()


@pytest.mark.parametrize("freq", [0.1, 100.0])
def test_start_waveform_accepts_frequency_bounds(env, freq):
    bb, hal = env
    hal._io_mode = {9: PortMode.ANALOG_OUT}
    assert waveform.start_waveform(9, "sawtooth", freq, 6.0, offset=6.0)["success"] is True
    bb.start_waveform.assert_called_once()


@pytest.mark.parametrize("mode", [None, PortMode.ANALOG_IN])
def test_start_waveform_requires_analog_out_mode(env, mode):
    bb, hal = env
    hal._io_mode = {3: mode} if mode else {}
    with pytest.raises(ValueError, match="configure_io"):
        waveform.start_waveform(3, "square", 1.0, 1.0)
    bb.start_waveform.assert_not_called()


def test_stop_waveform_clean(env):
    bb, _ = env
    res = waveform.stop_waveform()
    bb.stop_waveform.assert_called_once_with()
    assert res["success"] is True and "HIGH_IMP" in res["message"]
    assert "warnings" not in res


def test_stop_waveform_reports_power_good_loss(env):
    bb, _ = env
    bb.power_get_status.return_value = {"vadj1_en": True, "vadj1_pg": False}
    res = waveform.stop_waveform()
    assert any("VADJ1" in w for w in res["warnings"])


# ---------------------------------------------------------------------------
# capture_adc_snapshot
# ---------------------------------------------------------------------------

POLL = 1 / 128   # binary-exact so sample counts do not depend on float rounding


@pytest.fixture
def clock():
    c = _FakeClock()
    with patch.object(waveform, "time", SimpleNamespace(monotonic=c.monotonic, sleep=c.sleep)), \
         patch.object(waveform, "SNAPSHOT_POLL_INTERVAL_S", POLL):
        yield c


def _adc_env(env, io=3):
    bb, hal = env
    hal._io_mode = {io: PortMode.ANALOG_IN}
    return hal


def test_snapshot_flat_signal_statistics(env, clock):
    hal = _adc_env(env)
    hal.read_voltage.return_value = 2.5
    res = waveform.capture_adc_snapshot(3, duration_s=10 * POLL)
    assert res["n_samples"] == 10
    assert res["min_v"] == res["max_v"] == res["mean_v"] == 2.5
    assert res["stddev_v"] == 0 and res["peak_to_peak_v"] == 0
    assert res["frequency_hz"] is None
    assert res["waveform_preview"] == [2.5] * 10
    assert res["sample_rate_hz"] == pytest.approx(128.0)
    hal.read_voltage.assert_called_with(3)


def test_snapshot_estimates_frequency_of_a_sine(env, clock):
    hal = _adc_env(env, io=12)
    # 128 SPS, 10 Hz sine; phase offset avoids exact zeros
    vals = [5.0 + 2.0 * math.sin(2 * math.pi * 10 * (i + 0.25) / 128) for i in range(400)]
    hal.read_voltage.side_effect = vals
    res = waveform.capture_adc_snapshot(12, duration_s=5.0, n_samples=200)
    assert res["n_samples"] == 200
    assert res["frequency_hz"] == pytest.approx(10.0, rel=0.1)
    assert res["peak_to_peak_v"] == pytest.approx(4.0, abs=0.1)
    assert len(res["waveform_preview"]) == WAVEFORM_PREVIEW_POINTS


def test_snapshot_caps_samples_and_duration(env, clock):
    hal = _adc_env(env)
    hal.read_voltage.return_value = 1.0
    res = waveform.capture_adc_snapshot(3, duration_s=999.0)
    assert res["n_samples"] == MAX_SNAPSHOT_SAMPLES
    assert len(res["waveform_preview"]) == WAVEFORM_PREVIEW_POINTS


def test_snapshot_stops_on_read_error_and_keeps_partial(env, clock):
    hal = _adc_env(env)
    hal.read_voltage.side_effect = [1.0, 2.0, RuntimeError("spi")]
    res = waveform.capture_adc_snapshot(3, duration_s=1.0)
    assert res["n_samples"] == 2 and res["mean_v"] == 1.5


def test_snapshot_no_samples_is_runtime_error(env, clock):
    hal = _adc_env(env)
    hal.read_voltage.side_effect = RuntimeError("spi")
    with pytest.raises(RuntimeError, match="No samples"):
        waveform.capture_adc_snapshot(3)


@pytest.mark.parametrize("duration", [0.0, -1.0])
def test_snapshot_rejects_non_positive_duration(env, clock, duration):
    hal = _adc_env(env)
    with pytest.raises(ValueError, match="positive"):
        waveform.capture_adc_snapshot(3, duration_s=duration)
    hal.read_voltage.assert_not_called()


@pytest.mark.parametrize("mode,name", [(None, "UNCONFIGURED"), (PortMode.ANALOG_OUT, "ANALOG_OUT")])
def test_snapshot_requires_analog_in(env, mode, name):
    _, hal = env
    hal._io_mode = {3: mode} if mode else {}
    with pytest.raises(ValueError, match=name):
        waveform.capture_adc_snapshot(3)


def test_snapshot_rejects_digital_io(env):
    with pytest.raises(ValueError, match="does not support"):
        waveform.capture_adc_snapshot(2)


# ---------------------------------------------------------------------------
# capture_logic_analyzer
# ---------------------------------------------------------------------------

def _pack_1ch(samples):
    out = bytearray()
    for i in range(0, len(samples), 8):
        b = 0
        for k, s in enumerate(samples[i:i + 8]):
            b |= s << k
        out.append(b)
    return bytes(out)


@pytest.fixture
def la(env, clock):
    bb, _ = env
    bb.hat_la_read_all.return_value = _pack_1ch([0, 1] * 8)
    bb.hat_la_decode.side_effect = BugBuster.hat_la_decode
    return bb


def test_la_none_trigger_forces_and_returns_shape(la):
    bb = la
    bb.hat_la_get_status.side_effect = [{"state": 0}, {"state": 0}, {"state": 3, "actual_rate_hz": 1000}]
    res = waveform.capture_logic_analyzer(channels=1, rate_hz=1000, depth=16)
    bb.hat_la_configure.assert_called_once_with(channels=1, rate_hz=1000, depth=16)
    bb.hat_la_set_trigger.assert_not_called()
    bb.hat_la_arm.assert_called_once_with()
    bb.hat_la_force.assert_called_once_with()
    assert res["rate_hz"] == 1000
    assert res["duration_ms"] == 16.0
    assert res["channel_edges"][0]["transitions"] == 15
    assert res["frequency_hz"][0] == pytest.approx(500.0)
    assert res["timing_diagram"].startswith("CH0: _‾_‾")
    assert set(res) == {"channels", "rate_hz", "depth", "duration_ms", "channel_edges",
                        "frequency_hz", "protocol_hints", "timing_diagram"}


@pytest.mark.parametrize("trig,code", [("rising", 1), ("FALLING", 2), ("both", 3), ("high", 4), ("low", 5)])
def test_la_edge_trigger_is_programmed_not_forced(la, trig, code):
    bb = la
    waveform.capture_logic_analyzer(channels=1, rate_hz=1000, depth=16, trigger_type=trig, trigger_ch=2)
    bb.hat_la_set_trigger.assert_called_once_with(code, 2)
    bb.hat_la_force.assert_not_called()


@pytest.mark.parametrize("status,rate", [
    ({"stateName": "DONE", "actualRateHz": 2000}, 2000),
    ({"state_name": "done", "clockHz": 4000}, 4000),
    ({"done": True}, 1000),
])
def test_la_done_status_variants(la, status, rate):
    la.hat_la_get_status.side_effect = None
    la.hat_la_get_status.return_value = status
    assert waveform.capture_logic_analyzer(channels=1, rate_hz=1000, depth=16)["rate_hz"] == rate


def test_la_unknown_trigger_rejected_before_configure(la):
    with pytest.raises(ValueError, match="Unknown trigger_type"):
        waveform.capture_logic_analyzer(channels=1, rate_hz=1000, depth=16, trigger_type="edge")
    la.hat_la_configure.assert_not_called()


def test_la_timeout_stops_capture(la, clock):
    clock.step = 0.5
    la.hat_la_get_status.return_value = {"state": 1}
    with patch.object(waveform, "require_la_ready"):
        with pytest.raises(RuntimeError, match="timed out"):
            waveform.capture_logic_analyzer(channels=1, rate_hz=1000, depth=16, trigger_type="rising")
    la.hat_la_stop.assert_called_once_with()
    la.hat_la_read_all.assert_not_called()
    assert clock.now >= LA_CAPTURE_TIMEOUT_S


def test_la_empty_capture_is_error(la):
    la.hat_la_read_all.return_value = b""
    with pytest.raises(RuntimeError, match="empty capture"):
        waveform.capture_logic_analyzer(channels=1, rate_hz=1000, depth=16)


def test_la_reports_fault_warnings(la):
    la.power_get_status.return_value = {"efuse_faults": [True]}
    res = waveform.capture_logic_analyzer(channels=1, rate_hz=1000, depth=16)
    assert "E-fuse 1" in res["warnings"][0]


def test_la_requires_hat(la):
    la.hat_status_cached.return_value = {"detected": False}
    with pytest.raises(RuntimeError, match="No HAT"):
        waveform.capture_logic_analyzer()
    la.hat_la_configure.assert_not_called()


def test_la_busy_analyzer_refused(la):
    la.hat_la_get_status.return_value = {"state": 2, "stateName": "CAPTURING"}
    with pytest.raises(RuntimeError, match="CAPTURING"):
        waveform.capture_logic_analyzer()


@pytest.mark.parametrize("kwargs", [dict(channels=3), dict(rate_hz=0), dict(rate_hz=200_000_000),
                                    dict(channels=4, depth=1_000_000)])
def test_la_config_limits(la, kwargs):
    with pytest.raises(ValueError):
        waveform.capture_logic_analyzer(**kwargs)
    la.hat_la_configure.assert_not_called()


def test_la_multichannel_short_edges_give_zero_frequency(la):
    # 2 channels: ch0 toggles once, ch1 constant -> <3 edges -> 0 Hz
    la.hat_la_decode.side_effect = None
    la.hat_la_decode.return_value = [[0, 0, 1, 1], [1, 1, 1, 1], [0, 1, 0, 1]]
    res = waveform.capture_logic_analyzer(channels=2, rate_hz=1000, depth=4)
    assert res["frequency_hz"] == [0, 0]
    assert len(res["channel_edges"]) == 2
    assert res["protocol_hints"] == ["No standard protocol detected at these frequencies."]


# ---------------------------------------------------------------------------
# helpers
# ---------------------------------------------------------------------------

@pytest.mark.parametrize("freq,needle", [
    (115_200, "UART baud 115200"),
    (9_600, "UART baud 9600"),
    (100_000, "I2C standard"),
    (400_000, "I2C fast (400 kHz)"),
    (1_000_000, "SPI/I2C fast+"),
])
def test_protocol_hints(freq, needle):
    hints = waveform._protocol_hints([0, freq], 10_000_000)
    assert any(needle in h and h.startswith("CH1") for h in hints)


def test_protocol_hints_none_matching():
    assert waveform._protocol_hints([0, 3.0], 1000) == ["No standard protocol detected at these frequencies."]


def test_timing_diagram_edge_cases():
    assert waveform._timing_diagram([], 1, 10) == "No data"
    assert waveform._timing_diagram([[1]], 1, 0) == "No data"
    out = waveform._timing_diagram([[1, 0], []], 3, 4).splitlines()
    assert out == ["CH0: ‾___", "CH1: ____", "CH2: ____"]


# ---------------------------------------------------------------------------
# async job variants
# ---------------------------------------------------------------------------

@pytest.fixture
def sync_threads():
    with patch.object(waveform.threading, "Thread", _SyncThread):
        yield


def test_adc_job_lifecycle(env, clock, sync_threads):
    hal = _adc_env(env)
    hal.read_voltage.return_value = 1.25
    started = waveform.capture_adc_snapshot_start(3, duration_s=0.02)
    assert started["status"] == "pending"
    job = started["job_id"]
    assert waveform.capture_adc_snapshot_status(job) == {"job_id": job, "status": "done"}
    res = waveform.capture_adc_snapshot_result(job)
    assert res["io"] == 3 and res["mean_v"] == 1.25
    with pytest.raises(ValueError, match="Unknown job_id"):
        waveform.capture_adc_snapshot_result(job)        # consumed


def test_adc_job_error_is_reported(env, sync_threads):
    _, hal = env
    hal._io_mode = {}
    job = waveform.capture_adc_snapshot_start(3)["job_id"]
    assert waveform.capture_adc_snapshot_status(job)["status"] == "error"
    with pytest.raises(RuntimeError, match="Capture job failed: .*ANALOG_IN"):
        waveform.capture_adc_snapshot_result(job)


def test_job_result_before_done_is_refused():
    job = waveform._new_job()
    assert waveform.capture_logic_analyzer_status(job)["status"] == "pending"
    with pytest.raises(RuntimeError, match="not finished"):
        waveform.capture_logic_analyzer_result(job)
    assert job in waveform._jobs                          # not consumed


@pytest.mark.parametrize("fn", [waveform.capture_adc_snapshot_status, waveform.capture_logic_analyzer_status,
                                waveform.capture_adc_snapshot_result, waveform.capture_logic_analyzer_result])
def test_unknown_job_id(fn):
    with pytest.raises(ValueError, match="Unknown job_id"):
        fn("nope")


def test_real_thread_job_completes():
    done = {"x": 1}
    job = waveform._new_job()
    waveform._run_job(job, lambda: done)
    for _ in range(2000):
        if waveform._job_status(job)["status"] == "done":
            break
        time.sleep(0.001)
    assert waveform._job_result(job) is done


def test_la_job_passes_arguments(sync_threads):
    with patch.object(waveform, "capture_logic_analyzer", return_value={"ok": 1}) as cap:
        job = waveform.capture_logic_analyzer_start(2, 5000, 64, "rising", 1)["job_id"]
    cap.assert_called_once_with(2, 5000, 64, "rising", 1)
    assert waveform.capture_logic_analyzer_result(job) == {"ok": 1}


def test_la_job_lease_must_cover_all_mainboard_ios(sync_threads):
    with patch.dict(io_owner._active_leases, {"h-part": [0, 1], "h-all": list(range(12))}, clear=True), \
         patch.object(waveform, "capture_logic_analyzer", return_value={}):
        with pytest.raises(ValueError, match="does not cover slot 2"):
            waveform.capture_logic_analyzer_start(lease_handle="h-part")
        assert waveform.capture_logic_analyzer_start(lease_handle="h-all")["status"] == "pending"


def test_adc_job_unknown_lease_rejected_without_starting():
    with patch.dict(io_owner._active_leases, {}, clear=True):
        with pytest.raises(ValueError, match="Unknown lease"):
            waveform.capture_adc_snapshot_start(3, lease_handle="ghost")
    assert waveform._jobs == {}


def test_adc_job_lease_on_all_analog_slots_accepted(sync_threads):
    with patch.dict(io_owner._active_leases, {"h": [12, 13, 14, 15]}, clear=True), \
         patch.object(waveform, "capture_adc_snapshot", return_value={}) as cap:
        waveform.capture_adc_snapshot_start(6, 0.5, 7, lease_handle="h")
    cap.assert_called_once_with(6, 0.5, 7)


@pytest.mark.xfail(strict=True, reason="BUG: capture_adc_snapshot_start demands a lease on all four "
                                       "analog slots (12-15) instead of the captured IO's own slot")
def test_adc_job_lease_on_own_slot_is_enough(sync_threads):
    with patch.dict(io_owner._active_leases, {"h": [12]}, clear=True), \
         patch.object(waveform, "capture_adc_snapshot", return_value={}):
        assert waveform.capture_adc_snapshot_start(3, lease_handle="h")["status"] == "pending"
