"""Tier 2 — the DAQ HAT's BBP control plane, via the S3 mainboard.

Run with:
    PYTHONPATH=python python -m pytest tests/device/test_17_daq_control.py -v \
        --daq --device-usb=/dev/cu.usbmodem1234561

Needs BOTH links: the S3's CDC0 for control, and the P4's USB-HS for the
stream the control plane is supposed to be describing.
"""
import threading
import time

import pytest

from bugbuster.daq_config import DaqAction, DaqKey, DaqTrigEdge, DaqTrigLogic, DaqTrigRole

pytestmark = pytest.mark.requires_daq_bbp


def test_daq_measure_returns_a_plausible_snapshot(daq_bbp):
    m = daq_bbp.daq.measure()
    for field in ("current_a", "voltage_v", "power_w", "range",
                  "streaming", "source_enabled"):
        assert field in m, "DAQ_MEASURE response missing %r: %r" % (field, m)

    assert -5.0 < m["current_a"] < 5.0, "implausible current %r" % m["current_a"]
    assert -1.0 < m["voltage_v"] < 25.0, "implausible voltage %r" % m["voltage_v"]
    assert int(m["range"]) in (0, 1, 2), "range out of 0..2: %r" % m["range"]


def test_daq_measure_power_is_consistent_with_v_times_i(daq_bbp):
    """p == v * i within tolerance, from a single snapshot.

    The three come back in one response but are computed separately, so this
    catches a units or scaling error in any one of them.
    """
    m = daq_bbp.daq.measure()
    expected = m["voltage_v"] * m["current_a"]
    tol = max(abs(expected) * 0.05, 1e-6)
    assert abs(m["power_w"] - expected) <= tol, (
        "power %.9g W != v*i %.9g W (v=%.6g, i=%.9g)"
        % (m["power_w"], expected, m["voltage_v"], m["current_a"]))


def test_daq_measure_reflects_the_streaming_state(daq_bbp, daq_link):
    """The S3's view of `streaming` must track what the P4 is actually doing."""
    daq_link.stop()
    time.sleep(0.5)
    assert daq_bbp.daq.measure()["streaming"] is False, (
        "S3 reports streaming while the P4 is stopped")

    daq_link.drain()
    daq_link.start()
    time.sleep(0.8)
    try:
        assert daq_bbp.daq.measure()["streaming"] is True, (
            "S3 reports not-streaming while the P4 is streaming")
    finally:
        daq_link.stop()


def test_daq_config_get_all_returns_a_dict(daq_bbp):
    cfg = daq_bbp.daq.get_all()
    assert isinstance(cfg, dict) and cfg, "get_all returned %r" % (cfg,)


# ---------------------------------------------------------------------------
# Cross-layer: the S3's view vs the P4's own stream
# ---------------------------------------------------------------------------

def test_measure_agrees_with_the_p4_stream(daq_bbp, daq_safe):
    """DAQ_MEASURE and the USB stream must report the same physical reality.

    DAQ_MEASURE goes S3 -> HAT UART -> P4 and back; the stream comes straight
    off the P4's USB-HS endpoint. They are independent paths to the same
    quantity, so disagreement means a units error, a stale cache, or a broken
    bridge -- and nothing else in this repo checks it.

    Tolerance is deliberately loose: the two are not sampled at the same
    instant, so this is a sanity check on scale and sign, not on precision.
    A units error (mA vs A, mV vs V) is a 1000x discrepancy and is caught
    easily; genuine sampling skew is a few percent and is not flagged.
    """
    from tests.device.test_16_daq_stream import capture, mean_current

    cap = capture(daq_safe, 2.0, settle=0.5)
    stream_i = mean_current(cap)
    volts = cap.all_voltage()
    stream_v = sum(volts) / len(volts) if volts else None
    assert stream_v is not None, "no WAVE_V samples to compare against"

    m = daq_bbp.daq.measure()

    # Voltage: absolute floor of 50 mV absorbs offset on a near-zero rail.
    v_tol = max(abs(stream_v) * 0.20, 0.05)
    assert abs(m["voltage_v"] - stream_v) <= v_tol, (
        "voltage disagrees across layers: DAQ_MEASURE %.6g V vs stream %.6g V "
        "-- a ~1000x gap here means a mV/V units error"
        % (m["voltage_v"], stream_v))

    # Current: absolute floor of 1 mA, since an unloaded bench sits near zero
    # and a relative comparison there is meaningless.
    i_tol = max(abs(stream_i) * 0.20, 1e-3)
    assert abs(m["current_a"] - stream_i) <= i_tol, (
        "current disagrees across layers: DAQ_MEASURE %.9g A vs stream %.9g A "
        "-- a ~1000x gap here means a mA/A units error"
        % (m["current_a"], stream_i))


def test_measure_range_agrees_with_the_stream_meta(daq_bbp, daq_safe):
    """The range the S3 reports must match the range the samples were taken in."""
    from tests.device.test_16_daq_stream import capture
    from tests.lib.daq_records import meta_range

    cap = capture(daq_safe, 1.5, settle=0.5)
    meta = cap.all_meta()
    assert meta, "no meta bytes captured"
    stream_ranges = {meta_range(b) for b in meta}

    m = daq_bbp.daq.measure()
    assert int(m["range"]) in stream_ranges, (
        "DAQ_MEASURE reports range %d but the stream's samples were taken in "
        "range(s) %r" % (int(m["range"]), sorted(stream_ranges)))


# ---------------------------------------------------------------------------
# DAQ_CONFIG — per-key round-trip and actions
# ---------------------------------------------------------------------------

@pytest.mark.parametrize("key,value", [
    (DaqKey.AUTORANGING, 1),
    (DaqKey.FFT_ENABLE, 0),
    (DaqKey.DUT_VOLTAGE_MV, 5000),
    (DaqKey.DUT_ILIMIT_MA, 500),
    (DaqKey.STATS_WINDOW_MS, 1000),
])
def test_daq_config_set_get_roundtrip(daq_bbp, key, value):
    """Each config key must read back what was written.

    Restores the original value afterwards so key order cannot matter.
    """
    try:
        original = daq_bbp.daq.get(key)
    except Exception as exc:
        pytest.skip("cannot read %s: %s" % (key.name, exc))

    try:
        daq_bbp.daq.set(key, value)
        time.sleep(0.2)
        got = daq_bbp.daq.get(key)
        assert int(got) == int(value), (
            "%s: wrote %r, read back %r" % (key.name, value, got))
    finally:
        try:
            daq_bbp.daq.set(key, original)
        except Exception:
            pass


def test_energy_reset_action_is_observable_in_the_stream(daq_bbp, daq_safe):
    """DAQ_CONFIG ACTION ENERGY_RESET must zero the P4's accumulator.

    Cross-layer again: the command goes over BBP, and the evidence that it
    worked comes off the USB stream.
    """
    from tests.device.test_16_daq_stream import capture

    daq_safe.drain()
    daq_safe.start()
    time.sleep(1.5)
    before = daq_safe.collect(1.0)
    daq_safe.stop()
    assert before.energy, "no ENERGY records before the reset"
    e_before = before.energy[-1].elapsed_s

    daq_bbp.daq.action(DaqAction.ENERGY_RESET)
    time.sleep(0.5)

    after = capture(daq_safe, 1.5, settle=0.3)
    assert after.energy, "no ENERGY records after the reset"
    assert after.energy[0].elapsed_s < e_before, (
        "elapsed did not restart after ENERGY_RESET over BBP "
        "(%.3f -> %.3f)" % (e_before, after.energy[0].elapsed_s))


def test_charge_reset_action_is_accepted(daq_bbp):
    daq_bbp.daq.action(DaqAction.CHARGE_RESET)


@pytest.mark.destructive
def test_factory_reset_is_available(daq_bbp, request):
    """Marked destructive: only runs without --skip-destructive."""
    if request.config.getoption("--skip-destructive", default=False):
        pytest.skip("destructive")
    pytest.skip(
        "factory reset would clear calibration stored in NVS (rngcal, smu cal). "
        "Enable deliberately only on a board whose calibration you can restore.")


def test_daq_cal_status_is_readable(daq_bbp):
    """DAQ_CAL STATUS must answer without starting anything.

    START is deliberately not exercised: interactive calibration needs an
    operator with a reference meter, which the unattended constraint forbids.
    """
    st = daq_bbp.daq.cal_status()
    assert isinstance(st, dict) and st, "cal_status returned %r" % (st,)


def test_daq_cal_abort_is_idempotent(daq_bbp):
    """Aborting when no calibration is running must not error."""
    daq_bbp.daq.cal_abort()
    daq_bbp.daq.cal_abort()


# ---------------------------------------------------------------------------
# DAQ_TRIG -- trigger/flag IO configuration
# ---------------------------------------------------------------------------

def test_trigger_get_all_enumerates_twelve_ios(daq_bbp):
    st = daq_bbp.daq_trigger.get_all()
    assert "ios" in st, "get_all returned %r" % (st,)
    assert len(st["ios"]) == 12, "expected 12 IOs, got %d" % len(st["ios"])
    assert {io["io"] for io in st["ios"]} == set(range(1, 13))


@pytest.mark.parametrize("io", range(1, 13))
def test_trigger_set_io_roundtrip(daq_bbp, io):
    """Every one of the 12 mainboard IOs must round-trip its role."""
    original = daq_bbp.daq_trigger.get_io(io)
    try:
        daq_bbp.daq_trigger.set_io(io, role=DaqTrigRole.FLAG, edge=DaqTrigEdge.RISING)
        time.sleep(0.1)
        got = daq_bbp.daq_trigger.get_io(io)
        assert int(got["role"]) == int(DaqTrigRole.FLAG), (
            "IO %d: set role FLAG, read back %r" % (io, got["role"]))
    finally:
        try:
            daq_bbp.daq_trigger.set_io(
                io, role=DaqTrigRole(int(original["role"])),
                edge=DaqTrigEdge(int(original["edge"])))
        except Exception:
            pass


@pytest.mark.parametrize("logic", [DaqTrigLogic.OR, DaqTrigLogic.AND])
def test_trigger_logic_roundtrip(daq_bbp, logic):
    original = daq_bbp.daq_trigger.get_all()["logic"]
    try:
        daq_bbp.daq_trigger.set_logic(logic)
        time.sleep(0.1)
        assert int(daq_bbp.daq_trigger.get_all()["logic"]) == int(logic)
    finally:
        try:
            daq_bbp.daq_trigger.set_logic(DaqTrigLogic(int(original)))
        except Exception:
            pass


def test_trigger_arm_and_disarm_roundtrip(daq_bbp):
    try:
        daq_bbp.daq_trigger.arm(True, pre_samples=1024)
        time.sleep(0.2)
        assert daq_bbp.daq_trigger.status()["armed"] is True, "arm not reflected"
    finally:
        daq_bbp.daq_trigger.arm(False)
    time.sleep(0.2)
    assert daq_bbp.daq_trigger.status()["armed"] is False, "disarm not reflected"


# ---------------------------------------------------------------------------
# Cross-layer: S3 trigger engine -> HAT UART -> P4 -> USB stream
# ---------------------------------------------------------------------------

@pytest.mark.slow
def test_flag_event_reaches_the_p4_stream_as_a_marker(daq_bbp, daq_safe):
    """Configure an S3 IO as a FLAG, drive it, and expect a MARKER on the stream.

    Traverses every hop in the trigger feature: S3 event engine ->
    HAT_CMD_DAQ_MARK 0x5C over the HAT UART -> P4 marker emission -> USB
    stream. Also the only automated check that a marker is stamped with the
    LIVE fused-sample index rather than a stale one.

    This used to be unreachable, and that was proven rather than suspected:

      - daq_trigger_poll_digital() (Firmware/ESP32/src/dio/daq_trigger.cpp:162)
        reads all_dio[i].input_level to detect edges.
      - dio_poll_inputs() (Firmware/ESP32/src/dio/dio.cpp:165) only refreshed
        input_level when mode == DIO_MODE_INPUT.
      - Outputs were configured GPIO_MODE_OUTPUT, not GPIO_MODE_INPUT_OUTPUT
        (Firmware/ESP32/src/dio/dio.cpp:101), so a driven pin's own level was
        never read back into input_level.

    Read-back has now landed: DIO_MODE_OUTPUT uses GPIO_MODE_INPUT_OUTPUT and
    dio_poll_inputs() refreshes input_level for OUTPUT pins too, so an S3 IO
    the S3 itself drives is visible to its own trigger/flag engine. This test
    exercises that self-stimulus path end to end for the first time.
    """
    io = 4
    original = daq_bbp.daq_trigger.get_io(io)

    daq_bbp.dio_configure(io, 2)  # 2 = output
    daq_bbp.dio_write(io, False)
    time.sleep(0.2)  # let the baseline (low) settle before FLAG picks it up

    try:
        daq_bbp.daq_trigger.set_io(io, role=DaqTrigRole.FLAG, edge=DaqTrigEdge.RISING)
        time.sleep(0.1)  # first poll after set_io only captures the baseline level

        daq_safe.drain()
        daq_safe.start()
        time.sleep(0.3)

        result = {}

        def _collect():
            result["cap"] = daq_safe.collect(2.5)

        t = threading.Thread(target=_collect)
        t.start()
        time.sleep(1.0)
        daq_bbp.dio_write(io, True)  # rising edge -> S3 trigger engine -> marker
        t.join()

        cap = result["cap"]
        daq_safe.stop()

        assert cap.markers, (
            "no MARKER records received on the USB stream; frames=%r" % (cap.frames,))

        marker = cap.markers[-1]
        assert marker.channel == io, (
            "marker channel %d != driven IO %d" % (marker.channel, io))
        assert marker.edge == 1, "marker edge %d != rising(1)" % marker.edge

        if cap.wave_i:
            lo = cap.wave_i[0].start_index
            hi = cap.wave_i[-1].start_index + cap.wave_i[-1].count
            # Generous upper margin: the marker can arrive slightly after the
            # last WAVE_I block we happened to capture, but it must not be 0
            # or wildly out of range -- that would mean a stale/uninitialized
            # index rather than the live fused-sample index.
            assert lo <= marker.sample_index <= hi + 200000, (
                "marker sample_index %d outside observed WAVE_I index range "
                "[%d, %d] -- looks stale" % (marker.sample_index, lo, hi))
    finally:
        daq_bbp.dio_write(io, False)
        try:
            daq_bbp.daq_trigger.set_io(
                io, role=DaqTrigRole(int(original["role"])),
                edge=DaqTrigEdge(int(original["edge"])))
        except Exception:
            pass
        daq_bbp.dio_configure(io, 0)  # 0 = disabled / high-impedance
